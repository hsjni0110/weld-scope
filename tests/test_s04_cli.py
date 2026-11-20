"""Integration checks against the actual C++ executable; no mocked engine."""
import csv
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

CLI = None


def png(width=2, height=2):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    if (width, height) == (2, 2):
        raw = b''.join(b'\0' + b''.join(bytes([value]) * 3 for value in row) for row in [[0, 128], [129, 255]])
        encoded = zlib.compress(raw)
    else:
        compressor = zlib.compressobj()
        scanline = b'\0' * (1 + width * 3)
        encoded = b''.join(compressor.compress(scanline) for _ in range(height)) + compressor.flush()
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) + chunk(b'IDAT', encoded) + chunk(b'IEND', b'')


@unittest.skipUnless(CLI, 'Run this integration file with the C++ executable argument')
class CliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / '이미지,폴더'
        self.root.mkdir()
        self.config = self.root / 'config.json'
        self.config.write_text(json.dumps(dict(schema_version=1, intensity_threshold=128, fail_ratio=0.5)), encoding='utf-8')
        self.image = self.root / 'images/train/OK/샘플.png'
        self.image.parent.mkdir(parents=True)
        self.image.write_bytes(png())

    def run_cli(self, *arguments, code=0):
        result = subprocess.run([str(CLI), '--config', str(self.config), *map(str, arguments)], capture_output=True)
        self.assertEqual(result.returncode, code, result.stderr.decode('utf-8', errors='replace'))
        return result

    def row(self, path='train/OK/샘플.png', data=None):
        return dict(path=path, split='train', label='OK', debug=1,
                    sha256=hashlib.sha256(png() if data is None else data).hexdigest())

    def manifest(self, rows):
        path = self.root / 'manifest.json'
        path.write_text(json.dumps(dict(schema_version=1, root='images', rows=rows), ensure_ascii=False), encoding='utf-8')
        return path

    def results(self, output):
        with (output / 'results.csv').open(encoding='utf-8', newline='') as stream:
            return list(csv.DictReader(stream))

    def test_single_batch_repeat_unicode_hashes_and_debug_artifacts_match(self):
        single, batch, repeat = [self.root / name for name in ('single', 'batch', 'repeat')]
        self.run_cli('--image', self.image, '--output', single)
        self.run_cli('--manifest', self.manifest([self.row()]), '--output', batch)
        self.run_cli('--manifest', self.root / 'manifest.json', '--output', repeat)
        self.assertEqual(json.loads((single / 'summary.json').read_text())['scope'], 'unlabelled-single-image')
        records = [self.results(path)[0] for path in (single, batch, repeat)]
        for record in records:
            self.assertEqual(record['decision'], 'FAIL')
            self.assertEqual(float(record['score']), 0.5)
            self.assertEqual(record['candidate_pixels'], '2')
            self.assertEqual(record['width'], '2')
            self.assertEqual(record['config_sha256'], hashlib.sha256(self.config.read_bytes()).hexdigest())
            self.assertEqual(record['image_sha256'], hashlib.sha256(png()).hexdigest())
        for name in ('original.png', 'gray.png', 'mask.png', 'overlay.png'):
            self.assertEqual((single / 'images/000001' / name).read_bytes(), (batch / 'images/000001' / name).read_bytes())
            self.assertEqual((batch / 'images/000001' / name).read_bytes(), (repeat / 'images/000001' / name).read_bytes())

    def test_mixed_errors_are_recorded_without_scores_and_do_not_skip_valid_input(self):
        bad = self.image.parent / 'bad.png'
        bad.write_bytes(b'broken')
        rows = [self.row('train/OK/bad.png', b'broken'), self.row('train/OK/missing.png'), self.row()]
        output = self.root / 'mixed'
        self.run_cli('--manifest', self.manifest(sorted(rows, key=lambda row: row['path'])), '--output', output, code=2)
        records = self.results(output)
        self.assertEqual([record['decision'] for record in records], ['ERROR', 'ERROR', 'FAIL'])
        self.assertEqual([record['score'] for record in records[:2]], ['', ''])
        self.assertTrue(all(record['error'] for record in records[:2]))
        self.assertEqual(json.loads((output / 'summary.json').read_text())['ERROR'], 2)

    def test_damaged_png_ending_does_not_produce_a_verdict(self):
        data = png()
        endings = {'bad-crc': data[:-1] + bytes([data[-1] ^ 1]),
                   'missing-iend': data[:-12], 'extra-bytes': data + b'extra',
                   'second-image': data + data}
        for name, damaged in endings.items():
            with self.subTest(name=name):
                self.image.write_bytes(damaged)
                output = self.root / name
                self.run_cli('--image', self.image, '--output', output, code=2)
                row, = self.results(output)
                self.assertEqual(row['decision'], 'ERROR')
                self.assertEqual(row['score'], '')
                self.assertEqual(row['image_sha256'], hashlib.sha256(damaged).hexdigest())
                self.assertFalse((output / 'images').exists())

    def test_rejects_manifest_escape_duplicate_nontrain_unsorted_and_hash_mismatch(self):
        for index, rows in enumerate([
            [self.row('../escape.png')], [self.row('test/OK/a.png')], [self.row(), self.row()],
            [self.row('train/OK/z.png'), self.row('train/OK/a.png')],
            [dict(self.row(), split='validation')], [],
            [self.row('train/OK//샘플.png'), self.row()],
        ]):
            with self.subTest(index=index):
                self.run_cli('--manifest', self.manifest(rows), '--output', self.root / f'invalid{index}', code=1)
        output = self.root / 'hash-error'
        self.run_cli('--manifest', self.manifest([dict(self.row(), sha256='0' * 64)]), '--output', output, code=2)
        self.assertEqual(self.results(output)[0]['decision'], 'ERROR')

    def test_local_contrast_modes_report_distinct_scores_for_same_image(self):
        self.config.write_text(json.dumps(dict(schema_version=1, contrast_threshold=12, fail_ratio=0.75)),
                               encoding='utf-8')
        self.manifest([self.row()])
        for mode, decision, score, continuous in [('fast', 'FAIL', '1', ''), ('precise', 'PASS', '0.5', '0')]:
            with self.subTest(mode=mode):
                output = self.root / mode
                self.run_cli('--algorithm', mode, '--manifest', self.root / 'manifest.json', '--output', output)
                row, = self.results(output)
                self.assertEqual((row['width'], row['height'], row['candidate_pixels']), ('2', '2', '4'))
                self.assertEqual((row['decision'], row['score'], row['continuous_candidate_ratio']),
                                 (decision, score, continuous))
                self.assertEqual(row['image_sha256'], hashlib.sha256(self.image.read_bytes()).hexdigest())
                summary = json.loads((output / 'summary.json').read_text())
                self.assertEqual(summary['algorithm_version'], f'local-contrast-{mode}-v1')
                self.assertEqual(summary['config_sha256'], hashlib.sha256(self.config.read_bytes()).hexdigest())
                self.assertEqual(summary['contrast_threshold'], 12)
                self.assertEqual(summary['fail_ratio'], 0.75)
                self.assertTrue((output / 'images/000001/mask.png').is_file())
        self.assertEqual((self.root / 'fast/images/000001/mask.png').read_bytes(),
                         (self.root / 'precise/images/000001/mask.png').read_bytes())

    def test_local_contrast_errors_keep_existing_exit_codes(self):
        self.config.write_text(json.dumps(dict(schema_version=1, contrast_threshold=12, fail_ratio=0.1)),
                               encoding='utf-8')
        corrupt = self.image.parent / 'bad.png'
        corrupt.write_bytes(b'broken')
        self.manifest([self.row('train/OK/bad.png', b'broken'), dict(self.row(), sha256='0' * 64)])
        for mode in ('fast', 'precise'):
            with self.subTest(mode=mode):
                output = self.root / mode
                self.run_cli('--algorithm', mode, '--manifest', self.root / 'manifest.json', '--output', output, code=2)
                errors = self.results(output)
                self.assertEqual([row['decision'] for row in errors], ['ERROR', 'ERROR'])
                self.assertTrue(all(row['error'] and not row['score'] and not row['continuous_candidate_ratio']
                                    for row in errors))
        rejected = self.root / 'rejected'
        self.run_cli('--algorithm', 'other', '--image', self.image, '--output', rejected, code=1)
        self.config.write_text(json.dumps(dict(schema_version=1, contrast_threshold=256, fail_ratio=0.1)),
                               encoding='utf-8')
        self.run_cli('--algorithm', 'fast', '--image', self.image, '--output', rejected, code=1)
        self.assertFalse(rejected.exists())

    def test_bad_config_existing_output_and_huge_image_are_errors(self):
        output = self.root / 'exists'
        output.mkdir()
        sentinel = output / 'keep.txt'
        sentinel.write_text('keep')
        self.run_cli('--image', self.image, '--output', output, code=1)
        self.assertEqual(sentinel.read_text(), 'keep')
        huge = self.image.parent / 'huge.png'
        huge.write_bytes(png(4000, 4001))
        self.run_cli('--image', huge, '--output', self.root / 'huge', code=2)
        self.assertIn('PNG pixel limit', self.results(self.root / 'huge')[0]['error'])
        for index, config in enumerate([
            dict(schema_version=1, intensity_threshold=256, fail_ratio=0.5),
            dict(schema_version=1, intensity_threshold=128.5, fail_ratio=0.5),
            dict(schema_version=1, intensity_threshold=128, fail_ratio='NaN'),
            dict(schema_version=1, intensity_threshold=128, fail_ratio=2), {},
            dict(schema_version=1, intensity_threshold=4294967424, fail_ratio=0.5),
            dict(schema_version=1, intensity_threshold=-4294967168, fail_ratio=0.5),
            dict(schema_version=4294967297, intensity_threshold=128, fail_ratio=0.5),
        ]):
            self.config.write_text(json.dumps(config))
            self.run_cli('--image', self.image, '--output', self.root / f'config-error{index}', code=1)

    def test_large_debug_flag_is_rejected_before_narrowing(self):
        self.run_cli('--manifest', self.manifest([dict(self.row(), debug=4294967297)]),
                     '--output', self.root / 'large-debug', code=1)


def run_integration(executable):
    global CLI
    CLI = Path(executable).resolve()
    CliTests.__unittest_skip__ = False
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(CliTests)
    return unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful()


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('Usage: test_s04_cli.py path/to/weldscope_inspect.exe')
    raise SystemExit(0 if run_integration(sys.argv[1]) else 1)
