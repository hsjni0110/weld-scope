import hashlib
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from zipfile import ZipFile


@unittest.skipUnless(sys.platform == "win32", "Windows PowerShell required")
class PackageS12Tests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.repo = Path(__file__).resolve().parents[1]
        script = self.repo / "tools/package-s12.ps1"
        (self.root / "tools").mkdir()
        shutil.copyfile(script, self.root / "tools" / script.name)
        self.output = self.root / "output [1]" / "package.zip"

        baseline = self.write("app/baseline.json", b"baseline")
        selective = self.write("app/selective.json", b"selective")
        config_hashes = {"baseline": self.digest(baseline), "selective": self.digest(selective)}
        freeze = json.dumps({"config_sha256": config_hashes}).encode()
        self.write("docs/steps/S11-evidence/freeze.json", freeze)
        self.write("app/freeze.json", freeze)
        sample = self.write("sample.png", b"two stage")
        demo = {"modes": {"selective": {"image_sha256": self.digest(sample)}}}
        self.write("docs/steps/S11-evidence/demo/comparison.json", json.dumps(demo).encode())
        source = "src/WeldScope.App/res/samples/"
        self.write(source + "defect.png", b"defect")
        self.write("app/defect.png", b"defect")
        self.write(source + "normal.png", b"normal")
        self.write(source + "corrupt.png", b"corrupt")
        for path in (
            "db/s05/schema.sql", "db/s13/schema.sql", "docs/steps/S12-run-package.md",
            "docs/steps/S13-run-package.md", "tools/WeldScope-Start.cmd",
            "tools/Start-WeldScope-Practice.ps1",
            "docs/steps/S12-third-party-notices.md", ".tools/opencv/install-x64/LICENSE",
            ".tools/opencv/install-x64/etc/licenses/libpng-LICENSE",
            ".tools/opencv/install-x64/etc/licenses/zlib-LICENSE",
        ):
            self.write(path, b"packaging input")
        self.write("app/WeldScope.App.exe", self.pe_header())
        self.write("inspect.exe", self.pe_header())
        for name in ("pooled-smooth-220.json", "model-220.txt"):
            self.write("config/" + name, (self.repo / "config" / name).read_bytes())

    def write(self, name, data):
        target = self.root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return data

    @staticmethod
    def digest(data):
        return hashlib.sha256(data).hexdigest()

    @staticmethod
    def pe_header(machine=0x8664, characteristics=0x0022):
        pe_offset = 0x80
        optional_size = 240
        data = bytearray(pe_offset + 24 + optional_size + 40)
        data[:2] = b"MZ"
        struct.pack_into("<I", data, 0x3C, pe_offset)
        data[pe_offset:pe_offset + 4] = b"PE\0\0"
        struct.pack_into("<HHIIIHH", data, pe_offset + 4, machine, 1, 0, 0, 0, optional_size, characteristics)
        struct.pack_into("<H", data, pe_offset + 24, 0x20B)
        return data

    def package(self, output=None, folder=False):
        return subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
             "-File", "tools/package-s12.ps1", "-AppDir", "app", "-Inspect", "inspect.exe",
             "-TwoStageSample", "sample.png", "-Output", str(output or self.output),
             *(["-FolderInspection"] if folder else [])],
            cwd=self.root, capture_output=True, text=True, errors="replace", timeout=30,
        )

    def test_rejects_arm64_app(self):
        self.write("app/WeldScope.App.exe", self.pe_header(machine=0xAA64))
        result = self.package()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("WeldScope.App.exe", result.stderr)
        self.assertFalse(self.output.exists())

    def test_rejects_invalid_cli(self):
        for label, data in {
            "text": b"not an executable",
            "truncated": self.pe_header()[:0x98],
            "dll": self.pe_header(characteristics=0x2022),
        }.items():
            with self.subTest(label=label):
                self.write("inspect.exe", data)
                output = self.output.with_name(f"{label}.zip")
                result = self.package(output)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn("weldscope_inspect.exe", result.stderr)
                self.assertFalse(output.exists())

    def test_packages_x64_executable_headers(self):
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        with ZipFile(self.output) as archive:
            self.assertEqual(archive.read("WeldScope.App.exe"), self.pe_header())
            self.assertEqual(archive.read("weldscope_inspect.exe"), self.pe_header())
            self.assertIn("SHA256SUMS.txt", archive.namelist())

    def install_model(self):
        for name in ("pooled-smooth-220.json", "model-220.txt"):
            shutil.copyfile(self.root / "config" / name, self.root / "app" / name)

    def test_current_build_ships_registered_model_in_both_packages(self):
        self.install_model()
        for folder in (True, False):
            with self.subTest(folder=folder):
                archive_path = self.output.with_name(f"model-{folder}.zip")
                result = self.package(output=archive_path, folder=folder)
                self.assertEqual(result.returncode, 0, result.stderr)
                with ZipFile(archive_path) as archive:
                    manifest = "pooled-smooth-220.json"
                    self.assertIn(manifest, archive.namelist())
                    settings = archive.read(manifest)
                    expected = "6a557520c10a26ec9ce7dc9585916c57b9dab34a421bc6650bf3d2227dfa7d7b"
                    self.assertEqual(self.digest(settings), expected)
                    model = json.loads(settings)
                    self.assertEqual(self.digest(archive.read(model["model"])), model["model_sha256"])
                    sums = archive.read("SHA256SUMS.txt").decode("ascii").splitlines()
                    self.assertIn(f"{expected}  {manifest}", sums)
                    self.assertIn(f'{model["model_sha256"]}  {model["model"]}', sums)

    def test_folder_package_rejects_missing_or_modified_model(self):
        for bad_file in (None, "pooled-smooth-220.json", "model-220.txt"):
            with self.subTest(bad_file=bad_file):
                if bad_file:
                    self.install_model()
                    self.write("app/" + bad_file, b"altered")
                output = self.output.with_name(f"bad-{bad_file}.zip")
                result = self.package(output=output, folder=True)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertIn(bad_file or "pooled-smooth-220.json", result.stderr)
                self.assertFalse(output.exists())

    def test_rejects_unregistered_thresholds_even_when_sources_match(self):
        self.install_model()
        name = "pooled-smooth-220.json"
        model = json.loads((self.root / "config" / name).read_bytes())
        model["budget_us"] += 1
        for directory in ("config", "app"):
            self.write(directory + "/" + name, json.dumps(model).encode("ascii"))
        result = self.package(folder=True)
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn(name, result.stderr)
        self.assertFalse(self.output.exists())
