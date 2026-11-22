import hashlib
import io
from pathlib import Path
import tempfile
import unittest
from zipfile import ZipFile

from tools.prepare_inspection_inputs import make_cohorts, materialize


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
