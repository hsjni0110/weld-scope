"""Audit frozen OPENZDM scores, split membership, width routing and final decisions."""

import argparse
import subprocess
from collections import Counter, defaultdict
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from tools.evaluate_openzdm_patterns import INPUTS, calibrate, counts, manifest
from tools.evaluate_openzdm_policy import auc, ORIGINAL, ROOT, classify, read_json, read_rows, run, sha, verify_live, write_json


STUDY = ROOT / 'artifacts/OPENZDM-EDA'
EVIDENCE = ROOT / 'docs/algorithms/evidence'
PACKAGE = EVIDENCE / 'openzdm-spatial-model'
MODEL = PACKAGE / 'config.json'
RUNNER = STUDY / 'budget-build/Release/openzdm_shape.exe'
INSIDE = range(385, 416)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def indexed(rows):
    index = {row['path']: row for row in rows}
    require(len(index) == len(rows), 'Duplicate image path')
    return index


def side(row):
    return 'inside' if int(row['width']) in INSIDE else 'outside'


def compare_splits(catalog, fit, calibration, validation, test, primary):
    excluded = {r['path'] for r in read_json(INPUTS / 'excluded.json')}
    reviewed = {r['path'] for r in read_rows(STUDY / 'regions-verified/answer-key.csv')}
    require(set(primary) == set(test) - excluded, 'Unexpected primary test membership')
    val_paths = {r['path'] for r in catalog.values() if r['split'] == 'validation'}
    require(set(validation) == val_paths - excluded - reviewed, 'Unexpected validation membership')
    train_paths = {r['path'] for r in catalog.values() if r['split'] == 'train'}
    require(set(fit).isdisjoint(calibration) and set(fit) | set(calibration) == train_paths,
            'Unexpected fit or calibration membership')
    seen = set()
    counts = {}
    for name, rows in [('fit', fit), ('calibration', calibration), ('validation', validation), ('official_test', test)]:
        pixels = set()
        for path, row in rows.items():
            source = catalog[path]
            require(row['sha256'] == source['file_sha256'] and row['label'] == source['label'], path)
            pixels.add(source['pixel_sha256'])
        require(pixels.isdisjoint(seen), 'Identical decoded pixels across cohorts')
        seen.update(pixels)
        counts[name] = len(rows)
    return counts | {'primary_test': len(primary), 'excluded_test': len(test) - len(primary),
                     'reviewed_validation': len(reviewed)}


def pooled_scores(out, fit, validation_path):
    source = read_json(STUDY / 'pattern-converged/fit.json')
    root = Path(source['root'])
    require(root == Path(read_json(STUDY / 'width-study/fit.json')['root']), 'Train image roots differ')
    manifest([fit[k] for k in sorted(fit)], root, out / 'pooled-fit.json')
    config = read_json(PACKAGE / 'outside.json') | {'forests': 'pooled-forest'}
    write_json(out / 'pooled.json', config)
    subprocess.run([RUNNER, 'fit-whole', out / 'pooled-fit.json', out / 'pooled.json', out / 'pooled-forest'],
                   check=True)
    return indexed(run(RUNNER, 'whole', validation_path, out / 'pooled.json', out / 'pooled-validation.csv'))


def rank_comparison(local, pooled):
    require(local.keys() == pooled.keys(), 'Validation cohorts differ')
    rows = []
    for path, row in local.items():
        other = pooled[path]
        require(all(row[k] == other[k] for k in ('label', 'width', 'height')), path)
        rows.append(dict(row, local=float(row['fast_score']), pooled=float(other['fast_score']),
                         width_only=int(side(row) == 'inside')))
    report = {}
    for area in ('all', 'inside', 'outside'):
        cohort = [r for r in rows if area == 'all' or side(r) == area]
        groups = defaultdict(list)
        for r in cohort:
            groups[(r['width'], r['height'])].append(r)
        pairs = used = 0
        correct = Counter()
        for group in groups.values():
            labels = Counter(r['label'] for r in group)
            weight = labels['KO'] * labels['OK']
            if weight:
                used += len(group)
                pairs += weight
                for key in ('local', 'pooled'):
                    correct[key] += weight * auc(group, key, 1)
        report[area] = {'n': len(cohort), 'auc': {k: auc(cohort, k, 1) for k in ('local', 'pooled', 'width_only')},
                        'same_size_pairs': pairs, 'images_in_mixed_sizes': used,
                        'same_size_auc': {k: value / pairs for k, value in correct.items()}}
    return report


