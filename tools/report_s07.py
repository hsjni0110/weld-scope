"""Compare completed S07 runs on one verified train manifest."""
import argparse
from bisect import bisect_left
from collections import Counter, defaultdict
import csv
import ctypes
import hashlib
import json
import math
import os
from pathlib import Path
import platform
from tempfile import TemporaryDirectory

from PIL import Image, ImageDraw, ImageFont

from tools.report_s04 import has_openzdm_provenance, percentiles


VERSIONS = {
    'baseline': 'global-dark-ratio-v1',
    'fast': 'local-contrast-fast-v1',
    'precise': 'local-contrast-precise-v1',
}
DECISIONS = ('PASS', 'FAIL', 'ERROR')
TIMINGS = ('load_ms', 'analysis_ms', 'artifact_ms')
ARTIFACTS = ('original.png', 'gray.png', 'mask.png', 'overlay.png')
RUN_CONTEXT = 'run-context.json'
PENDING_CONTEXT = 'run-context.pending.json'
CONTEXT_FIELDS = ('manifest_sha256', 'executable_sha256', 'environment', 'execution_order')


def sha256(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read_rows(path):
    with path.open(encoding='utf-8', newline='') as stream:
        return list(csv.DictReader(stream))


def artifact_files(directory, rows):
    directories = [row['artifact_path'] for row in rows if row['artifact_path']]
    if len(directories) != len(set(directories)):
        raise ValueError(f'{directory}: duplicate image artifact directory')
    expected = {Path(folder) / name for folder in directories for name in ARTIFACTS}
    actual = {path.relative_to(directory) for path in (directory / 'images').rglob('*.png')}
    if actual != expected:
        raise ValueError(f'{directory}: unexpected or missing PNG artifacts')
    return actual


def validate_score(mode, row, width, height, fail_ratio):
    total = int(row['total_pixels'])
    candidates = int(row['candidate_pixels'])
    if min(width, height) < 1 or total != width * height or not 0 <= candidates <= total:
        raise ValueError('invalid image or candidate pixel count')
    candidate_ratio = candidates / total
    expected_score = candidate_ratio
    continuous = row.get('continuous_candidate_ratio', '')
    if mode == 'precise':
        continuous_ratio = float(continuous)
        if not 0 <= continuous_ratio <= candidate_ratio:
            raise ValueError('continuous candidate ratio outside the candidate area')
        expected_score = (candidate_ratio + continuous_ratio) / 2
    elif continuous:
        raise ValueError('unexpected continuous candidate ratio')
    score = float(row['score'])
    decision = 'FAIL' if score >= fail_ratio else 'PASS'
    if (not 0 <= score <= 1 or not math.isclose(score, expected_score, rel_tol=0, abs_tol=1e-15)
            or row['decision'] != decision):
        raise ValueError('score or decision disagrees with the recorded features')


def load_comparison(run, manifest_path):
    expected = {}
    for row in json.loads(manifest_path.read_text(encoding='utf-8'))['rows']:
        if row['path'] in expected or row['split'] != 'train' or row['label'] not in ('KO', 'OK'):
            raise ValueError(f"Invalid train manifest row: {row['path']}")
        expected[row['path']] = row
    if not expected:
        raise ValueError('Empty train manifest')

    summaries, by_mode = {}, {}
    for mode, version in VERSIONS.items():
        directory = run / mode
        summary = json.loads((directory / 'summary.json').read_text(encoding='utf-8'))
        config_path = directory / 'config.json'
        config_hash = sha256(config_path)
        config = json.loads(config_path.read_text(encoding='utf-8'))
        threshold_key = 'intensity_threshold' if mode == 'baseline' else 'contrast_threshold'
        threshold = config.get(threshold_key)
        fail_ratio = config.get('fail_ratio')
        if (config.get('schema_version') != 1 or type(threshold) is not int or not 0 <= threshold <= 255
                or type(fail_ratio) not in (int, float) or not 0 <= fail_ratio <= 1
                or summary.get(threshold_key) != threshold or summary.get('fail_ratio') != fail_ratio):
            raise ValueError(f'{mode}: summary thresholds disagree with config')
        rows = read_rows(directory / 'results.csv')
        counts = Counter(row['decision'] for row in rows)
        if (summary['algorithm_version'] != version or summary['scope'] != 'train-development-only'
                or summary['independent_evaluation'] != 0 or summary['config_sha256'] != config_hash
                or summary['requested'] != len(rows) or set(counts).difference(DECISIONS)
                or any(summary[decision] != counts[decision] for decision in DECISIONS)):
            raise ValueError(f'{mode}: invalid or inconsistent completed summary')
        indexed = {row['source_path']: row for row in rows}
        if len(indexed) != len(rows) or indexed.keys() != expected.keys():
            raise ValueError(f'{mode}: missing, duplicate or unexpected input rows')
        if any(row['config_sha256'] != config_hash for row in rows):
            raise ValueError(f'{mode}: row configuration hash mismatch')
        summaries[mode] = dict(summary, results_csv_sha256=sha256(directory / 'results.csv'))
        by_mode[mode] = indexed

    if len({(item['opencv_version'], item['threads']) for item in summaries.values()}) != 1:
        raise ValueError('OpenCV version or thread count mismatch')
    if summaries['fast']['config_sha256'] != summaries['precise']['config_sha256']:
        raise ValueError('Fast and precise config hashes differ')
    comparison = []
    for ordinal, (path, item) in enumerate(sorted(expected.items()), start=1):
        modes = {mode: run_rows[path] for mode, run_rows in by_mode.items()}
        hashes = {row['image_sha256'] for row in modes.values()}
        all_unreadable = hashes == {''} and all(row['decision'] == 'ERROR' for row in modes.values())
        if hashes != {item['sha256']} and not all_unreadable:
            raise ValueError(f'Input hash mismatch: {path}')
        dimensions = set()
        for mode, row in modes.items():
            if (row['label'], row['split']) != (item['label'], item['split']):
                raise ValueError(f'{mode}: label or split mismatch: {path}')
            artifact = f'images/{ordinal:06d}' if item['debug'] == 1 and row['decision'] != 'ERROR' else ''
            if row['artifact_path'] != artifact:
                raise ValueError(f'{mode}: artifact directory disagrees with manifest position: {path}')
            if row['decision'] == 'ERROR':
                if not row['error'] or row['score'] or row.get('continuous_candidate_ratio'):
                    raise ValueError(f'{mode}: invalid ERROR row: {path}')
            else:
                try:
                    width, height = int(row['width']), int(row['height'])
                    validate_score(mode, row, width, height, summaries[mode]['fail_ratio'])
                except (TypeError, ValueError) as error:
                    raise ValueError(f'{mode}: invalid result for {path}: {error}') from error
                dimensions.add((width, height))
            if any(not math.isfinite(float(row[field])) or float(row[field]) < 0 for field in TIMINGS):
                raise ValueError(f'{mode}: invalid time: {path}')
        if len(dimensions) > 1:
            raise ValueError(f'Image dimensions differ: {path}')
        comparison.append(dict(source_path=path, label=item['label'],
                               image_sha256='' if all_unreadable else item['sha256'], by_mode=modes))
    for mode, indexed in by_mode.items():
        artifact_files(run / mode, indexed.values())
    return comparison, summaries


def summarize(rows):
    labels = Counter(row['label'] for row in rows)
    by_mode, transitions = {}, {}
    for mode in VERSIONS:
        counts = {label: dict.fromkeys(DECISIONS, 0) for label in ('KO', 'OK')}
        timings = defaultdict(lambda: {field: [] for field in TIMINGS})
        for item in rows:
            row = item['by_mode'][mode]
            counts[item['label']][row['decision']] += 1
            if row['decision'] != 'ERROR':
                size = f"{row['width']}x{row['height']}"
                for field in TIMINGS:
                    if field != 'artifact_ms' or row['artifact_path']:
                        timings[size][field].append(float(row[field]))
        by_size = {
            size: dict(count=len(times['analysis_ms']), artifact_count=len(times['artifact_ms']),
                       **{field: percentiles(values) for field, values in times.items() if values})
            for size, times in sorted(timings.items())
        }
        by_mode[mode] = dict(
            by_label=counts, by_size=by_size,
            ko_pass=dict(count=counts['KO']['PASS'], denominator=labels['KO']),
            ok_fail=dict(count=counts['OK']['FAIL'], denominator=labels['OK']),
            error=dict(count=sum(group['ERROR'] for group in counts.values()), denominator=len(rows)),
        )
        if mode != 'baseline':
            changes = {f'{label} {before}->{after}': 0 for label in ('KO', 'OK')
                       for before, after in [('PASS', 'FAIL'), ('FAIL', 'PASS')]}
            for item in rows:
                baseline = item['by_mode']['baseline']['decision']
                decision = item['by_mode'][mode]['decision']
                change = f"{item['label']} {baseline}->{decision}"
                if change in changes:
                    changes[change] += 1
            transitions[mode] = changes

    threshold_comparison = None
    if labels['KO'] and labels['OK'] and not any(mode['error']['count'] for mode in by_mode.values()):
        threshold_comparison = {}
        ko_pass_limit = by_mode['baseline']['ko_pass']['count']
        for mode in VERSIONS:
            scores = {label: sorted(float(row['by_mode'][mode]['score']) for row in rows if row['label'] == label)
                      for label in ('KO', 'OK')}
            # Crossing this KO score would exceed the leak limit. Ties stay FAIL.
            threshold = scores['KO'][ko_pass_limit] if ko_pass_limit < labels['KO'] else 1.0
            threshold_comparison[mode] = dict(threshold=threshold, ko_pass=bisect_left(scores['KO'], threshold),
                                              ok_fail=labels['OK'] - bisect_left(scores['OK'], threshold))
    return dict(by_mode=by_mode, transitions=transitions, train_threshold_comparison=threshold_comparison)


def verify_repeat(run, reference, reference_digests=None):
    def stable_rows(path):
        rows = read_rows(path)
        indexed = {row['source_path']: {key: value for key, value in row.items() if key not in TIMINGS}
                   for row in rows}
        if len(rows) != len(indexed):
            raise ValueError(f'{path}: duplicate repeat rows')
        return indexed

    def png_hashes(directory, rows):
        return {path: sha256(directory / path) for path in artifact_files(directory, rows.values())}

    png_count = 0
    for mode in VERSIONS:
        left, right = run / mode, reference / mode
        for name in ('config.json', 'summary.json'):
            if sha256(left / name) != sha256(right / name):
                raise ValueError(f'{mode}: repeat {name} differs')
        right_csv = right / 'results.csv'
        if reference_digests and mode in reference_digests and sha256(right_csv) != reference_digests[mode]:
            raise ValueError(f'{mode}: reference CSV digest differs from its report')
        rows = stable_rows(left / 'results.csv')
        if rows != stable_rows(right_csv):
            raise ValueError(f'{mode}: repeat non-timing rows differ')
        hashes = png_hashes(left, rows)
        if hashes != png_hashes(right, rows):
            raise ValueError(f'{mode}: repeat PNG artifacts differ')
        png_count += len(hashes)
    return dict(reference=str(reference), rows_per_mode=len(rows), png_files=png_count)


def draw_examples(run, rows, destination):
    paths = ('train/OK/image_1.png', 'train/KO/image_100.png')
    visual_rows = [item for item in rows if all(row['artifact_path'] for row in item['by_mode'].values())]
    selected = {item['source_path']: item for path in paths for item in visual_rows if item['source_path'] == path}
    for category in ('교정', '오판 유지', '새 오판'):
        for item in visual_rows:
            if item['source_path'] in selected:
                continue
            expected = 'PASS' if item['label'] == 'OK' else 'FAIL'
            baseline_correct = item['by_mode']['baseline']['decision'] == expected
            outcomes = [item['by_mode'][mode]['decision'] == expected for mode in ('fast', 'precise')]
            present = {'교정': not baseline_correct and any(outcomes),
                       '오판 유지': not baseline_correct and not all(outcomes),
                       '새 오판': baseline_correct and not all(outcomes)}
            if present[category]:
                selected[item['source_path']] = item
                break
    if not selected and visual_rows:
        selected[visual_rows[0]['source_path']] = visual_rows[0]
    attributed = has_openzdm_provenance([item['by_mode']['baseline'] for item in selected.values()])
    evidence = dict(examples=list(selected), openzdm_attribution_verified=attributed)
    if not selected:
        return evidence

    font = ImageFont.truetype('C:/Windows/Fonts/malgun.ttf', 18)
    title = ImageFont.truetype('C:/Windows/Fonts/malgunbd.ttf', 25)
    row_height, top_margin = 148, 102
    footer = top_margin + row_height * len(selected)
    canvas = Image.new('RGB', (1280, footer + 102), '#ffffff')
    draw = ImageDraw.Draw(canvas)
    draw.text((24, 18), 'S07 분석 비교', font=title, fill='#172334')
    headers = ('원본', 'S04 기준 후보', '빠른 분석 후보', '정밀 분석 후보')
    for column, header in enumerate(headers):
        draw.text((24 + column * 314, 64), header, font=font, fill='#172334')
    for index, item in enumerate(selected.values()):
        top = top_margin + index * row_height
        draw.text((24, top), f"{item['source_path']}   원본 라벨 {item['label']}", font=font, fill='#172334')
        mode_rows = item['by_mode']
        panels = [('baseline', 'original.png'), *((mode, 'mask.png') for mode in VERSIONS)]
        for column, (mode, name) in enumerate(panels):
            x = 24 + column * 314
            row = mode_rows[mode]
            with Image.open(run / mode / row['artifact_path'] / name) as source:
                panel = source.convert('RGB')
            scale = min(294 / panel.width, 78 / panel.height)
            size = (max(1, round(panel.width * scale)), max(1, round(panel.height * scale)))
            panel = panel.resize(size, Image.Resampling.BILINEAR if column == 0 else Image.Resampling.NEAREST)
            draw.rectangle((x, top + 30, x + 293, top + 107), fill='#131c29')
            canvas.paste(panel, (x + (294 - size[0]) // 2, top + 30 + (78 - size[1]) // 2))
            if column:
                decision = {'PASS': '정상', 'FAIL': '불량'}[row['decision']]
                draw.text((x, top + 112), f"판정 {decision}   점수 {float(row['score']):.4f}", font=font, fill='#172334')
    attribution_lines = (
        'OPENZDM  |  DOI 10.5281/zenodo.14025630  |  CC BY 4.0',
        '2D image dataset for the presence or absence of aesthetical defects on EV battery welds.',
        '원본 비율 유지 확대, 이상 후보 마스크 추가. 공식 train 개발 비교.',
    ) if attributed else ('입력 출처 미확인', '원본 비율 유지 확대, 이상 후보 마스크 추가.')
    for index, line in enumerate(attribution_lines):
        draw.text((24, footer + 8 + index * 25), line, font=font, fill='#172334')
    canvas.save(destination)
    return evidence


def environment():
    class MemoryStatus(ctypes.Structure):
        _fields_ = [('length', ctypes.c_uint32), ('load_percent', ctypes.c_uint32),
                    ('total_physical', ctypes.c_uint64), ('other_amounts', ctypes.c_uint64 * 6)]

    memory = MemoryStatus(length=ctypes.sizeof(MemoryStatus))
    has_memory = ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(memory))
    return dict(windows=list(platform.win32_ver()), machine=platform.machine(), processor=platform.processor(),
                logical_processors=os.cpu_count(), total_physical_bytes=memory.total_physical if has_memory else None,
                configuration='Release', timing_samples_per_input=1)


def record_context(run, manifest, executable, order):
    context = dict(manifest_sha256=sha256(manifest), executable_sha256=sha256(executable),
                   environment=environment(), execution_order=order)
    run.mkdir(parents=True, exist_ok=False)
    (run / PENDING_CONTEXT).write_text(json.dumps(context, indent=2) + '\n', encoding='utf-8')


def read_context(run, manifest_hash):
    path = run / RUN_CONTEXT
    if not path.is_file():
        return {}
    context = json.loads(path.read_text(encoding='utf-8'))
    if set(context) != set(CONTEXT_FIELDS) or sorted(context['execution_order']) != sorted(VERSIONS):
        raise ValueError(f'Invalid run context: {path}')
    if context['manifest_sha256'] != manifest_hash:
        raise ValueError(f'Run context manifest SHA-256 mismatch: {path}')
    return context


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--executable', type=Path)
    parser.add_argument('--reference', type=Path)
    parser.add_argument('--record-context', action='store_true')
    parser.add_argument('--order', nargs=len(VERSIONS), choices=tuple(VERSIONS))
    args = parser.parse_args()
    if args.record_context:
        if not args.executable or sorted(args.order or []) != sorted(VERSIONS) or args.reference:
            parser.error('--record-context requires --executable and all three modes in --order')
        record_context(args.run, args.manifest, args.executable, args.order)
        return
    if args.executable or args.order:
        parser.error('--executable and --order are only used with --record-context')
    output = args.run / 'report'
    if output.exists():
        raise FileExistsError(f'Completed report already exists: {output}')
    manifest_hash = sha256(args.manifest)
    context = read_context(args.run, manifest_hash)
    reference_context = read_context(args.reference, manifest_hash) if args.reference else {}
    if args.reference and not (context and reference_context):
        raise ValueError('Repeat comparison requires a completed run context for both runs')
    rows, summaries = load_comparison(args.run, args.manifest)
    report = dict(requested=len(rows), scope='train-development-only', independent_evaluation=False,
                  manifest_sha256=manifest_hash, runs=summaries, **summarize(rows))
    report.update(context)
    if args.reference:
        reference = json.loads((args.reference / 'report/development-summary.json').read_text(encoding='utf-8'))
        for key in CONTEXT_FIELDS:
            if reference.get(key) != reference_context[key]:
                raise ValueError(f'Reference report {key} differs from its run context')
        for key in ('executable_sha256', 'environment'):
            if context[key] != reference_context[key]:
                raise ValueError(f'Repeat {key} differs')
        report['reproducibility'] = verify_repeat(
            args.run, args.reference, {mode: reference['runs'][mode]['results_csv_sha256'] for mode in VERSIONS})
    with TemporaryDirectory(prefix='.report-preparing-', dir=args.run) as temporary:
        staging = Path(temporary)
        compared_fields = ('width', 'height', 'candidate_pixels', 'total_pixels', 'continuous_candidate_ratio',
                           'score', 'decision', 'error', *TIMINGS)
        columns = ('source_path', 'label', 'image_sha256')
        with (staging / 'comparison.csv').open('w', encoding='utf-8', newline='') as stream:
            writer = csv.writer(stream)
            writer.writerow((*columns, *(f'{mode}_{field}' for mode in VERSIONS for field in compared_fields)))
            for item in rows:
                writer.writerow([item[key] for key in columns]
                                + [row.get(field, '') for row in item['by_mode'].values() for field in compared_fields])
        report.update(draw_examples(args.run, rows, staging / 'examples.png'))
        (staging / 'development-summary.json').write_text(
            json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        staging.rename(output)
    print(f'Compared {len(rows)} train images: {output}')


if __name__ == '__main__':
    main()
