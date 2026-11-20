"""Verify file and TCP outcomes against the x64 MFC app and a dedicated MySQL schema."""

import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory
import time
import uuid

from tools.verify_s09 import capture, send, u, wait_for
from tools.verify_s10_gui import app_window, choose_file, control_text, open_picker, select_first_history_row
from tools.verify_s10_integration import host, request, wait_until


VERDICTS = {"PASS": "정상", "FAIL": "불량", "REVIEW": "검토 필요", "ERROR": "처리 오류"}
STAGE_LABELS = {"FAST": "빠른 분석", "PRECISE": "정밀 분석"}


def read_database():
    if os.environ["WELDSCOPE_MYSQL_HOST"] != "127.0.0.1":
        raise ValueError("Use the dedicated loopback MySQL instance")
    mysql = Path(".tools/mysql-8.4.13-winx64/bin/mysql.exe").resolve(strict=True)
    schema = os.environ["WELDSCOPE_MYSQL_DATABASE"]
    admin = [str(mysql), "--no-defaults", "--protocol=TCP", "--host=127.0.0.1",
             "--port=" + os.environ["WELDSCOPE_MYSQL_PORT"], "--user=root",
             "--batch", "--skip-column-names", schema]

    def query(sql):
        result = subprocess.run(admin, input=sql, text=True, check=True, capture_output=True, timeout=10)
        return [line.split("\t") for line in result.stdout.splitlines()]

    return query


def completed_request(request_id, config_id, port=33361):
    start = time.perf_counter()
    first = wait_until(lambda: request(port, request_id, config=config_id), lambda response: response[-1] != "BUSY")
    assert first[0] == "STATUS" and first[-1] == "IN_PROGRESS", first
    result = wait_until(lambda: request(port, request_id, config=config_id), lambda response: response[0] == "RESULT")
    assert result[2] == first[2], result
    return result, (time.perf_counter() - start) * 1000


def check_record(query, inspection_id, config_hash, image_hash, decision, version, *, include_time=False):
    inspection_id = str(uuid.UUID(inspection_id))
    rows = query("SELECT config_sha256, source_sha256, decision, algorithm_version "
                 f"FROM inspections WHERE inspection_id = '{inspection_id}';")
    assert rows == [[config_hash, image_hash, decision, version]], rows
    stages = query("SELECT stage, score, decision, reason, duration_ms FROM analysis_attempts "
                   f"WHERE inspection_id = '{inspection_id}' ORDER BY sequence_no;")
    assert stages and all(float(row[-1]) >= 0 for row in stages), stages
    return stages if include_time else [row[:-1] for row in stages]


def check_pinned_registration(exe, out, freeze, baseline, selective):
    # Windows can briefly hold copied DLLs after the process has exited.
    with TemporaryDirectory(prefix="registration-", dir=out, ignore_cleanup_errors=True) as folder:
        folder = Path(folder)
        deployed = folder / exe.name
        for file in (exe, exe.with_name("baseline.json"), exe.with_name("defect.png"), *exe.parent.glob("*.dll")):
            shutil.copyfile(file, folder / file.name)

        def verify_mode(label):
            with app_window(deployed, out) as (_, hwnd):
                wait_for(lambda: control_text(hwnd, 1106) == "DB 연결" and control_text(hwnd, 1002) == label,
                         "Unexpected local mode for changed deployment")
                new_id = "s11-ui-" + uuid.uuid4().hex
                assert request(33361, new_id, config="selective-v1")[-1] == "CONFIG_UNAVAILABLE"
                assert request(33361, baseline[1], config="baseline-v1") == baseline
                assert request(33361, selective[1], config="selective-v1") == selective

        verify_mode("설정 오류")
        alternate = json.loads(exe.with_name("selective.json").read_text(encoding="utf8"))
        alternate["budget_us"] += 1
        alternate_bytes = json.dumps(alternate).encode("utf8")
        alternate_freeze = dict(freeze, config_sha256=dict(
            freeze["config_sha256"], selective=hashlib.sha256(alternate_bytes).hexdigest()))
        (folder / "selective.json").write_bytes(alternate_bytes)
        (folder / "freeze.json").write_text(json.dumps(alternate_freeze), encoding="utf8")
        verify_mode("선택적 검사")