def pooled_policy(out, validation_path, catalog):
    precise = indexed([dict(r, path=r['source_path']) for r in
                       read_rows(ROOT / 'artifacts/S07/review-train-01/precise/results.csv')])
    draft = read_json(out / 'pooled.json')
    policies = {}
    for area, source in [('inside', STUDY / 'pattern-converged/calibration.json'),
                         ('outside', STUDY / 'width-study/cal.json')]:
        rows = run(RUNNER, 'whole', source, out / 'pooled.json', out / f'cal-{area}.csv')
        for r in rows:
            p = precise[r['path']]
            require(p['image_sha256'] == catalog[r['path']]['file_sha256'] and p['label'] == r['label'], r['path'])
            r['precise_score'] = p['score']
        labels = Counter(r['label'] for r in rows)
        if area == 'inside':
            target = {'KO/PASS': labels['KO'] * 243 / 1346, 'OK/FAIL': labels['OK'] * 598 / 1829}
        else:
            baseline = read_rows(STUDY / 'width-study/cal-original.csv')
            for r, c in zip(baseline, rows, strict=True):
                require(r['path'] == c['path'] and r['label'] == c['label'], r['path'])
                r['precise_score'] = c['precise_score']
            target = counts(baseline, read_json(ORIGINAL), 'fast_score')
        policies[area] = calibrate(rows, 'fast_score', draft, target, 7)
    pooled_cfg = out / 'pooled-integrated.json'
    write_json(out / 'pooled-outside.json', policies['outside'])
    write_json(pooled_cfg, dict(policies['inside'], all_widths=0, outside_config='pooled-outside.json'))
    reports = []
    for trial, order in enumerate([('local', 'pooled'), ('pooled', 'local')], 1):
        report = {}
        for name in order:
            config = MODEL if name == 'local' else pooled_cfg
            rows = run(RUNNER, 'whole', validation_path, config, out / f'validation-{trial}-{name}.csv')
            report[name] = {}
            for area, file in [('inside', config), ('outside', config.parent / read_json(config)['outside_config'])]:
                group = [r for r in rows if side(r) == area]
                verify_live(group, read_json(file))
                require(all(r['decision'] != 'ERROR' for r in group), 'Error in validation')
                report[name][area] = dict(Counter(r['label'] + '/' + r['decision'] for r in group))
                report[name][area]['precise_calls'] = sum(bool(r['precise_ns']) for r in group)
        reports.append(report)
    return reports


def fast_action(row, config):
    return classify(float(row['fast_score']), config['fast_pass_max'], config['fast_fail_min'])


def boundary_experiment(out, local, validation):
    configs = {'inside': read_json(MODEL), 'outside': read_json(PACKAGE / 'outside.json')}
    source_root = Path(validation['root'])
    generated = out / 'edge-images'
    generated.mkdir()
    changes = {384: 1, 385: -1, 415: 1, 416: -1}
    samples = []
    originals = {}
    for source in validation['rows']:
        original = local[source['path']]
        width = int(original['width'])
        if width not in changes:
            continue
        source_path = source_root / source['path']
        require(sha(source_path) == source['sha256'], source['path'])
        with Image.open(source_path) as image:
            require(image.width == width, source['path'])
            width_after = width + changes[width]
            edited = image.crop((0, 0, width_after, image.height))
            if width_after > width:
                edited.paste(image.crop((width - 1, 0, width, image.height)), (width, 0))
            name = f"edge-{len(samples):03}.png"
            edited.save(generated / name)
        originals[name] = original
        samples.append(dict(source, path=name, sha256=sha(generated / name)))
    input_file = out / 'edge.json'
    manifest(samples, generated, input_file)
    routed = run(RUNNER, 'whole', input_file, MODEL, out / 'edge-routed.csv')
    forced = {}
    for area, config in configs.items():
        fixed = {k: v for k, v in config.items() if k != 'outside_config'}
        fixed.update(all_widths=1, forests=str(PACKAGE / config['forests']))
        cfg = out / f'force-{area}.json'
        write_json(cfg, fixed)
        forced[area] = indexed(run(RUNNER, 'whole', input_file, cfg, out / f'edge-{area}.csv'))
    cases = []
    for row in routed:
        name = row['path']
        original = originals[name]
        before, after = side(original), side(row)
        require(before != after and row['fast_score'] == forced[after][name]['fast_score'], name)
        cases.append({'source': original['path'], 'width': int(original['width']), 'label': row['label'],
                      'original': fast_action(original, configs[before]),
                      'edited_fixed': fast_action(forced[before][name], configs[before]),
                      'edited_routed': fast_action(row, configs[after])})
    return cases


