"""Prepare the verified OPENZDM folder and check actual MFC folder inspections."""

import argparse
from collections import Counter
from contextlib import nullcontext
import json
import os
from pathlib import Path
import re
from shutil import copyfile
import subprocess
from tempfile import TemporaryDirectory
import time
from zipfile import ZipFile

from tools.evaluate_s11 import checksum, match_archive, materialize, read_csv, read_json, write_json


EVIDENCE = Path("docs/steps/S02-evidence")
ARCHIVE = Path("data/openzdm/Weld Aesthetical Defects.zip")
SPLITS = {"train": 43169, "validation": 9250, "test": 9251}
FILTERS = {"PASS": 1206, "FAIL": 1207, "REVIEW": 1208, "ERROR": 1209, "CANCELLED": 1210}


def read_database():
    if os.environ["WELDSCOPE_MYSQL_HOST"] != "127.0.0.1":
        raise ValueError("Use a dedicated loopback MySQL instance")
    client = Path(".tools/mysql-8.4.13-winx64/bin/mysql.exe").resolve(strict=True)
    args = [str(client), "--no-defaults", "--protocol=TCP", "-h", "127.0.0.1",
            "-P", os.environ["WELDSCOPE_MYSQL_PORT"], "-u", "root", "--batch", "--raw",
            "--skip-column-names", "--default-character-set=utf8mb4", os.environ["WELDSCOPE_MYSQL_DATABASE"]]

    def query(sql):
        result = subprocess.run(args, input=sql, capture_output=True, check=True, encoding="utf-8", timeout=90)
        return [line.split("\t") for line in result.stdout.splitlines()]

    return query


def official_rows(evidence):
    rows = read_csv(evidence / "manifest.csv")
    counts = Counter(row["split"] for row in rows if row["status"] == "valid")
    if counts != SPLITS or len(rows) != sum(SPLITS.values()):
        raise ValueError(f"Unexpected OPENZDM manifest coverage: {counts}")
    return rows


def prepare_folder(archive_path, evidence, output):
    if output.exists() or output.is_symlink():
        raise FileExistsError(output)
    rows = official_rows(evidence)
    match_archive(archive_path, read_json(evidence / "archive-checksum.json")["sha256"])
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix=".s13-input-", dir=output.parent) as temporary, ZipFile(archive_path) as archive:
        root = Path(temporary) / "images"
        for prefix in ("train", "val", "test"):
            selected = [{"path": row["path"], "sha256": row["file_sha256"]}
                        for row in rows if row["path"].startswith(prefix + "/")]
            materialize(archive, selected, root / prefix)
        root.rename(output)
    print(f"Prepared {len(rows)} verified PNGs at {output}", flush=True)


def expected_folder(root, evidence):
    rows = official_rows(evidence)
    expected = {row["path"]: row["file_sha256"] for row in rows}
    paths = {path.relative_to(root).as_posix() for path in root.rglob("*")
             if path.is_file() and path.suffix.lower() == ".png"}
    if paths != set(expected):
        raise ValueError(f"Prepared folder differs from manifest: {len(paths)} PNGs")
    return expected


def prepare_fixture(app, demo_image, destination):
    fixture_app = destination / "app"
    fixture_app.mkdir()
    for path in (app, app.with_name("baseline.json"), app.with_name("defect.png"), *app.parent.glob("*.dll")):
        copyfile(path, fixture_app / path.name)
    settings = dict(schema_version=1, contrast_threshold=12, budget_us=2_000_000,
                    fast_pass_max=.11, fast_fail_min=.28, precise_pass_max=0, precise_fail_min=1)
    config = fixture_app / "selective.json"
    write_json(config, settings)
    write_json(fixture_app / "freeze.json", dict(schema_version=1, config_sha256={"selective": checksum(config)}))
    folder = destination / "시연 입력"
    samples = Path("src/WeldScope.App/res/samples")
    fixtures = {"정상/같음.png": (samples / "normal.png", "PASS"),
                "불량/샘플.PNG": (demo_image, "FAIL"),
                "검토/같음.png": (samples / "defect.png", "REVIEW"),
                "오류/손상.png": (samples / "corrupt.png", "ERROR")}
    for name, (source, _) in fixtures.items():
        target = folder / name
        target.parent.mkdir(parents=True, exist_ok=True)
        copyfile(source, target)
    (folder / "설명.txt").write_text("Unsupported file", encoding="utf-8")
    return fixture_app / app.name, folder, fixtures