def verify(exe, out, review_fixture, two_stage_source, integration_host):
    exe, out = exe.resolve(strict=True), out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    freeze = json.loads(exe.with_name("freeze.json").read_text(encoding="utf8"))
    config_bytes = exe.with_name("selective.json").read_bytes()
    settings = json.loads(config_bytes)
    config_hash = hashlib.sha256(config_bytes).hexdigest()
    assert config_hash == freeze["config_sha256"]["selective"]
    image_path = exe.with_name("defect.png")
    image_hash = hashlib.sha256(image_path.read_bytes()).hexdigest()
    baseline_hash = hashlib.sha256(exe.with_name("baseline.json").read_bytes()).hexdigest()
    query = read_database()
    u.SetProcessDpiAwarenessContext(c.c_void_p(-4))

    prior_files = {row[0] for row in query("SELECT inspection_id FROM inspections WHERE request_id LIKE 'file-%';")}
    selective_id = "s11-ui-" + uuid.uuid4().hex
    baseline_id = "s11-ui-" + uuid.uuid4().hex

    def newest_file(expected_source_hash):
        rows = query("SELECT inspection_id, decision FROM inspections WHERE request_id LIKE 'file-%' "
                     f"AND config_sha256 = '{config_hash}' AND source_sha256 = '{expected_source_hash}' "
                     "ORDER BY created_at DESC LIMIT 1;")
        assert len(rows) == 1 and rows[0][0] not in prior_files, rows
        prior_files.add(rows[0][0])
        return rows[0]

    def inspect_file(process, hwnd, source):
        choose_file(open_picker(process, hwnd), source)
        start = u.GetDlgItem(hwnd, 1101)
        wait_for(lambda: control_text(hwnd, 1005) == str(source) and control_text(hwnd, 1009) == "준비"
                 and u.IsWindowEnabled(start), "Selected file did not become ready")
        send(start, 0xF5)
        wait_for(lambda: control_text(hwnd, 1106) == "저장 완료", "File result was not saved")

    two_stage = selective = None
    timing = {}
    paired_observations = []
    with app_window(exe, out) as (process, hwnd):
        def label(control_id):
            return control_text(hwnd, control_id)

        wait_for(lambda: label(1106) == "DB 연결" and label(1002) == "선택적 검사", "Frozen settings were not installed")
        for _ in range(5):
            inspect_file(process, hwnd, image_path)
            file_id, file_decision = newest_file(image_hash)
            file_trace = check_record(query, file_id, config_hash, image_hash, file_decision,
                                      "local-contrast-selective-v1", include_time=True)
            file_stages = [row[:-1] for row in file_trace]
            assert label(1007) == VERDICTS[file_decision]
            for stage in file_stages:
                assert STAGE_LABELS[stage[0]] in label(1010), stage
            capture(hwnd, out / "01-selective-file.png")
            if review_fixture:
                assert file_decision == "REVIEW" and [stage[0] for stage in file_stages] == ["FAST", "PRECISE"], file_stages
                assert request(33361, selective_id, config="selective-v1")[-1] == "CONFIG_UNAVAILABLE"
                break

            selective_id = "s11-ui-" + uuid.uuid4().hex
            selective, tcp_elapsed_ms = completed_request(selective_id, "selective-v1")
            tcp_trace = check_record(query, selective[2], config_hash, image_hash, selective[-1],
                                     "local-contrast-selective-v1", include_time=True)
            assert len(file_stages) == len(tcp_trace) == 1 and file_stages[0] == tcp_trace[0][:-1]
            assert file_trace[0][0] == "FAST" and float(file_trace[0][1]) <= settings["fast_pass_max"]
            for trace, decision in ((file_trace, file_decision), (tcp_trace, selective[-1])):
                over_budget = round(float(trace[0][-1]) * 1000) > settings["budget_us"]
                assert decision == ("REVIEW" if over_budget else "PASS"), (trace, decision)
            paired_observations.append({
                "file_inspection_id": file_id, "file_decision": file_decision, "file_attempts": file_trace,
                "tcp_inspection_id": selective[2], "tcp_decision": selective[-1], "tcp_attempts": tcp_trace,
            })
            wait_for(lambda: label(1106) == "저장 완료" and label(1007) == VERDICTS[selective[-1]], "TCP outcome missing")
            timing["tcp_client_polling_inclusive_ms"] = tcp_elapsed_ms
            capture(hwnd, out / "02-selective-tcp.png")
            if selective[-1] == file_decision:
                break

        wait_for(lambda: label(1012) == "최근 저장 이력", "Selective history missing")
        list_view = u.GetDlgItem(hwnd, 1104)
        select_first_history_row(list_view)
        send(u.GetDlgItem(hwnd, 1110), 0xF5)
        wait_for(lambda: "빠른 분석" in label(1010) and label(1004) == "", "Selective stage history did not reopen")
        history_decision = selective[-1] if selective else file_decision
        assert label(1007) == VERDICTS[history_decision] and label(1009) == "완료"
        for stage in file_stages:
            assert STAGE_LABELS[stage[0]] in label(1010), stage
        capture(hwnd, out / "03-selective-history.png")

        if two_stage_source:
            source = two_stage_source.resolve(strict=True)
            source_hash = hashlib.sha256(source.read_bytes()).hexdigest()
            observed = []
            for _ in range(5):
                inspect_file(process, hwnd, source)
                two_stage_id, decision = newest_file(source_hash)
                stages = check_record(query, two_stage_id, config_hash, source_hash, decision,
                                      "local-contrast-selective-v1", include_time=True)
                observed.append({"inspection_id": two_stage_id, "decision": decision, "stages": stages})
                assert decision == "REVIEW", observed
                if [item[0] for item in stages] == ["FAST", "PRECISE"]:
                    break
                assert [item[0] for item in stages] == ["FAST"] and "시간 예산 초과" in label(1010), observed
            reached_precise = len(stages) == 2
            assert label(1007) == VERDICTS[decision]
            if reached_precise:
                assert all(name in label(1010) for name in STAGE_LABELS.values())
                capture(hwnd, out / "04-frozen-two-stage.png")
            two_stage = {"inspection_id": two_stage_id, "source_name": source.name,
                         "source_sha256": source_hash, "reached_precise": reached_precise, "observations": observed}

        baseline, _ = completed_request(baseline_id, "baseline-v1")
        assert baseline[-1] == "FAIL"
        assert [row[0] for row in check_record(query, baseline[2], baseline_hash, image_hash, "FAIL",
                                              "global-dark-ratio-v1")] == ["BASELINE"]

    with app_window(exe, out) as (_, hwnd):
        wait_for(lambda: control_text(hwnd, 1106) == "DB 연결", "Restart did not connect")
        if selective:
            assert request(33361, selective_id, config="selective-v1") == selective
        assert request(33361, baseline_id, config="baseline-v1") == baseline
        assert request(33361, baseline_id, config="selective-v1")[-1] == "REQUEST_CONFLICT"

    if review_fixture:
        settings_path = exe.with_name("selective.json")
        original = settings_path.read_bytes()
        try:
            settings_path.write_bytes(original + b"\n")
            with app_window(exe, out) as (process, hwnd):
                wait_for(lambda: control_text(hwnd, 1002) == "설정 오류", "Mismatched settings were not rejected")
                choose_file(open_picker(process, hwnd), image_path)
                wait_for(lambda: control_text(hwnd, 1005) == str(image_path), "File selection disappeared")
                assert not u.IsWindowEnabled(u.GetDlgItem(hwnd, 1101))
                invalid_id = "s11-ui-" + uuid.uuid4().hex
                assert request(33361, invalid_id, config="selective-v1")[-1] == "CONFIG_UNAVAILABLE"
                assert request(33361, baseline_id, config="baseline-v1") == baseline
        finally:
            settings_path.write_bytes(original)
    else:
        check_pinned_registration(exe, out, freeze, baseline, selective)

    if integration_host:
        with host(integration_host.resolve(strict=True), os.environ.copy(), image_path,
                  config=exe.with_name("baseline.json"), device_image=image_path) as (port, command):
            timed, _ = completed_request("s11-timing-" + uuid.uuid4().hex, "selective-v1", port)
            wait_until(lambda: command("state"), lambda state: state.startswith("STORED ") and timed[2] in state)
            save_ms = float(command("save_ms"))
            assert save_ms >= 0
            timing.update({"db_save_including_connection_and_commit_ms": save_ms,
                           "db_timed_inspection_id": timed[2]})

    (out / "verification.json").write_text(json.dumps({
        "review_fixture": review_fixture, "config_sha256": config_hash, "source_sha256": image_hash,
        "file_inspection_id": file_id, "tcp_inspection_id": selective[2] if selective else None,
        "baseline_inspection_id": baseline[2], "pinned_registration": not review_fixture,
        "decision": file_decision, "tcp_decision": selective[-1] if selective else None, "stages": file_stages,
        "replay_after_restart": True, "two_stage": two_stage,
        "timing": timing, "paired_observations": paired_observations,
    }, indent=2) + "\n", encoding="utf8")
    print("S11 MFC file, TCP, MySQL history and replay checks passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--review-fixture", action="store_true")
    parser.add_argument("--two-stage-source", type=Path, help="Train image that enters both stages with the frozen settings")
    parser.add_argument("--integration-host", type=Path, help="Optional coordinator test host for a separate DB save time")
    args = parser.parse_args()
    verify(args.executable, args.output, args.review_fixture, args.two_stage_source, args.integration_host)
