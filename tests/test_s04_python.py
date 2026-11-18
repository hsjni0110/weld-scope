from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import unittest


class PythonResolutionTests(unittest.TestCase):
    def resolve(self, root, explicit=None):
        helper = Path(__file__).resolve().parents[1] / 'tools/python-s04.ps1'
        quote = lambda path: "'" + str(path).replace("'", "''") + "'"
        command = f". {quote(helper)}; Resolve-S04Python -RepositoryRoot {quote(root)}"
        if explicit is not None:
            command += f" -Python {quote(explicit)}"
        return subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-Command', command], capture_output=True)

    def make_candidate(self, root, relative):
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.touch()
        return path

    def test_venv_precedes_embedded_and_explicit_selection_is_shared(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            embedded = self.make_candidate(root, '.tools/python/python.exe')
            result = self.resolve(root)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
            self.assertEqual(Path(result.stdout.decode().strip()), embedded)
            venv = self.make_candidate(root, '.venv/Scripts/python.exe')
            result = self.resolve(root)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
            self.assertEqual(Path(result.stdout.decode().strip()), venv)
            result = self.resolve(root, embedded)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
            self.assertEqual(Path(result.stdout.decode().strip()), embedded)

    def test_invalid_explicit_selection_does_not_fall_back(self):
        with TemporaryDirectory() as directory:
            root = Path(directory)
            self.make_candidate(root, '.venv/Scripts/python.exe')
            self.assertNotEqual(self.resolve(root, root / 'missing.exe').returncode, 0)


if __name__ == '__main__': unittest.main()
