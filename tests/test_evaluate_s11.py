import hashlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile

from tools.evaluate_s11 import (checksum, confirm_ranked, evaluate_metrics, evaluate_test, highest_threshold,
                                make_cohorts, materialize, nearest_rank, run_candidate, validate, write_json)


class EvaluationPreparationTests(unittest.TestCase):
    def row(self, path, split, dhash, data=b"image"):
        return dict(path=path, split=split, label=path.split("/")[1], dhash=f"{dhash:016x}",
                    file_sha256=hashlib.sha256(data).hexdigest(), status="valid")

    def test_transitive_cross_split_and_identical_hash_groups(self):
        rows = [
            self.row("train/OK/a.png", "train", 1), self.row("train/OK/b.png", "train", 3),
            self.row("val/OK/c.png", "validation", 7), self.row("val/KO/d.png", "validation", 15),
            self.row("val/OK/keep.png", "validation", 31),
            self.row("test/KO/e.png", "test", 15), self.row("test/OK/keep.png", "test", 63),
        ]
        pairs = [(f"{a:016x}", f"{b:016x}", (a ^ b).bit_count()) for a, b in [(1, 3), (3, 7)]]
        cohorts, excluded = make_cohorts(rows, pairs)
        self.assertEqual([r["path"] for r in cohorts["validation"]], ["val/OK/keep.png"])
        self.assertEqual([r["path"] for r in cohorts["test-primary"]], ["test/OK/keep.png"])
        self.assertEqual(len(cohorts["test"]), 2)
        self.assertEqual({r["path"] for r in excluded}, {"val/OK/c.png", "val/KO/d.png", "test/KO/e.png"})

    def test_path_mapping_and_materialization_hash_guard(self):
        rows = [self.row("test/OK/a.png", "test", 2)]
        with self.assertRaises(ValueError):
            make_cohorts([self.row("test/OK/a.png", "validation", 2)], [])
        with self.assertRaises(ValueError):
            make_cohorts([self.row("test/OK/../a.png", "test", 2)], [])
        with self.assertRaises(ValueError):
            make_cohorts(rows + [self.row("test/OK/A.png", "test", 3)], [])
        image = io.BytesIO()
        with ZipFile(image, "w") as archive:
            archive.writestr("test/OK/a.png", b"different")
        image.seek(0)
        with tempfile.TemporaryDirectory() as directory, ZipFile(image) as archive:
            with self.assertRaises(ValueError):
                materialize(archive, [{"path": rows[0]["path"], "sha256": rows[0]["file_sha256"]}], Path(directory) / "test")
            self.assertFalse((Path(directory) / "test").exists())

    def test_rates_include_review_and_error_and_split_timing_by_size(self):
        def row(label, decision, width, precise=""):
            return dict(label=label, decision=decision, width=str(width), height="22", analysis_ms="0.2",
                        load_ms="0.1", precise_elapsed_us=precise)
        measured = evaluate_metrics([
            row("KO", "PASS", 40), row("KO", "REVIEW", 40, "0"), row("KO", "ERROR", 40),
            row("OK", "FAIL", 41), row("OK", "PASS", 41),
        ])
        self.assertEqual(measured["ko_leak"], {"count": 1, "denominator": 3, "rate": 1 / 3})
        self.assertEqual(measured["automatic_error"], {"count": 2, "denominator": 3, "rate": 2 / 3})
        self.assertEqual(measured["precise"]["count"], 1)
        self.assertEqual(set(measured["timing_by_size"]), {"40x22", "41x22"})
        self.assertEqual(nearest_rank([1, 2, 3, 4], 95), 4)

    def test_single_threshold_counts_ties_as_fail(self):
        rows = [dict(label="KO", score=value) for value in ("0.2", "0.3", "0.3", "0.9", "")]
        self.assertEqual(highest_threshold(rows, 2), 0.3)
        self.assertEqual(highest_threshold(rows, 4), 1.0)

    def test_test_cannot_start_without_freeze(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            inputs = base / "inputs"
            inputs.mkdir()
            names = ("validation.json", "test.json", "test-primary.json", "excluded.json")
            for name in names:
                write_json(inputs / name, [])
            write_json(inputs / "provenance.json", {"sha256": {name: checksum(inputs / name) for name in names}})
            with self.assertRaisesRegex(ValueError, "accepted validation freeze"):
                evaluate_test(base, base / "dummy.exe", base / "dummy.zip")
            self.assertFalse((inputs / "images" / "test").exists())

    def test_validation_cannot_replace_a_frozen_record(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            freeze = base / "freeze.json"
            write_json(freeze, {"schema_version": 1})
            original = checksum(freeze)
            with self.assertRaisesRegex(FileExistsError, "already frozen"):
                validate(base, base / "runner", base / "calibrator", base / "baseline", base / "local", resume=True)
            self.assertEqual(checksum(freeze), original)

    def test_validation_retries_an_interrupted_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / "config").mkdir()
            candidate_dir = base / "validation" / "rechecks" / "candidate-0001"
            partial = candidate_dir / "run" / "results.csv"
            partial.parent.mkdir(parents=True)
            partial.write_text("interrupted", encoding="utf-8")
            candidate = dict(fast_pass_max=0.2, fast_fail_min=0.5, precise_pass_max=0.2, precise_fail_min=0.5,
                             eligible=True, counts=[[0, 0, 0, 0], [0, 1, 0, 0]], precise_count=0)
            selection = {"constraints": {"ko_leak_max_count": 0}, "budget_us": 132, "calibration": candidate}

            def fresh_run(inspect, mode, config, manifest, run, split):
                self.assertNotEqual(run.parent, candidate_dir)
                run.mkdir()
                (run / "results.csv").write_text("complete", encoding="utf-8")
                return [dict(label="KO", decision="FAIL", width="40", height="22", analysis_ms="0.2",
                             load_ms="0.1", precise_elapsed_us="")]

            with patch("tools.evaluate_s11.run_inspection", side_effect=fresh_run) as run:
                accepted = confirm_ranked(base, base / "runner", base / "manifest.json",
                                          {"candidates": [candidate]}, selection, 12, {})
            self.assertEqual(run.call_count, 1)
            self.assertEqual(accepted["status"], "accepted")
            self.assertEqual(partial.read_text(encoding="utf-8"), "complete")
            incomplete, = candidate_dir.parent.glob("candidate-0001-incomplete-*")
            self.assertEqual((incomplete / "run" / "results.csv").read_text(encoding="utf-8"), "interrupted")

    def test_validation_reuses_a_completed_candidate(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            manifest = base / "manifest.json"
            row = dict(path="val/KO/a.png", split="validation", label="KO", sha256="a" * 64)
            write_json(manifest, {"rows": [row]})
            candidate = base / "rechecks" / "candidate-0001"
            run = candidate / "run"
            run.mkdir(parents=True)
            config = {"budget_us": 132}
            setting = candidate / "candidate.json"
            write_json(setting, config)
            write_json(run / "summary.json", dict(input_manifest_valid=1, manifest_sha256=checksum(manifest),
                                                   config_sha256=checksum(setting), threads=1))
            (run / "results.csv").write_text("source_path,split,label,image_sha256\n"
                                           f"val/KO/a.png,validation,KO,{row['sha256']}\n", encoding="utf-8")
            with patch("tools.evaluate_s11.run_inspection") as inspect:
                rows = run_candidate(base / "runner", manifest, candidate, config)
            inspect.assert_not_called()
            self.assertEqual(rows[0]["source_path"], row["path"])


if __name__ == "__main__":
    unittest.main()