def test_report(out, test, primary):
    archive = STUDY / 'width-budget'
    first = indexed(read_rows(archive / 'test-1-half48.csv'))
    second = indexed(read_rows(archive / 'test-2-half48.csv'))
    baseline = indexed(read_rows(archive / 'test-1-S12.csv'))
    require(list(first) == list(second) == list(baseline) == list(test), 'Archived input order differs')
    reverse = out / 'reverse.json'
    manifest(list(reversed(list(test.values()))), INPUTS / 'images', reverse)
    fresh = indexed(run(RUNNER, 'whole', reverse, MODEL, out / 'reverse.csv'))
    require(list(fresh) == list(reversed(first)), 'Input reversal failed')
    configs = {'inside': read_json(MODEL), 'outside': read_json(PACKAGE / 'outside.json')}
    for area, config in configs.items():
        verify_live([r for r in fresh.values() if side(r) == area], config)
    for path, r in fresh.items():
        require(r['fast_score'] == first[path]['fast_score'] == second[path]['fast_score'], path)
        require(r['label'] == test[path]['label'], path)
    report = {}
    for name, data in [('S12', baseline), ('first', first), ('second', second), ('reversed_photos', fresh)]:
        subset = [data[p] for p in primary]
        table = Counter((r['label'], r['decision']) for r in subset)
        report[name] = {'counts': {label: {d: table[label, d] for d in ('PASS', 'FAIL', 'REVIEW', 'ERROR')}
                                   for label in ('KO', 'OK')},
                        'different_vs_first': sum(r['decision'] != first[r['path']]['decision'] for r in subset),
                        'precise_calls': sum(bool(r['precise_ns']) for r in subset)}
    return report


def draw_boundary_examples(out, cases, root):
    # Show a normal and a defective source from the upper boundary. Arrows mark the edited column.
    examples = ['val/OK/image_1214.png', 'val/KO/image_420.png']
    canvas = Image.new('RGB', (920, 348), 'white')
    draw = ImageDraw.Draw(canvas)
    font = ImageFont.load_default(size=15)
    title = ImageFont.load_default(size=19)
    draw.text((14, 8), 'One-column width change | frozen fast-stage actions', font=title, fill='black')
    for band, path in enumerate(examples):
        index, case = next((i, c) for i, c in enumerate(cases) if c['source'] == path)
        top = 45 + 150 * band
        draw.text((14, top), path, font=title, fill='black')
        paths = [root / path, out / 'edge-images' / f'edge-{index:03}.png']
        for left, source in zip((14, 478), paths, strict=True):
            with Image.open(source) as img:
                canvas.paste(img.convert('RGB'), (left, top + 32))
                width = img.width
                caption = f'{width}px | original: {case["original"]}'
                if left == 478:
                    caption = f'{width}px | same policy: {case["edited_fixed"]} | routed: {case["edited_routed"]}'
                    x = left + width - 1
                    draw.polygon([(x, top + 30), (x - 4, top + 23), (x + 4, top + 23)], fill='#b43521')
                draw.text((left, top + 95), caption, font=font, fill='black')
    canvas.save(out / 'boundary-examples.png')


def trace_digests(out):
    return {file.name: sha(file) for file in sorted(out.glob('*.csv'))}


def main(out):
    out.mkdir(parents=True, exist_ok=False)
    saved = read_json(EVIDENCE / 'openzdm-eda-spatial-policy.json')
    require(sha(RUNNER) == saved['package']['runner'], 'Runner changed')
    for name, digest in saved['package']['files'].items():
        require(sha(PACKAGE / name) == digest, 'Model package changed')
    for name, digest in saved['stages']['width-budget']['traces'].items():
        require(sha(STUDY / 'width-budget' / name) == digest, 'Frozen trace changed')
    fit = indexed(read_json(STUDY / 'pattern-converged/fit.json')['rows']
                  + read_json(STUDY / 'width-study/fit.json')['rows'])
    cal = indexed(read_json(STUDY / 'pattern-converged/calibration.json')['rows']
                  + read_json(STUDY / 'width-study/cal.json')['rows'])
    validation_path = STUDY / 'width-budget/validation.json'
    validation = read_json(validation_path)
    test = indexed(read_json(INPUTS / 'test.json')['rows'])
    primary = indexed(read_json(INPUTS / 'test-primary.json')['rows'])
    catalog = indexed(read_rows(ROOT / 'artifacts/S02/run-01/manifest.csv'))
    splits = compare_splits(catalog, fit, cal, indexed(validation['rows']), test, primary)
    pooled = pooled_scores(out, fit, validation_path)
    local = indexed(read_rows(STUDY / 'width-budget/validation-half48.csv'))
    rank = rank_comparison(local, pooled)
    comparison = pooled_policy(out, validation_path, catalog)
    boundary = boundary_experiment(out, local, validation)
    draw_boundary_examples(out, boundary, Path(validation['root']))
    test_counts = test_report(out, test, primary)
    write_json(out / 'audit.json', {'splits': splits, 'validation_ranking': rank, 'boundary_cases': boundary,
                                   'pooled_validation_decisions': comparison,
                                   'primary_test': test_counts, 'official_identical_fast_scores': len(test),
                                   'trace_sha256': trace_digests(out),
                                   'frozen_runner_sha256': sha(RUNNER),
                                   'pooled_model_sha256': sha(out / 'pooled-forest/whole.xml.gz')})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=Path)
    main(parser.parse_args().output)
