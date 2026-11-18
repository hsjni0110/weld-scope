import hashlib
import io
from pathlib import Path
import tempfile
import unittest
from zipfile import ZipFile

from tools.prepare_s04_inputs import extract_train, validate_prepared


class TrainPreparationTests(unittest.TestCase):
    def make_archive(self, entries):
        buffer = io.BytesIO()
        with ZipFile(buffer, 'w') as archive:
            for path, data in entries.items():
                archive.writestr(path, data)
        buffer.seek(0)
        return ZipFile(buffer)

    def row(self, path='train/OK/a.png', data=b'image'):
        return dict(path=path, split='train', label='OK', status='valid',
                    file_sha256=hashlib.sha256(data).hexdigest())

    def test_extracts_only_sorted_train_bytes_and_marks_selected_debug_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'output'
            archive = self.make_archive({'train/OK/a.png': b'image', 'test/KO/private.png': b'held'})
            result = extract_train(archive, [self.row()], output, {'train/OK/a.png'})
            self.assertEqual((output / 'images/train/OK/a.png').read_bytes(), b'image')
            self.assertEqual(result['rows'][0]['debug'], 1)
            self.assertFalse((output / 'images/test').exists())

    def test_rejects_wrong_hash_unsafe_path_nontrain_and_duplicate(self):
        bad_rows = [self.row(data=b'wrong'), self.row('../escape.png'),
                    self.row('test/OK/a.png')]
        for row in bad_rows:
            with self.subTest(row=row), tempfile.TemporaryDirectory() as directory:
                with self.assertRaises(ValueError):
                    extract_train(self.make_archive({'train/OK/a.png': b'image'}), [row], Path(directory) / 'out', set())
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(ValueError):
                extract_train(self.make_archive({'train/OK/a.png': b'image'}), [self.row(), self.row()], Path(directory) / 'out', set())

    def test_refuses_existing_output_and_debug_outside_train(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(FileExistsError):
                extract_train(self.make_archive({}), [], Path(directory), set())
            with self.assertRaises(ValueError):
                extract_train(self.make_archive({'train/OK/a.png': b'image'}), [self.row()], Path(directory) / 'new', {'test/OK/a.png'})


    def test_hash_failure_leaves_no_final_directory_and_retry_works(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'retry'
            with self.assertRaises(ValueError):
                extract_train(self.make_archive({'train/OK/a.png': b'image'}), [self.row(data=b'wrong')], output, set())
            self.assertFalse(output.exists())
            extract_train(self.make_archive({'train/OK/a.png': b'image'}), [self.row()], output, set())
            self.assertTrue((output / 'manifest.json').exists())

    def test_cached_sample_cannot_claim_full_train(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / 'inputs'
            extract_train(self.make_archive({'train/OK/a.png': b'image'}), [self.row()], output, set(), 'a' * 64)
            validate_prepared(output, [self.row()], set(), 'a' * 64)
            with self.assertRaises(ValueError):
                validate_prepared(output, [self.row(), self.row('train/OK/b.png')], set(), 'a' * 64)


if __name__ == '__main__':
    unittest.main()
