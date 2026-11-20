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


def png(width=2, height=2, pixels=None):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    if pixels is not None or (width, height) == (2, 2):
        if pixels is None:
            pixels = [[0, 128], [129, 255]]
        raw = b''.join(b'\0' + b''.join(bytes([value]) * 3 for value in row) for row in pixels)
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

    def test_validation_manifest_selection_and_observation_output(self):
        self.config.write_text(json.dumps(dict(schema_version=1, contrast_threshold=12, fail_ratio=0.75)),
                               encoding='utf-8')
        image = self.root / 'images/val/OK/fixture.png'
        image.parent.mkdir(parents=True)
        image.write_bytes(png())
        row = dict(self.row('val/OK/fixture.png'), split='validation', debug=0)
        manifest = self.manifest([row])
        self.run_cli('--algorithm', 'fast', '--manifest', manifest, '--output', self.root / 'no-split', code=1)
        self.run_cli('--algorithm', 'fast', '--image', image, '--evaluation-split', 'validation',
                     '--output', self.root / 'single-val', code=1)
        output = self.root / 'validation'
        self.run_cli('--algorithm', 'fast', '--manifest', manifest, '--evaluation-split', 'validation',
                     '--output', output)
        observation, = self.results(output)
        self.assertEqual(observation['fast_score'], '1')
        self.assertGreaterEqual(int(observation['fast_elapsed_us']), 0)
        self.assertEqual(int(observation['fast_elapsed_ns']) // 1000, int(observation['fast_elapsed_us']))
        self.assertEqual(observation['precise_elapsed_ns'], '')
        self.assertEqual(observation['precise_score'], '')
        self.assertEqual(observation['reason'], 'fast_threshold')
        summary = json.loads((output / 'summary.json').read_text())
        self.assertEqual(summary['evaluation_split'], 'validation')
        self.assertEqual(summary['manifest_sha256'], hashlib.sha256(manifest.read_bytes()).hexdigest())
        self.assertEqual(summary['input_manifest_valid'], 1)
        incomplete = self.root / 'validation-incomplete'
        self.run_cli('--algorithm', 'fast', '--manifest', self.manifest([row, self.row('val/OK/missing.png')
                     | {'split': 'validation'}]), '--evaluation-split', 'validation',
                     '--output', incomplete, code=2)
        self.assertEqual(json.loads((incomplete / 'summary.json').read_text())['input_manifest_valid'], 0)
        self.run_cli('--manifest', self.manifest([dict(self.row('test/OK/fixture.png'), split='test'), row]),
                     '--evaluation-split', 'validation', '--output', self.root / 'mixed-split', code=1)

    def test_selective_cli_records_ordered_attempts_and_final_review(self):
        self.image.write_bytes(png(20, 4, [[0] * 10 + [255] + [0] * 9] * 4))
        self.config.write_text(json.dumps(dict(schema_version=1, contrast_threshold=12,
            fast_pass_max=0, fast_fail_min=1, precise_pass_max=0, precise_fail_min=1, budget_us=1_000_000)),
            encoding='utf-8')
        output = self.root / 'selective'
        self.run_cli('--algorithm', 'selective', '--image', self.image, '--output', output)
        row, = self.results(output)
        self.assertEqual(row['decision'], 'REVIEW')
        self.assertEqual(row['reason'], 'precise_uncertain')
        self.assertEqual(row['fast_outcome'], 'REVIEW')
        self.assertEqual(row['precise_outcome'], 'REVIEW')
        self.assertTrue(row['fast_elapsed_us'] and row['precise_elapsed_us'])
        fast_ns, precise_ns = (int(row[f'{stage}_elapsed_ns']) for stage in ('fast', 'precise'))
        self.assertAlmostEqual(float(row['analysis_ms']), (fast_ns + precise_ns) / 1_000_000, places=8)
        self.assertTrue((output / 'images/000001/overlay.png').is_file())
        summary = json.loads((output / 'summary.json').read_text())
        self.assertEqual(summary['REVIEW'], 1)
        self.assertEqual(summary['algorithm_version'], 'local-contrast-selective-v1')

    def test_test_manifest_requires_matching_freeze_before_creating_output(self):
        image = self.root / 'images/test/OK/fixture.png'
        image.parent.mkdir(parents=True)
        image.write_bytes(png())
        manifest = self.manifest([dict(self.row('test/OK/fixture.png'), split='test', debug=0)])
        output = self.root / 'result'
        args = ('--manifest', manifest, '--evaluation-split', 'test', '--output', output)
        self.run_cli(*args, code=1)
        config_hash = hashlib.sha256(self.config.read_bytes()).hexdigest()
        freeze = dict(schema_version=1, test_manifest_sha256=hashlib.sha256(manifest.read_bytes()).hexdigest(),
                      executable_sha256=hashlib.sha256(CLI.read_bytes()).hexdigest(),
                      config_sha256=dict(baseline=config_hash, fast='1' * 64, precise='2' * 64, selective='3' * 64))
        freeze_path = self.root / 'freeze.json'
        for key in ('test_manifest_sha256', 'executable_sha256', 'config_sha256'):
            with self.subTest(key=key):
                wrong_hash = '0' * 64 if key != 'config_sha256' else freeze[key] | {'baseline': '0' * 64}
                freeze_path.write_text(json.dumps(freeze | {key: wrong_hash}), encoding='utf-8')
                self.run_cli(*args, '--freeze', freeze_path, code=1)
                self.assertFalse(output.exists())
        freeze_path.write_text(json.dumps(freeze), encoding='utf-8')
        self.run_cli(*args, '--freeze', freeze_path)
        self.assertEqual(self.results(output)[0]['decision'], 'FAIL')
        self.assertEqual(json.loads((output / 'summary.json').read_text())['evaluation_split'], 'test')


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
