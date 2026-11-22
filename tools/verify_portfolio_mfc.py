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

from tools.mfc_test_support import (
    VERDICTS, app_window, capture, check_record, choose_file, completed_request, control_text,
    open_picker, read_database, request, select_first_history_row, send, u, wait_for,
)


REGISTRATION = "pooled-smooth-220-v2"
VERSION = "openzdm-pooled-smooth-220-v2"
MANIFEST = "pooled-smooth-220.json"
MODEL = "model-220.txt"
REFERENCE = {"defect.png": (0.7688432614002394, "FAIL"),
             "normal.png": (0.010489226697190355, "PASS"),
             "image_1.png": (0.27026603970784652, "REVIEW")}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(exe, out, repeats, legacy_exe=None, review_image=None):
    if repeats < 1:
        raise ValueError("Expected at least one observation per input")
    exe, out = exe.resolve(strict=True), out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    settings = exe.with_name(MANIFEST)
    config_hash = digest(settings)
    config = json.loads(settings.read_bytes())
    sources = {"defect.png": exe.with_name("defect.png"),
               "normal.png": Path("src/WeldScope.App/res/samples/normal.png").resolve(strict=True)}
    if review_image:
        sources["image_1.png"] = review_image.resolve(strict=True)
    query = read_database()
    u.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
    observations = []
    prefix = "research-" + uuid.uuid4().hex

    def check(inspection_id, source, decision):
        trace = check_record(query, inspection_id, config_hash, digest(source), decision, VERSION, include_time=True)
        assert len(trace) == 1 and trace[0][0] == "LIGHTGBM", trace
        stage, score, band_decision, _, elapsed = trace[0]
        reference_score, expected = REFERENCE[source.name]
        assert float(score) == reference_score, (source, score)
        assert decision == band_decision == expected, trace
        reason, = query(f"SELECT reason FROM inspections WHERE inspection_id='{inspection_id}';")[0]
        assert reason == ("score_between_limits" if expected == "REVIEW" else "score_threshold"), reason
        return dict(inspection_id=inspection_id, sha256=digest(source), decision=decision,
                    reason=reason, stage=stage, score=float(score), analysis_ms=float(elapsed))

    legacy = {}
    if legacy_exe:
        with app_window(legacy_exe.resolve(strict=True), out) as (_, old_window):
            wait_for(lambda: control_text(old_window, 1106) == "DB 연결", "Legacy app not ready")
            for registration in ("baseline-v1", "selective-v1", "pooled-smooth-220-v1"):
                old_request_id = prefix + "-" + registration
                frame, _ = completed_request(old_request_id, registration)
                legacy[registration] = frame

    with app_window(exe, out) as (process, window):
        def label(identifier):
            return control_text(window, identifier)

        wait_for(lambda: label(1106) == "DB 연결" and label(1002) == "LightGBM", "Portfolio model unavailable")
        for index, name in enumerate(tuple(sources) * repeats):
            path = sources[name]
            before = {r[0] for r in query("SELECT inspection_id FROM inspections WHERE request_id LIKE 'file-%';")}
            choose_file(open_picker(process, window), path)
            start = u.GetDlgItem(window, 1101)
            wait_for(lambda: label(1005) == str(path) and label(1009) == "준비" and u.IsWindowEnabled(start),
                     "File not ready")
            started = time.perf_counter()
            send(start, 0xF5)
            wait_for(lambda: label(1106) == "저장 완료" and "판정 점수" in label(1010), "File inspection not displayed")
            elapsed_ms = (time.perf_counter() - started) * 1000
            rows = query("SELECT inspection_id,decision FROM inspections WHERE request_id LIKE 'file-%' "
                         f"AND source_sha256='{digest(path)}' AND config_sha256='{config_hash}';")
            [(inspection_id, decision)] = [row for row in rows if row[0] not in before]
            record = check(inspection_id, path, decision)
            assert label(1007) == VERDICTS[decision]
            observations.append(dict(record, input="file", filename=name, observation=index//len(sources)+1,
                                     click_to_saved_ms=elapsed_ms))
            if decision == "REVIEW":
                assert "판정 기준 사이" in label(1010)
            assert "분석 시간" in label(1010) and not u.GetDlgItem(window, 1103)
            if index < len(sources):
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
        wait_for(lambda: "판정 점수" in control_text(window, 1010) and control_text(window, 1004) == "",
                 "Could not reopen candidate history")
        assert control_text(window, 1007) == VERDICTS[frame[-1]]
        capture(window, out / "history-after-restart.png")
        assert request(33361, request_id, config=REGISTRATION) == frame
        assert request(33361, request_id, config="selective-v1")[-1] == "REQUEST_CONFLICT"
        assert query(f"SELECT COUNT(*) FROM inspections WHERE request_id='{request_id}';") == [["1"]]
        for registration, old_frame in legacy.items():
            assert request(33361, old_frame[1], config=registration) == old_frame
        # A cancelled pre-upgrade record has no analysis attempts. Its request must still replay.
        old_request, old_id = prefix + "-old", str(uuid.uuid4())
        query("INSERT INTO inspections (inspection_id,request_id,input_id,source_path,config_sha256,"
              "algorithm_version,dataset_version,work_state,reason) VALUES "
              f"('{old_id}','{old_request}','weld-01','defect.png',"
              "'9ca2e425b9ccbf7fb15066040bfec8b802bf1beed9716a16ce861da31b3fcf3b',"
              "'openzdm-pooled-400-v1','openzdm-14025630','CANCELLED','cancelled');")
        assert request(33361, old_request, config="pooled-400-v1") == ["STATUS", old_request, old_id, "CANCELLED"]
        assert request(33361, old_request, config=REGISTRATION)[-1] == "REQUEST_CONFLICT"
        for retired in ("baseline-v1", "selective-v1", "pooled-smooth-220-v1", "spatial-48-v1",
                        "pooled-400-v1", "pooled-width-220-v1"):
            response = request(33361, "new-" + uuid.uuid4().hex, config=retired)
            assert response[-1] == "CONFIG_UNAVAILABLE", (retired, response)
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
    parser.add_argument("--legacy-executable", type=Path)
    parser.add_argument("--review-image", type=Path)
    args = parser.parse_args()
    verify(args.executable, args.output, args.repeats, args.legacy_executable, args.review_image)


if __name__ == "__main__":
    main()
