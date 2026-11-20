import csv
import hashlib
import json
from pathlib import Path
from shutil import copytree
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest

from PIL import Image

from tools.report_s07 import draw_examples, load_comparison, summarize, verify_repeat


class ComparisonTests(unittest.TestCase):
    def setUp(self):
        temporary = TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.run = Path(temporary.name)
        self.manifest = self.run / 'manifest.json'
        self.ko = 'train/KO/a.png'
        self.ok = 'train/OK/b.png'
        self.manifest.write_text(json.dumps({'rows': [
            dict(path=self.ko, label='KO', split='train', sha256='a' * 64, debug=0),
            dict(path=self.ok, label='OK', split='train', sha256='b' * 64, debug=0),
        ]}), encoding='utf-8')
        for mode, version in [('baseline', 'global-dark-ratio-v1'), ('fast', 'local-contrast-fast-v1'),
                              ('precise', 'local-contrast-precise-v1')]:
            directory = self.run / mode
            directory.mkdir()
            key = 'intensity_threshold' if mode == 'baseline' else 'contrast_threshold'
            config = json.dumps(dict(schema_version=1, fail_ratio=0.5, **{key: 12})).encode()
            (directory / 'config.json').write_bytes(config)
            config_hash = hashlib.sha256(config).hexdigest()
            summary = dict(algorithm_version=version, scope='train-development-only',
                           independent_evaluation=0, requested=2, PASS=1, FAIL=1, ERROR=0,
                           config_sha256=config_hash, opencv_version='4.13.0', threads=1,
                           fail_ratio=0.5, **{key: 12})
            (directory / 'summary.json').write_text(json.dumps(summary), encoding='utf-8')
            scores = ('0.2', '0.8') if mode == 'baseline' else ('0.8', '0.2')
            rows = [dict(source_path=path, split='train', label=label, image_sha256=key * 64,
                         config_sha256=config_hash, width='5', height='2', candidate_pixels='8' if score == '0.8' else '2',
                         total_pixels='10', decision='FAIL' if score == '0.8' else 'PASS', score=score,
                         error='', artifact_path='', load_ms='2', analysis_ms='1', artifact_ms='0')
                    for path, label, key, score in zip((self.ko, self.ok), ('KO', 'OK'), ('a', 'b'), scores)]
            if mode != 'baseline':
                for row in rows:
                    row['continuous_candidate_ratio'] = row['score'] if mode == 'precise' else ''
            self.write_rows(mode, rows[::-1] if mode == 'fast' else rows)

    def rows(self, mode):
        with (self.run / mode / 'results.csv').open(encoding='utf-8', newline='') as stream:
            return list(csv.DictReader(stream))

    def write_rows(self, mode, rows):
        with (self.run / mode / 'results.csv').open('w', encoding='utf-8', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)

    def test_comparison_pairs_by_source_and_reports_corrections(self):
        rows, _ = load_comparison(self.run, self.manifest)
        summary = summarize(rows)
        self.assertEqual([row['source_path'] for row in rows], [self.ko, self.ok])
        self.assertEqual(summary['by_mode']['baseline']['ko_pass'], dict(count=1, denominator=1))
        self.assertEqual(summary['by_mode']['fast']['ko_pass'], dict(count=0, denominator=1))
        self.assertEqual(summary['by_mode']['fast']['ok_fail'], dict(count=0, denominator=1))
        self.assertEqual(summary['transitions']['fast']['KO PASS->FAIL'], 1)
        self.assertEqual(summary['transitions']['fast']['OK FAIL->PASS'], 1)
        self.assertEqual(summary['train_threshold_comparison']['fast']['threshold'], 1)
        self.assertEqual(summary['by_mode']['precise']['by_size']['5x2']['analysis_ms'], dict(p50=1, p95=1))

    def test_rejects_missing_duplicate_or_different_input(self):
        rows = self.rows('fast')
        for invalid in [rows[:1], [rows[0], rows[0]], [dict(rows[0], source_path='train/OK/c.png'), rows[1]],
                        [dict(rows[0], label='KO'), rows[1]], [dict(rows[0], image_sha256='c' * 64), rows[1]]]:
            with self.subTest(rows=invalid):
                self.write_rows('fast', invalid)
                with self.assertRaises(ValueError):
                    load_comparison(self.run, self.manifest)

    def test_rejects_inconsistent_completed_summary(self):
        summary_path = self.run / 'precise/summary.json'
        summary = json.loads(summary_path.read_text())
        summary_path.write_text(json.dumps(dict(summary, requested=3)))
        with self.assertRaisesRegex(ValueError, 'summary'):
            load_comparison(self.run, self.manifest)

    def test_rejects_rewritten_threshold_even_when_decisions_still_match(self):
        summary_path = self.run / 'fast/summary.json'
        summary = json.loads(summary_path.read_text())
        for key, value in [('fail_ratio', 0.6), ('contrast_threshold', 15)]:
            with self.subTest(key=key):
                summary_path.write_text(json.dumps(dict(summary, **{key: value})))
                with self.assertRaisesRegex(ValueError, 'config'):
                    load_comparison(self.run, self.manifest)

    def test_rejects_different_configs_for_improved_modes(self):
        config_file = self.run / 'fast/config.json'
        config_file.write_text(json.dumps(dict(schema_version=1, contrast_threshold=12, fail_ratio=0.6)))
        config_hash = hashlib.sha256(config_file.read_bytes()).hexdigest()
        summary_path = self.run / 'fast/summary.json'
        summary = json.loads(summary_path.read_text())
        summary_path.write_text(json.dumps(dict(summary, fail_ratio=0.6, config_sha256=config_hash)))
        self.write_rows('fast', [dict(row, config_sha256=config_hash) for row in self.rows('fast')])
        with self.assertRaisesRegex(ValueError, 'config'):
            load_comparison(self.run, self.manifest)

    def test_rejects_invalid_candidate_counts_ratios_and_scores(self):
        for mode, changes in (
            ('baseline', {'candidate_pixels': '11'}),
            ('fast', {'total_pixels': '11'}),
            ('fast', {'score': '0.19'}),
            ('fast', {'continuous_candidate_ratio': '0.1'}),
            ('precise', {'continuous_candidate_ratio': '1.25'}),
            ('precise', {'continuous_candidate_ratio': '0.9'}),
            ('precise', {'continuous_candidate_ratio': '0.6'}),
            ('precise', {'continuous_candidate_ratio': ''}),
        ):
            with self.subTest(mode=mode, changes=changes):
                rows = self.rows(mode)
                self.write_rows(mode, [dict(rows[0], **changes), rows[1]])
                with self.assertRaises(ValueError):
                    load_comparison(self.run, self.manifest)
                self.write_rows(mode, rows)

    def test_counts_errors_with_no_readable_hash_and_disables_threshold_comparison(self):
        for mode in ('baseline', 'fast', 'precise'):
            rows = self.rows(mode)
            row = next(row for row in rows if row['source_path'] == self.ko)
            old_decision = row['decision']
            row.update(image_sha256='', decision='ERROR', score='', error='File unavailable')
            if 'continuous_candidate_ratio' in row:
                row['continuous_candidate_ratio'] = ''
            self.write_rows(mode, rows)
            summary_path = self.run / mode / 'summary.json'
            summary = json.loads(summary_path.read_text())
            summary[old_decision] -= 1
            summary['ERROR'] += 1
            summary_path.write_text(json.dumps(summary))
        rows, _ = load_comparison(self.run, self.manifest)
        summary = summarize(rows)
        self.assertEqual(summary['by_mode']['fast']['error'], dict(count=1, denominator=2))
        self.assertEqual(summary['by_mode']['fast']['ko_pass'], dict(count=0, denominator=1))
        self.assertIsNone(summary['train_threshold_comparison'])

    def copy_reference(self):
        reference = self.run / 'reference'
        reference.mkdir()
        for mode in ('baseline', 'fast', 'precise'):
            copytree(self.run / mode, reference / mode)
        return reference

    def test_repeat_ignores_times_but_detects_changed_score_or_candidate_png(self):
        rows = self.rows('fast')
        rows[0]['artifact_path'] = 'images/000001'
        self.write_rows('fast', rows)
        images = self.run / 'fast/images/000001'
        images.mkdir(parents=True)
        for name in ('original', 'gray', 'mask', 'overlay'):
            Image.new('L', (2, 2)).save(images / f'{name}.png')
        reference = self.copy_reference()
        rows = self.rows('fast')
        self.write_rows('fast', [dict(row, analysis_ms='42') for row in rows])
        self.assertEqual(verify_repeat(self.run, reference)['rows_per_mode'], 2)
        self.write_rows('fast', [dict(rows[0], score='0.3'), rows[1]])
        with self.assertRaisesRegex(ValueError, 'rows'):
            verify_repeat(self.run, reference)
        self.write_rows('fast', rows)
        Image.new('L', (2, 2), 255).save(reference / 'fast/images/000001/mask.png')
        with self.assertRaisesRegex(ValueError, 'PNG'):
            verify_repeat(self.run, reference)

    def test_repeat_rejects_duplicate_or_unauthenticated_reference_rows(self):
        reference = self.copy_reference()
        source = reference / 'fast/results.csv'
        text = source.read_text(encoding='utf-8')
        sha = hashlib.sha256(source.read_bytes()).hexdigest()
        source.write_text(text.replace(',2,1,0', ',3,1,0'), encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'digest'):
            verify_repeat(self.run, reference, {'fast': sha})
        source.write_text(text + text.splitlines(keepends=True)[-1], encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'duplicate'):
            verify_repeat(self.run, reference)

    def test_repeat_rejects_artifacts_missing_from_both_runs(self):
        rows = self.rows('fast')
        rows[0]['artifact_path'] = 'images/000001'
        self.write_rows('fast', rows)
        reference = self.copy_reference()
        with self.assertRaisesRegex(ValueError, 'PNG'):
            verify_repeat(self.run, reference)

    def test_rejects_two_inputs_claiming_one_artifact_directory(self):
        manifest = json.loads(self.manifest.read_text())
        for row in manifest['rows']:
            row['debug'] = 1
        self.manifest.write_text(json.dumps(manifest))
        for mode in ('baseline', 'fast', 'precise'):
            rows = self.rows(mode)
            self.write_rows(mode, [dict(row, artifact_path='images/000001') for row in rows])
            images = self.run / mode / 'images/000001'
            images.mkdir(parents=True)
            for name in ('original', 'gray', 'mask', 'overlay'):
                Image.new('L', (5, 2)).save(images / f'{name}.png')
        with self.assertRaisesRegex(ValueError, 'artifact'):
            load_comparison(self.run, self.manifest)
        with self.assertRaisesRegex(ValueError, 'artifact'):
            verify_repeat(self.run, self.copy_reference())

    def test_unverified_images_are_not_attributed_to_openzdm(self):
        rows, _ = load_comparison(self.run, self.manifest)
        for mode, row in rows[0]['by_mode'].items():
            row['artifact_path'] = 'images/000001'
            directory = self.run / mode / row['artifact_path']
            directory.mkdir(parents=True)
            for name in ('original', 'mask'):
                Image.new('L', (2, 2)).save(directory / f'{name}.png')
        result = draw_examples(self.run, rows, self.run / 'examples.png')
        self.assertFalse(result['openzdm_attribution_verified'])
        self.assertEqual(result['examples'], [self.ko])

    def run_report(self, run, *options):
        return subprocess.run([sys.executable, '-W', 'error', '-m', 'tools.report_s07',
                               '--manifest', str(self.manifest), '--run', str(run), *options],
                              capture_output=True, text=True)

    def test_report_reference_uses_context_captured_before_execution(self):
        first, repeat = self.run / 'first', self.run / 'repeat'
        for run, order in [(first, ['baseline', 'fast', 'precise']), (repeat, ['precise', 'fast', 'baseline'])]:
            result = self.run_report(run, '--record-context', '--executable', sys.executable, '--order', *order)
            self.assertEqual(result.returncode, 0, result.stderr)
            for mode in order:
                copytree(self.run / mode, run / mode)
            (run / 'run-context.pending.json').rename(run / 'run-context.json')
        result = self.run_report(first)
        self.assertEqual(result.returncode, 0, result.stderr)
        result = self.run_report(repeat, '--reference', str(first))
        self.assertEqual(result.returncode, 0, result.stderr)
        summary = json.loads((repeat / 'report/development-summary.json').read_text(encoding='utf-8'))
        self.assertEqual(summary['reproducibility']['rows_per_mode'], 2)
        self.assertEqual(summary['execution_order'], ['precise', 'fast', 'baseline'])
        self.assertEqual(summary['executable_sha256'], hashlib.sha256(Path(sys.executable).read_bytes()).hexdigest())

    def test_report_cannot_assign_execution_metadata_to_existing_results(self):
        result = self.run_report(self.run, '--executable', sys.executable)
        self.assertNotEqual(result.returncode, 0)
        result = self.run_report(self.run, '--record-context', '--executable', sys.executable,
                                 '--order', 'baseline', 'fast', 'precise')
        self.assertNotEqual(result.returncode, 0)
        result = self.run_report(self.run)
        self.assertEqual(result.returncode, 0, result.stderr)
        summary = json.loads((self.run / 'report/development-summary.json').read_text(encoding='utf-8'))
        self.assertNotIn('executable_sha256', summary)
        self.assertNotIn('execution_order', summary)
        self.assertNotIn('environment', summary)
        result = self.run_report(self.copy_reference(), '--reference', str(self.run))
        self.assertIn('run context', result.stderr)
