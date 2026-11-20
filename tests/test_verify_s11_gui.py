from contextlib import ExitStack, nullcontext
import hashlib
import json
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import unittest
from unittest.mock import Mock, patch


@unittest.skipUnless(sys.platform == "win32", "S11 GUI verification uses Windows controls")
class S11GuiVerificationTests(unittest.TestCase):
    def test_fast_only_retries_fail_with_observations_and_remove_stale_success(self):
        from tools import verify_s11_gui as gui

        with TemporaryDirectory() as directory, ExitStack() as stack:
            base = Path(directory)
            exe = base / "app.exe"
            for path in (exe, base / "baseline.json", base / "defect.png"):
                path.touch()
            source = base / "two-stage.png"
            source.write_bytes(b"two-stage input")
            settings = b'{"budget_us": 132, "fast_pass_max": 0.25}'
            (base / "selective.json").write_bytes(settings)
            (base / "freeze.json").write_text(json.dumps({
                "config_sha256": {"selective": hashlib.sha256(settings).hexdigest()},
            }), encoding="utf-8")
            out = base / "output"
            out.mkdir()
            stale = (out / "verification.json", out / "04-frozen-two-stage.png")
            for path in stale:
                path.touch()

            def query(sql):
                if "AND config_sha256" not in sql:
                    return []
                return [next(files)]

            attempts = [f"file-{number}" for number in range(5)]
            files = iter([["main-file", "REVIEW"]] + [[identifier, "REVIEW"] for identifier in attempts])
            file_stage = [["FAST", "0.1", "PASS", "fast_threshold", "0.132458"]]
            tcp_stage = [["FAST", "0.1", "PASS", "fast_threshold", "0.132001"]]
            review_stage = [["FAST", "0.5", "REVIEW", "fast_uncertain", "0.2"]]
            records = Mock(side_effect=[file_stage, tcp_stage] + [review_stage] * 5)
            selection = []
            labels = {1007: gui.VERDICTS["REVIEW"], 1009: "완료", 1010: "빠른 분석 시간 예산 초과"}

            def completed_request(request_id, config):
                if config == "selective-v1":
                    return ["RESULT", request_id, "tcp-selective", "REVIEW"], 0
                self.fail("Verification continued after all five retries missed PRECISE")

            mocks = dict(
                read_database=Mock(return_value=query),
                app_window=Mock(side_effect=lambda *_: nullcontext((None, 1))),
                choose_file=Mock(side_effect=lambda _, path: selection.append(path)),
                open_picker=Mock(),
                u=Mock(),
                wait_for=Mock(),
                send=Mock(),
                control_text=lambda _, identifier: labels.get(identifier, ""),
                check_record=records,
                capture=Mock(),
                select_first_history_row=Mock(),
                completed_request=completed_request,
            )
            stack.enter_context(patch.multiple(gui, **mocks))
            with self.assertRaisesRegex(AssertionError, "PRECISE was never reached") as failure:
                gui.verify(exe, out, False, source, None)
            self.assertEqual(selection, [base / "defect.png"] + [source] * 5)
            self.assertTrue(all(identifier in str(failure.exception) for identifier in attempts))
            self.assertTrue(all(not path.exists() for path in stale))


if __name__ == "__main__":
    unittest.main()