def verify_folder(app, root, output, evidence, fixture_image, timeout):
    import ctypes as c

    from tools.verify_s09 import capture, find_window, send, u, wait_for
    from tools.verify_s10_gui import app_window, choose_file, control_text, select_first_history_row
    from tools.verify_s10_integration import fault_proxy, request, wait_until
    app = app.resolve(strict=True)
    output = output.resolve()
    output.mkdir(parents=True)
    if fixture_image:
        app, root, fixtures = prepare_fixture(app, fixture_image.resolve(strict=True), output)
        expected = {name: checksum(source) for name, (source, _) in fixtures.items()}
    else:
        root = root.resolve(strict=True)
        expected = expected_folder(root, evidence)
    total = len(expected)
    query = read_database()
    existing = {row[0] for row in query("SELECT CAST(batch_id AS CHAR) FROM inspection_batches;")}
    env = dict(os.environ, WELDSCOPE_DEVICE_PORT="0")
    u.SetProcessDpiAwarenessContext(c.c_void_p(-4))

    def enter_folder_tab(hwnd):
        tab = u.GetDlgItem(hwnd, 1200)
        picker = u.GetDlgItem(hwnd, 1201)
        for x in range(85, 226, 12):
            if u.IsWindowVisible(picker):
                return
            position = (12 << 16) | x
            send(tab, 0x201, 1, position)
            send(tab, 0x202, 0, position)
        wait_for(lambda: u.IsWindowVisible(picker), "Folder tab did not open")

    def select_folder(process, hwnd, path, total):
        u.PostMessageW(hwnd, 0x111, 1201, u.GetDlgItem(hwnd, 1201))
        picker = wait_for(lambda: find_window(process.pid, lambda window: u.GetWindow(window, 4) == hwnd),
                          "Native folder picker missing")
        choose_file(picker, path)
        # Explorer can navigate to the typed folder on the first OK.
        time.sleep(.4)
        if u.IsWindowVisible(picker):
            u.PostMessageW(picker, 0x111, 1, u.GetDlgItem(picker, 1))
        button = u.GetDlgItem(hwnd, 1101)
        try:
            wait_for(lambda: control_text(hwnd, 1202) == str(path) and u.IsWindowEnabled(button),
                     "Folder contents were not accepted")
        except AssertionError as error:
            capture(picker if u.IsWindowVisible(picker) else hwnd, output / "folder-selection-error.png")
            raise AssertionError((str(path), control_text(hwnd, 1202), control_text(hwnd, 1204))) from error
        assert f"전체 {total}" in control_text(hwnd, 1203), control_text(hwnd, 1203)
        return button

    proxy = (fault_proxy(int(env["WELDSCOPE_MYSQL_PORT"]), b"COMMIT", True, after=b"START TRANSACTION")
             if fixture_image else nullcontext((env["WELDSCOPE_MYSQL_PORT"], None)))
    retried = False
    with proxy as (proxy_port, fault), app_window(
            app, output, dict(env, WELDSCOPE_MYSQL_PORT=str(proxy_port))) as (process, hwnd):
        def label(identifier):
            return control_text(hwnd, identifier)

        wait_for(lambda: label(1106) == "DB 연결", "App DB unavailable")
        enter_folder_tab(hwnd)
        start_button = select_folder(process, hwnd, root, total)
        capture(hwnd, output / "01-folder-ready.png")
        tcp_port = int(wait_for(lambda: re.search(r"127\.0\.0\.1:(\d+)", label(1107)),
                                "Loopback listener unavailable")[1])
        began = time.monotonic()
        send(start_button, 0xF5)
        progress_capture = False
        previous = ""
        while label(1204) != "검사 완료":
            status = label(1204)
            progress = label(1203)
            if fixture_image and not retried and label(1106) == "저장 확인 필요":
                assert fault.is_set()
                capture(hwnd, output / "02-storage-retry.png")
                send(u.GetDlgItem(hwnd, 1108), 0xF5)
                retried = True
            if status in {"검사 중지", "검사 중단"} or time.monotonic() - began > timeout:
                raise AssertionError((status, progress, "Folder run did not complete"))
            if not progress_capture and status == "검사 중":
                capture(hwnd, output / "02-in-progress.png")
                progress_capture = True
                if not fixture_image:
                    assert request(tcp_port, "s13-busy-check")[-1] == "BUSY"
            if time.monotonic() - began >= 15 and progress != previous:
                print(progress, flush=True)
                previous = progress
            time.sleep(.2 if fixture_image else 2)
        elapsed = time.monotonic() - began
        assert not fixture_image or retried, "Commit reply loss did not reach the retry path"
        assert f"저장 완료 {total} / 전체 {total}" == label(1203), label(1203)
        capture(hwnd, output / "03-complete.png")

        [batch] = [row for row in query(
            "SELECT CAST(batch_id AS CHAR),state,total_files FROM inspection_batches;") if row[0] not in existing]
        batch_id, state, persisted_total = batch
        assert state == "completed" and int(persisted_total) == total, batch
        assert re.fullmatch(r"[0-9A-Fa-f-]{36}", batch_id), batch_id
        records = query(
            "SELECT i.source_path,i.source_sha256,COALESCE(i.decision,'CANCELLED'),i.reason "
            "FROM inspection_batch_members bm JOIN inspections i USING (inspection_id) "
            f"WHERE bm.batch_id='{batch_id}' ORDER BY bm.sequence_no;")
        counts, reasons, paths = Counter(), Counter(), {}
        for absolute, digest, verdict, reason in records:
            path = Path(absolute).relative_to(root).as_posix()
            assert path not in paths, path
            paths[path] = digest
            counts[verdict] += 1
            reasons[reason] += 1
            if fixture_image:
                assert verdict == fixtures[path][1], (path, verdict, reason)
        if fixture_image:
            # A corrupt PNG can be hashed before decode even when validation fails.
            assert paths.keys() == expected.keys() and all(paths[p] == expected[p] for p in paths if p != "오류/손상.png")
        else:
            assert paths == expected, (len(paths), len(expected), "Paths or hashes differ")
        for verdict, control in FILTERS.items():
            assert label(control).endswith(f" {counts[verdict]}"), (verdict, label(control), counts)
            send(u.GetDlgItem(hwnd, control), 0xF5)
            wait_for(lambda: send(u.GetDlgItem(hwnd, 1211), 0x1004) == min(100, counts[verdict]),
                     f"{verdict} filter row count")
        if fixture_image:
            send(u.GetDlgItem(hwnd, FILTERS["PASS"]), 0xF5)
            wait_for(lambda: send(u.GetDlgItem(hwnd, 1211), 0x1004) == 1, "PASS row missing")
            select_first_history_row(u.GetDlgItem(hwnd, 1211))
            send(u.GetDlgItem(hwnd, 1216), 0xF5)
            wait_for(lambda: "빠른 분석 기준" in label(1010), "Folder detail omitted the final reason")
            capture(hwnd, output / "04-row-detail.png")

        nonce = str(time.time_ns())
        stored = wait_until(lambda: request(tcp_port, "s13-" + nonce), lambda frame: frame[0] == "RESULT")
        assert request(tcp_port, "s13-" + nonce) == stored, stored

    stopped = False
    single_file_completed = False
    with app_window(app, output, env) as (process, hwnd):
        wait_for(lambda: control_text(hwnd, 1106) == "DB 연결", "DB unavailable after restart")
        enter_folder_tab(hwnd)
        wait_for(lambda: control_text(hwnd, 1203) == f"저장 완료 {total} / 전체 {total}",
                 "Completed folder run was not restored")
        capture(hwnd, output / "05-reopened.png")
        if fixture_image:
            stop_root = output / "중지 입력"
            stop_root.mkdir()
            for i in range(300):
                copyfile(fixture_image, stop_root / f"{i:03}.png")
            before_stop = {row[0] for row in query("SELECT CAST(batch_id AS CHAR) FROM inspection_batches;")}
            send(select_folder(process, hwnd, stop_root, 300), 0xF5)
            stop = u.GetDlgItem(hwnd, 1102)
            wait_for(lambda: u.IsWindowEnabled(stop), "Folder stop was not available")
            send(stop, 0xF5)
            wait_for(lambda: control_text(hwnd, 1204) == "검사 중지", "Folder was not stopped")
            [stop_batch] = [row for row in query(
                "SELECT CAST(b.batch_id AS CHAR),b.state,b.total_files,COUNT(m.inspection_id) "
                "FROM inspection_batches b LEFT JOIN inspection_batch_members m USING (batch_id) "
                "GROUP BY b.batch_id;") if row[0] not in before_stop]
            _, state, stop_total, saved = stop_batch
            assert state == "stopped" and int(saved) < int(stop_total) == 300, stop_batch
            wait_for(lambda: f"미처리 {300 - int(saved)}" in control_text(hwnd, 1217),
                     "Unprocessed count does not match the database")
            capture(hwnd, output / "06-stopped.png")
            stopped = True
            single = output / "단일 입력"
            single.mkdir()
            copyfile(fixture_image, single / "one.png")
            send(select_folder(process, hwnd, single, 1), 0xF5)
            wait_for(lambda: control_text(hwnd, 1204) == "검사 완료", "One-file folder did not show completion")
            assert control_text(hwnd, 1203) == "저장 완료 1 / 전체 1"
            single_file_completed = True
    timing = query(
        "SELECT a.stage,COUNT(*),ROUND(AVG(a.duration_ms)*1000,2),ROUND(MAX(a.duration_ms)*1000,2) "
        "FROM inspection_batch_members b JOIN analysis_attempts a USING(inspection_id) "
        f"WHERE b.batch_id='{batch_id}' GROUP BY a.stage;")
    report = dict(batch_id=batch_id, files=total, elapsed_seconds=elapsed,
                  counts={verdict: counts[verdict] for verdict in FILTERS}, reasons=dict(reasons),
                  timings_us={stage: dict(n=int(n), mean=float(mean), maximum=float(maximum))
                              for stage, n, mean, maximum in timing},
                  source_paths_verified=len(paths), reopened=True, tcp_replay=True, commit_retry=retried,
                  stopped=stopped, single_file_completed=single_file_completed)
    (output / "verification.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(report, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)
    prepare = subcommands.add_parser("prepare")
    prepare.add_argument("--archive", type=Path, default=ARCHIVE)
    prepare.add_argument("--evidence", type=Path, default=EVIDENCE)
    prepare.add_argument("--output", type=Path, required=True)
    verify = subcommands.add_parser("verify")
    verify.add_argument("--app", type=Path, required=True)
    verify.add_argument("--output", type=Path, required=True)
    inputs = verify.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--input", type=Path)
    inputs.add_argument("--fixture-image", type=Path, help="Use small fixed inputs and a known train PNG for FAIL")
    verify.add_argument("--evidence", type=Path, default=EVIDENCE)
    verify.add_argument("--timeout", type=int, default=3600)
    args = parser.parse_args()
    if args.command == "prepare":
        prepare_folder(args.archive, args.evidence, args.output)
    else:
        verify_folder(args.app, args.input, args.output, args.evidence, args.fixture_image, args.timeout)


if __name__ == "__main__":
    main()
