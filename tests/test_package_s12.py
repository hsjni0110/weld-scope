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

        source = "src/WeldScope.App/res/samples/"
        self.write(source + "defect.png", b"defect")
        self.write("app/defect.png", b"defect")
        self.write(source + "normal.png", b"normal")
        self.write(source + "corrupt.png", b"corrupt")
        for path in (
            "db/s05/schema.sql", "db/s13/schema.sql",
            "docs/steps/S13-run-package.md", "tools/WeldScope-Start.cmd",
            "tools/Start-WeldScope-Practice.ps1",
            "docs/steps/S12-third-party-notices.md", ".tools/opencv/install-x64/LICENSE",
            ".tools/opencv/install-x64/etc/licenses/libpng-LICENSE",
            ".tools/opencv/install-x64/etc/licenses/zlib-LICENSE",
        ):
            self.write(path, b"packaging input")
        self.write("app/WeldScope.App.exe", self.pe_header())
        for name in ("pooled-smooth-220.json", "model-220.txt"):
            self.write("app/" + name, (self.repo / "config" / name).read_bytes())

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

    def package(self, output=None):
        return subprocess.run(
            ["powershell.exe", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
             "-File", "tools/package-s12.ps1", "-AppDir", "app", "-Output", str(output or self.output)],
            cwd=self.root, capture_output=True, text=True, errors="replace", timeout=30,
        )

    def test_rejects_arm64_app(self):
        self.write("app/WeldScope.App.exe", self.pe_header(machine=0xAA64))
        result = self.package()
        self.assertNotEqual(result.returncode, 0, result.stdout)
        self.assertIn("WeldScope.App.exe", result.stderr)
        self.assertFalse(self.output.exists())

    def test_packages_single_model_and_excludes_legacy_files(self):
        for name in ("baseline.json", "selective.json", "freeze.json", "weldscope_inspect.exe"):
            self.write("app/" + name, b"legacy")
        result = self.package()
        self.assertEqual(result.returncode, 0, result.stderr)
        with ZipFile(self.output) as archive:
            self.assertEqual(archive.read("WeldScope.App.exe"), self.pe_header())
            settings = archive.read("pooled-smooth-220.json")
            model = json.loads(settings)
            self.assertEqual(model["schema_version"], 2)
            self.assertNotIn("budget_us", model)
            self.assertEqual(self.digest(archive.read(model["model"])), model["model_sha256"])
            self.assertIn("SHA256SUMS.txt", archive.namelist())
            self.assertIn("WeldScope-Start.cmd", archive.namelist())
            self.assertTrue({"baseline.json", "selective.json", "freeze.json", "weldscope_inspect.exe"}
                            .isdisjoint(archive.namelist()))

    def test_rejects_missing_or_modified_model(self):
        cases = (("model-220.txt", None), ("pooled-smooth-220.json", b"altered"), ("model-220.txt", b"altered"))
        for index, (name, data) in enumerate(cases):
            with self.subTest(name=name, data=data):
                path = self.root / "app" / name
                original = (self.repo / "config" / name).read_bytes()
                if data is None:
                    path.unlink()
                else:
                    path.write_bytes(data)
                try:
                    output = self.output.with_name(f"bad-{index}.zip")
                    result = self.package(output)
                    self.assertNotEqual(result.returncode, 0, result.stdout)
                    self.assertIn(name, result.stderr)
                    self.assertFalse(output.exists())
                finally:
                    path.write_bytes(original)
