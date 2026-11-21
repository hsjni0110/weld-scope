"""Check the default portfolio inspection model through the native MFC file picker, TCP and MySQL."""

import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import shutil
import tempfile
import time
import uuid

from tools.verify_s09 import capture, send, u, wait_for
from tools.verify_s10_gui import app_window, choose_file, control_text, open_picker, select_first_history_row
from tools.verify_s10_integration import request
from tools.verify_s11_gui import check_record, completed_request, read_database, VERDICTS


REGISTRATION = "pooled-smooth-220-v1"
VERSION = "openzdm-pooled-smooth-220-v1"
MANIFEST = "pooled-smooth-220.json"
MODEL = "model-220.txt"
REFERENCE_SCORES = {"defect.png": 0.7688432614002394, "normal.png": 0.010489226697190355}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(exe, out, repeats):
    if repeats < 1:
        raise ValueError("Expected at least one observation per input")
    exe, out = exe.resolve(strict=True), out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    settings = exe.with_name(MANIFEST)
    config_hash = digest(settings)
    config = json.loads(settings.read_bytes())
    sources = {"defect.png": exe.with_name("defect.png"),
               "normal.png": Path("src/WeldScope.App/res/samples/normal.png").resolve(strict=True)}
    query = read_database()
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    observations = []
    prefix = "research-" + uuid.uuid4().hex

    def check(inspection_id, source, decision):
        trace = check_record(query, inspection_id, config_hash, digest(source), decision, VERSION, include_time=True)
        assert len(trace) == 1 and trace[0][0] == "POOLED_WIDTH", trace
        stage, score, band_decision, _, elapsed = trace[0]
        assert float(score) == REFERENCE_SCORES[source.name], (source, score)
        assert band_decision == ("FAIL" if source.name == "defect.png" else "PASS"), trace
        timed_out = round(float(elapsed) * 1_000_000) > config["budget_us"] * 1000
        expected_reason = "budget_exhausted" if timed_out else "fast_threshold"
        assert decision == ("REVIEW" if timed_out else band_decision)
        reason, = query(f"SELECT reason FROM inspections WHERE inspection_id='{inspection_id}';")[0]
        assert reason == expected_reason, reason
        return dict(inspection_id=inspection_id, sha256=digest(source), decision=decision,
                    reason=reason, stage=stage, score=float(score), analysis_ms=float(elapsed))

    with app_window(exe, out) as (process, window):
        def label(identifier):
            return control_text(window, identifier)

        wait_for(lambda: label(1106) == "DB 연결" and label(1002) == "연속 보간", "Portfolio model unavailable")
        for index, name in enumerate(("normal.png", "defect.png") * repeats):
            path = sources[name]
            before = {r[0] for r in query("SELECT inspection_id FROM inspections WHERE request_id LIKE 'file-%';")}
            choose_file(open_picker(process, window), path)
            start = u.GetDlgItem(window, 1101)
            wait_for(lambda: label(1005) == str(path) and label(1009) == "준비" and u.IsWindowEnabled(start),
                     "File not ready")
            started = time.perf_counter()
            send(start, 0xF5)
            wait_for(lambda: label(1106) == "저장 완료" and "폭별 질감" in label(1010), "File inspection not displayed")
            elapsed_ms = (time.perf_counter() - started) * 1000
            rows = query("SELECT inspection_id,decision FROM inspections WHERE request_id LIKE 'file-%' "
                         f"AND source_sha256='{digest(path)}' AND config_sha256='{config_hash}';")
            [(inspection_id, decision)] = [row for row in rows if row[0] not in before]
            record = check(inspection_id, path, decision)
            assert label(1007) == VERDICTS[decision]
            observations.append(dict(record, input="file", filename=name, observation=index//2+1,
                                     click_to_saved_ms=elapsed_ms))
            if index < 2:
                capture(window, out / ("file-" + name))
        for index in range(repeats):
            request_id = prefix + "-" + str(index)
            frame, elapsed_ms = completed_request(request_id, REGISTRATION)
            observations.append(dict(check(frame[2], sources["defect.png"], frame[-1]), input="TCP",
                                     observation=index+1, frame=frame, polling_ms=elapsed_ms))
        wait_for(lambda: label(1106) == "저장 완료" and label(1007) == VERDICTS[frame[-1]], "TCP verdict missing")
        capture(window, out / "tcp.png")

    with app_window(exe, out) as (_, window):
        wait_for(lambda: control_text(window, 1012) == "최근 저장 이력", "Stored history unavailable")
        select_first_history_row(u.GetDlgItem(window, 1104))
        send(u.GetDlgItem(window, 1110), 0xF5)
        wait_for(lambda: "폭별 질감" in control_text(window, 1010) and control_text(window, 1004) == "",
                 "Could not reopen candidate history")
        assert control_text(window, 1007) == VERDICTS[frame[-1]]
        capture(window, out / "history-after-restart.png")
        assert request(33361, request_id, config=REGISTRATION) == frame
        assert request(33361, request_id, config="selective-v1")[-1] == "REQUEST_CONFLICT"
        assert query(f"SELECT COUNT(*) FROM inspections WHERE request_id='{request_id}';") == [["1"]]
        legacy = {}
        for registration, filename, version in (("baseline-v1", "baseline.json", "global-dark-ratio-v1"),
                                                 ("selective-v1", "selective.json", "local-contrast-selective-v1")):
            result, _ = completed_request(prefix + registration, registration)
            trace = check_record(query, result[2], digest(exe.with_name(filename)), digest(sources["defect.png"]),
                                 result[-1], version, include_time=True)
            legacy[registration] = dict(frame=result, attempts=trace)
        # A cancelled pre-upgrade record has no analysis attempts. Its request must still replay.
        old_request, old_id = prefix + "-old", str(uuid.uuid4())
        query("INSERT INTO inspections (inspection_id,request_id,input_id,source_path,config_sha256,"
              "algorithm_version,dataset_version,work_state,reason) VALUES "
              f"('{old_id}','{old_request}','weld-01','defect.png',"
              "'9ca2e425b9ccbf7fb15066040bfec8b802bf1beed9716a16ce861da31b3fcf3b',"
              "'openzdm-pooled-400-v1','openzdm-14025630','CANCELLED','cancelled');")
        assert request(33361, old_request, config="pooled-400-v1") == ["STATUS", old_request, old_id, "CANCELLED"]
        assert request(33361, old_request, config=REGISTRATION)[-1] == "REQUEST_CONFLICT"
        for retired in ("spatial-48-v1", "pooled-400-v1", "pooled-width-220-v1"):
            assert request(33361, prefix + "-" + retired, config=retired)[-1] == "CONFIG_UNAVAILABLE"
    with tempfile.TemporaryDirectory(dir=out, ignore_cleanup_errors=True) as temporary:
        folder = Path(temporary)
        for path in (exe, *exe.parent.glob("*.dll"), *exe.parent.glob("*.json"),
                     exe.with_name(MODEL), sources["defect.png"]):
            shutil.copyfile(path, folder / path.name)
        with (folder / MODEL).open("ab") as target:
            target.write(b"x")
        with app_window(folder / exe.name, out) as (_, window):
            wait_for(lambda: control_text(window, 1002) == "설정 오류", "Corrupt candidate was accepted")
            assert request(33361, prefix + "-corrupt", config=REGISTRATION)[-1] == "CONFIG_UNAVAILABLE"
            assert request(33361, request_id, config=REGISTRATION) == frame
        (folder / MANIFEST).unlink()
        with app_window(folder / exe.name, out) as (_, window):
            wait_for(lambda: control_text(window, 1002) == "설정 오류", "Missing default manifest silently fell back")
            assert request(33361, prefix + "-absent", config=REGISTRATION)[-1] == "CONFIG_UNAVAILABLE"
    report = dict(executable_sha256=digest(exe), config_sha256=config_hash, settings=config, version=VERSION,
                  observations=observations, legacy=legacy, history_after_restart=True,
                  replay_after_restart=True, replay_rows=1, corrupt_model_rejected=True,
                  replay_with_corrupt_model=True, missing_manifest_rejected=True, retired_request_replayed=True)
    (out / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print("Verified MFC file, TCP, persisted history, replay, pinned models and legacy registrations")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--repeats", type=int, default=3)
    args = parser.parse_args()
    verify(args.executable, args.output, args.repeats)


if __name__ == "__main__":
    main()
