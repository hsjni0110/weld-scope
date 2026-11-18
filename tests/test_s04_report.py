import csv
import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory
import unittest
from PIL import Image
from tools.report_s04 import aggregate, has_openzdm_provenance


class DevelopmentReportTests(unittest.TestCase):
    def test_counts_errors_and_keeps_sizes_separate(self):
        def row(label, decision, width, time):
            return dict(label=label, decision=decision, width=str(width), height='2', load_ms=str(time), analysis_ms=str(time))
        report = aggregate([row('KO', 'PASS', 2, 1), row('KO', 'FAIL', 2, 3), row('OK', 'ERROR', 0, 0), row('OK', 'PASS', 3, 10)])
        self.assertEqual(report['attempted'], 4)
        self.assertEqual(report['decisions'], dict(PASS=2, FAIL=1, ERROR=1))
        self.assertEqual(report['by_label']['KO'], dict(PASS=1, FAIL=1, ERROR=0))
        self.assertEqual(report['by_label']['OK']['ERROR'], 1)
        self.assertEqual(report['by_size']['2x2']['analysis_ms']['p50'], 2)
        self.assertEqual(report['by_size']['3x2']['analysis_ms']['p95'], 10)
        self.assertFalse(report['independent_evaluation'])


    def test_arbitrary_single_image_has_no_dataset_attribution(self):
        row = dict(source_path='C:/user/photo.png', image_sha256='a' * 64, label='')
        self.assertFalse(has_openzdm_provenance([row]))


class ReportFileTests(unittest.TestCase):
    def make_run(self, directory, size):
        run = Path(directory)
        images = run / 'images/000001'
        images.mkdir(parents=True)
        for name in ('original.png', 'gray.png', 'mask.png', 'overlay.png'):
            Image.new('RGB', size, (0, 0, 0)).save(images / name)
        row = dict(source_path='user.png', image_sha256='b' * 64, label='', decision='FAIL',
                   width=str(size[0]), height=str(size[1]), score='1', load_ms='1', analysis_ms='1',
                   artifact_path='images/000001')
        with (run / 'results.csv').open('w', encoding='utf-8', newline='') as stream:
            writer = csv.DictWriter(stream, fieldnames=row.keys())
            writer.writeheader()
            writer.writerow(row)
        (run / 'summary.json').write_text(json.dumps(dict(requested=1, PASS=0, FAIL=1, ERROR=0,
            config_sha256='a' * 64, scope='unlabelled-single-image', opencv_version='test-fixture', threads=1)))
        return run

    def report(self, run):
        script = Path(__file__).resolve().parents[1] / 'tools/report_s04.py'
        return subprocess.run([sys.executable, '-W', 'error', str(script), '--run', str(run)], capture_output=True)

    def test_extreme_aspect_ratios_produce_real_report_images(self):
        for size in ((1, 1000), (1000, 1)):
            with self.subTest(size=size), TemporaryDirectory() as directory:
                run = self.make_run(directory, size)
                result = self.report(run)
                self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
                with Image.open(run / 'report/examples.png') as image:
                    image.load()
                    self.assertEqual(image.size, (1280, 420))

    def test_failed_report_can_retry_and_completed_report_is_not_overwritten(self):
        with TemporaryDirectory() as directory:
            run = self.make_run(directory, (2, 2))
            missing = run / 'images/000001/overlay.png'
            missing.unlink()
            self.assertNotEqual(self.report(run).returncode, 0)
            self.assertFalse((run / 'report').exists())
            Image.new('RGB', (2, 2)).save(missing)
            result = self.report(run)
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors='replace'))
            before = (run / 'report/examples.png').read_bytes()
            self.assertNotEqual(self.report(run).returncode, 0)
            self.assertEqual((run / 'report/examples.png').read_bytes(), before)


if __name__ == '__main__': unittest.main()
