"""Small synthetic inputs validate the auditor, not real inspection accuracy."""

import csv
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from zipfile import ZipFile, ZipInfo

from PIL import Image
import tools.prepare_openzdm as preparation
from tools.prepare_openzdm import audit_archive, download_verified, find_similar_pairs, validate_members, verify_archive


def image_bytes(value=100, file_format="PNG"):
    stream = io.BytesIO()
    image = Image.new("L", (409, 40), value)
    image.save(stream, format=file_format)
    return stream.getvalue()


class PreparationTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def make_archive(self, items):
        path = self.root / "input.zip"
        with ZipFile(path, "w") as archive:
            for name, content in items:
                archive.writestr(name, content)
        return path

    def test_rejects_zip_paths_that_escape_or_collide_on_windows(self):
        # Removing validation must allow neither traversal nor Windows alias paths.
        for name in ("../bad.png", "/bad.png", "C:/bad.png", "a\\bad.png", "a/../bad.png", "a/file:ads", "a/NUL.png", "a/name. "):
            with self.subTest(name=name):
                with self.assertRaises(ValueError):
                    validate_members([ZipInfo(name)])
        with self.assertRaises(ValueError):
            validate_members([ZipInfo("a/A.png"), ZipInfo("a/a.png")])
        link = ZipInfo("link")
        link.create_system = 3
        link.external_attr = 0o120777 << 16
        with self.assertRaises(ValueError):
            validate_members([link])

    def test_rejects_oversized_member(self):
        item = ZipInfo("large.png")
        item.file_size = 17 * 1024 * 1024
        with self.assertRaises(ValueError):
            validate_members([item])

    def test_maps_official_val_directory_without_changing_original_path(self):
        path = self.make_archive([("val/OK/a.png", image_bytes())])
        output = self.root / "report"
        summary = audit_archive(path, output)
        self.assertEqual(summary["valid_image_count"], 1)
        with (output / "manifest.csv").open(encoding="utf-8", newline="") as stream:
            row = next(csv.DictReader(stream))
        self.assertEqual(row["path"], "val/OK/a.png")
        self.assertEqual(row["split"], "validation")

    def test_font_failure_preserves_audit_and_contact_sheet_can_be_retried(self):
        path = self.make_archive([("train/OK/a.png", image_bytes())])
        output = self.root / "report"
        with patch.object(preparation, "load_font", side_effect=ValueError("font unavailable")):
            summary = audit_archive(path, output)
            original_manifest = (output / "manifest.csv").read_bytes()
            original_summary = (output / "summary.json").read_bytes()
            self.assertEqual(summary["valid_image_count"], 1)
            with self.assertRaisesRegex(ValueError, "font unavailable"):
                preparation.create_contact_sheet(path, output)
        self.assertFalse((output / "contact_selection.json").exists())
        self.assertFalse((output / "contact_sheet.png").exists())
        preparation.create_contact_sheet(path, output)
        self.assertEqual((output / "manifest.csv").read_bytes(), original_manifest)
        self.assertEqual((output / "summary.json").read_bytes(), original_summary)
        with self.assertRaises(FileExistsError):
            preparation.create_contact_sheet(path, output)

    def test_similarity_counts_group_sizes_and_cross_split_pairs(self):
        # Hashes 0/31 are similar; 2**64-1 is outside the radius.
        rows = [
            {"status": "valid", "path": "a", "split": "train", "dhash": "0000000000000000"},
            {"status": "valid", "path": "b", "split": "train", "dhash": "0000000000000000"},
            {"status": "valid", "path": "c", "split": "test", "dhash": "0000000000000000"},
            {"status": "valid", "path": "d", "split": "validation", "dhash": "000000000000001f"},
            {"status": "valid", "path": "e", "split": "test", "dhash": "000000000000001f"},
            {"status": "valid", "path": "f", "split": "test", "dhash": "ffffffffffffffff"},
            {"status": "invalid", "path": "ignored", "split": "train", "dhash": "0000000000000000"},
        ]
        summary = preparation.write_similarity(rows, self.root)
        self.assertEqual(summary["unique_hash_count"], 3)
        self.assertEqual(summary["similar_distinct_hash_pairs"], 1)
        self.assertEqual(summary["same_hash_image_pairs"], 4)
        self.assertEqual(summary["candidate_image_pairs"], 10)
        self.assertEqual(summary["cross_split_candidate_pairs"], 8)
        with (self.root / "similar_hash_pairs.csv").open(encoding="utf-8", newline="") as stream:
            pairs = list(csv.DictReader(stream))
        self.assertEqual(len(pairs), 1)
        self.assertEqual(pairs[0]["image_pair_count"], "6")
        self.assertEqual(pairs[0]["cross_split_pair_count"], "5")

    def test_cli_reuses_verified_checksum_from_download(self):
        archive = self.root / "downloaded.zip"
        output = self.root / "output"
        output.mkdir()
        summary = {"file_count": 0, "valid_image_count": 0, "invalid_file_count": 0, "evaluation_hold_reasons": []}
        with patch.object(preparation, "download_archive", return_value=(archive, "verified-sha256")), \
                patch.object(preparation, "verify_archive", side_effect=AssertionError("Unexpected second hash pass")), \
                patch.object(preparation, "audit_archive", return_value=summary), \
                patch.object(preparation, "create_contact_sheet", create=True), \
                patch("sys.argv", ["prepare_openzdm.py", "--output", str(output)]):
            self.assertEqual(preparation.main(), 0)
        checksum = json.loads((output / "archive-checksum.json").read_text(encoding="utf-8"))
        self.assertEqual(checksum["sha256"], "verified-sha256")

    def test_existing_download_returns_checksum_after_one_verification(self):
        target = self.root / preparation.ARCHIVE_NAME
        payload = b"abcdef"
        target.write_bytes(payload)
        def verify_small_archive(path):
            return verify_archive(path, 6, hashlib.md5(payload).hexdigest())
        with patch.object(preparation, "verify_archive", side_effect=verify_small_archive) as verification:
            path, checksum = preparation.download_archive(self.root)
        self.assertEqual(path, target)
        self.assertEqual(checksum, hashlib.sha256(payload).hexdigest())
        self.assertEqual(verification.call_count, 1)

    def test_cli_contact_only_does_not_repeat_audit_or_replace_summary(self):
        path = self.make_archive([("train/OK/a.png", image_bytes())])
        output = self.root / "report"
        audit_archive(path, output)
        original_summary = (output / "summary.json").read_bytes()
        original_manifest = (output / "manifest.csv").read_bytes()
        with patch.object(preparation, "verify_archive", return_value="verified"), \
                patch.object(preparation, "audit_archive", side_effect=AssertionError("Audit repeated")), \
                patch("sys.argv", ["prepare_openzdm.py", "--archive", str(path), "--output", str(output), "--contact-sheet-only"]):
            self.assertEqual(preparation.main(), 0)
        self.assertEqual((output / "summary.json").read_bytes(), original_summary)
        self.assertEqual((output / "manifest.csv").read_bytes(), original_manifest)
        self.assertTrue((output / "contact_sheet.png").is_file())

    def test_checksum_mismatch_leaves_original_untouched(self):
        path = self.make_archive([("train/OK/a.png", image_bytes())])
        original = path.read_bytes()
        with self.assertRaises(ValueError):
            verify_archive(path, len(original), "0" * 32)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(verify_archive(path, len(original), hashlib.md5(original).hexdigest()), hashlib.sha256(original).hexdigest())

    def test_download_resumes_partial_and_rejects_server_ignoring_range(self):
        # A real local HTTP server verifies the wire contract without external access.
        from http.server import BaseHTTPRequestHandler, HTTPServer
        from threading import Thread
        payload = b"abcdef"
        requests = []

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                requests.append(self.headers.get("Range"))
                is_full = self.headers.get("Range") == "bytes=0-5"
                self.send_response(206 if self.path == "/resume" else 200)
                self.send_header("Content-Range", "bytes 0-5/6" if is_full else "bytes 3-5/6")
                self.send_header("Content-Length", "6" if is_full else "3")
                self.end_headers()
                self.wfile.write(payload if is_full else payload[3:])

            def log_message(self, *args):
                pass

        with HTTPServer(("127.0.0.1", 0), Handler) as server:
            worker = Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                target = self.root / "archive.zip"
                partial = target.with_suffix(".zip.part")
                partial.write_bytes(payload[:3])
                with self.assertRaises(ValueError):
                    download_verified(f"http://127.0.0.1:{server.server_port}/ignored", target, 6, hashlib.md5(payload).hexdigest())
                self.assertEqual(partial.read_bytes(), b"abc")
                digest = download_verified(f"http://127.0.0.1:{server.server_port}/resume", target, 6, hashlib.md5(payload).hexdigest())
                self.assertEqual(digest, hashlib.sha256(payload).hexdigest())
                self.assertEqual(target.read_bytes(), payload)
                self.assertFalse(partial.exists())
                empty_target = self.root / "empty.zip"
                empty_target.with_suffix(".zip.part").touch()
                download_verified(f"http://127.0.0.1:{server.server_port}/resume", empty_target, 6, hashlib.md5(payload).hexdigest())
                self.assertEqual(empty_target.read_bytes(), payload)
                self.assertEqual(requests, ["bytes=3-5", "bytes=3-5", "bytes=0-5"])
            finally:
                server.shutdown()
                worker.join()

    def test_audit_records_corruption_unknown_label_and_exact_label_conflict(self):
        content = image_bytes()
        path = self.make_archive([
            ("train/OK/a.png", content), ("test/KO/b.png", content),
            ("validation/OK/broken.png", b"not an image"),
            ("train/UNKNOWN/other.png", content), ("notes.txt", b"notes"),
        ])
        output = self.root / "report"
        summary = audit_archive(path, output)
        self.assertEqual(summary["file_count"], 5)
        self.assertEqual(summary["valid_image_count"], 2)
        self.assertEqual(summary["invalid_file_count"], 3)
        self.assertEqual(summary["cross_split_pixel_groups"], 1)
        self.assertEqual(summary["conflicting_pixel_groups"], 1)
        self.assertFalse(summary["evaluation_ready"])
        with (output / "manifest.csv").open(encoding="utf-8", newline="") as stream:
            rows = list(csv.DictReader(stream))
        self.assertEqual(len(rows), 5)
        self.assertEqual({row["label"] for row in rows if row["status"] == "valid"}, {"OK", "KO"})
        self.assertEqual(len(json.loads((output / "exact_duplicates.json").read_text())["pixel_sha256"]), 1)

    def test_distinguishes_identical_pixels_from_identical_file_bytes(self):
        path = self.make_archive([
            ("train/OK/a.png", image_bytes()),
            ("validation/OK/a.bmp", image_bytes(file_format="BMP")),
        ])
        summary = audit_archive(path, self.root / "report")
        self.assertEqual(summary["duplicate_pixel_groups"], 1)
        self.assertEqual(summary["duplicate_file_groups"], 0)

    def test_reports_native_dimensions_and_color_without_changing_pixels(self):
        color = io.BytesIO()
        Image.new("RGB", (20, 20)).save(color, format="PNG")
        path = self.make_archive([("train/OK/a.png", color.getvalue())])
        summary = audit_archive(path, self.root / "report")
        self.assertEqual(summary["valid_image_count"], 1)
        self.assertEqual(summary["image_properties"], {"20x20/RGB": 1})
        self.assertEqual(summary["description_mismatch_count"], 1)
        self.assertFalse(summary["evaluation_ready"])

    def test_oversized_decoded_image_is_reported_and_following_files_are_audited(self):
        import struct
        import zlib
        def chunk(kind, value):
            return struct.pack(">I", len(value)) + kind + value + struct.pack(">I", zlib.crc32(kind + value))
        blob = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 20000, 20000, 8, 0, 0, 0, 0))
        blob += chunk(b"IDAT", zlib.compress(b"")) + chunk(b"IEND", b"")
        path = self.make_archive([("train/OK/bomb.png", blob), ("train/OK/valid.png", image_bytes())])
        summary = audit_archive(path, self.root / "report")
        self.assertEqual(summary["invalid_file_count"], 1)
        self.assertEqual(summary["valid_image_count"], 1)

    def test_empty_archive_is_not_evaluation_ready(self):
        summary = audit_archive(self.make_archive([]), self.root / "report")
        self.assertFalse(summary["evaluation_ready"])

    def test_manifest_is_repeatable_and_contact_sheet_excludes_test(self):
        path = self.make_archive([
            ("train/OK/a.png", image_bytes(60)),
            ("train/KO/b.png", image_bytes(120)),
            ("test/KO/secret.png", image_bytes(180)),
        ])
        first, second = self.root / "one", self.root / "two"
        audit_archive(path, first)
        audit_archive(path, second)
        preparation.create_contact_sheet(path, first)
        preparation.create_contact_sheet(path, second)
        self.assertEqual((first / "manifest.csv").read_bytes(), (second / "manifest.csv").read_bytes())
        selection = json.loads((first / "contact_selection.json").read_text())
        self.assertEqual(selection, ["train/OK/a.png", "train/KO/b.png"])
        with Image.open(first / "contact_sheet.png") as image:
            image.load()
            self.assertGreater(image.width, 800)

    def test_existing_output_is_not_overwritten(self):
        path = self.make_archive([])
        output = self.root / "report"
        output.mkdir()
        (output / "manifest.csv").write_text("user data")
        with self.assertRaises(FileExistsError):
            audit_archive(path, output)
        self.assertEqual((output / "manifest.csv").read_text(), "user data")

    def test_similar_candidates_include_five_bits_but_not_six(self):
        self.assertEqual(list(find_similar_pairs([0, 31, 63])), [(0, 1, 5), (1, 2, 1)])

    def test_similarity_index_matches_bruteforce_on_fixed_hashes(self):
        import random
        randomizer = random.Random(20261117)
        hashes = [randomizer.getrandbits(64) for _ in range(40)]
        hashes += [hashes[0] ^ 31, hashes[0], hashes[5] ^ (31 << 30)]
        expected = {(left, right, (hashes[left] ^ hashes[right]).bit_count())
                    for left in range(len(hashes)) for right in range(left + 1, len(hashes))
                    if (hashes[left] ^ hashes[right]).bit_count() <= 5}
        self.assertEqual(set(find_similar_pairs(hashes)), expected)


if __name__ == "__main__":
    unittest.main()
