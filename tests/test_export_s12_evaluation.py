import csv
import hashlib
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from zipfile import ZipFile

from tools.evaluate_s11 import MODES, checksum, test_comparisons as compare_results, write_json
from tools.export_s12_evaluation import export_evaluation


class EvaluationExportTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.evaluation = self.root / "evaluation"
        inputs = self.evaluation / "inputs"
        inputs.mkdir(parents=True)
        self.inspect = self.root / "inspect.exe"
        self.calibrator = self.root / "calibrator.exe"
        self.output = self.root / "evaluation.zip"
        self.replay = self.root / "replay"
        self.inspect.write_bytes(b"historical inspector")
        self.calibrator.write_bytes(b"historical calibrator")
        self.notice = self.root / "notices.md"
        self.notice.write_text("local test notice", encoding="utf-8")
        self.row = dict(path="test/OK/image.png", split="test", label="OK", sha256="a" * 64)
        for filename, rows in (("validation.json", []), ("test.json", [self.row]),
                               ("test-primary.json", [self.row]), ("excluded.json", [])):
            write_json(inputs / filename, {"rows": rows})
        manifest_hashes = {path.name: checksum(path) for path in inputs.iterdir()}
        write_json(inputs / "provenance.json", dict(sha256=manifest_hashes, source_archive_sha256="b" * 64))
        validation = self.evaluation / "validation"
        validation.mkdir()
        write_json(validation / "selection.json", {"status": "accepted"})
        configs = self.evaluation / "config"
        configs.mkdir()
        for mode in MODES:
            write_json(configs / f"{mode}.json", {"mode": mode})
        self.freeze = self.evaluation / "freeze.json"
        write_json(self.freeze, dict(
            schema_version=1, executable_sha256=checksum(self.inspect), calibrator_sha256=checksum(self.calibrator),
            test_manifest_sha256=manifest_hashes["test.json"], test_primary_sha256=manifest_hashes["test-primary.json"],
            validation_manifest_sha256=manifest_hashes["validation.json"], excluded_sha256=manifest_hashes["excluded.json"],
            provenance_sha256=checksum(inputs / "provenance.json"), source_archive_sha256="b" * 64,
            validation_selection_sha256=checksum(validation / "selection.json"),
            config_sha256={mode: checksum(configs / f"{mode}.json") for mode in MODES},
            algorithm_version={mode: f"{mode}-test" for mode in MODES}, environment={"os": "historical fixture"}))
        for results in (self.evaluation / "test", self.replay):
            self.write_results(results)

    def write_results(self, results):
        row = dict(source_path=self.row["path"], split="test", label="OK", image_sha256=self.row["sha256"],
                   decision="PASS", reason="fixture", width="4", height="3", analysis_ms="0.1",
                   load_ms="0.3", precise_elapsed_us="")
        for repetition in (1, 2):
            for mode in MODES:
                directory = results / f"run-{repetition:02d}" / mode
                directory.mkdir(parents=True)
                with (directory / "results.csv").open("w", encoding="utf-8", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(row))
                    writer.writeheader()
                    writer.writerow(row)
                write_json(directory / "summary.json", dict(
                    scope="test-frozen", evaluation_split="test", algorithm_version=f"{mode}-test",
                    requested=1, PASS=1, FAIL=0, ERROR=0, REVIEW=0, input_manifest_valid=1, threads=1,
                    manifest_sha256=checksum(self.evaluation / "inputs" / "test.json"),
                    config_sha256=checksum(self.evaluation / "config" / f"{mode}.json")))
        measurements = [{mode: [row] for mode in MODES} for _ in range(2)]
        write_json(results / "report.json",
                   dict(freeze_sha256=checksum(self.freeze), cohorts=compare_results(measurements, {self.row["path"]})))

    def export(self, replay=True):
        with patch("tools.export_s12_evaluation.NOTICE_FILES", {"THIRD-PARTY-NOTICES.md": self.notice}):
            return export_evaluation(self.evaluation, self.inspect, self.calibrator, self.output,
                                     self.replay if replay else None)

    def test_exports_allowlist_and_hashes_without_matching_host(self):
        (self.evaluation / "unrelated.txt").write_text("not included", encoding="utf-8")
        self.export()
        with ZipFile(self.output) as archive:
            expected = {"evaluation/freeze.json", "evaluation/inputs/test.json", "evaluation/test/report.json",
                        "evaluation/test/run-02/selective/results.csv", "replay/run-01/fast/summary.json",
                        "original/weldscope_inspect.exe", "original/weldscope_calibrate_s11.exe",
                        "THIRD-PARTY-NOTICES.md", "SHA256SUMS.txt"}
            self.assertTrue(expected <= set(archive.namelist()))
            self.assertEqual(len(archive.namelist()), 49)
            self.assertNotIn("evaluation/unrelated.txt", archive.namelist())
            for line in archive.read("SHA256SUMS.txt").decode("ascii").splitlines():
                digest, name = line.split("  ")
                self.assertEqual(hashlib.sha256(archive.read(name)).hexdigest(), digest)
        original = checksum(self.output)
        with self.assertRaises(FileExistsError):
            self.export()
        self.assertEqual(checksum(self.output), original)

    def test_rejects_changed_calibrator_before_publishing(self):
        self.calibrator.write_bytes(b"different executable")
        with self.assertRaisesRegex(ValueError, "calibrator"):
            self.export()
        self.assertFalse(self.output.exists())

    def test_rejects_report_that_disagrees_with_csv(self):
        write_json(self.replay / "report.json", {"freeze_sha256": checksum(self.freeze), "cohorts": {}})
        with self.assertRaisesRegex(ValueError, "does not match"):
            self.export()
        self.assertFalse(self.output.exists())

    def test_replay_is_optional(self):
        self.export(replay=False)
        with ZipFile(self.output) as archive:
            self.assertNotIn("replay/report.json", archive.namelist())


if __name__ == "__main__":
    unittest.main()
