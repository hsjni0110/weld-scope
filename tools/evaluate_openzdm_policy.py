"""Freeze validation-only EDA policies and measure them with the C++ policy runner."""

import argparse
import csv
import hashlib
import itertools
import json
import math
import platform
import subprocess
from bisect import bisect_left, bisect_right
from collections import Counter
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ORIGINAL = ROOT / 'docs/steps/S11-evidence/config/selective.json'
FIELDS = ('fast_pass_max', 'fast_fail_min', 'precise_pass_max', 'precise_fail_min')
LABELS = ('OK', 'KO')
DECISIONS = ('PASS', 'FAIL', 'REVIEW', 'ERROR')


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'))


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def read_rows(path):
    with path.open(newline='', encoding='utf-8-sig') as file:
        return list(csv.DictReader(file))


def auc(rows, field, direction):
    ok = sorted(direction * r[field] for r in rows if r['label'] == 'OK')
    ko = [direction * r[field] for r in rows if r['label'] == 'KO']
    rank = sum((bisect_left(ok, value) + bisect_right(ok, value)) / 2 for value in ko)
    return rank / (len(ok) * len(ko))


def rank(values, percentile):
    return values[max(0, math.ceil(len(values) * percentile / 100) - 1)]


def mask(check):
    return int.from_bytes(bytes(bool(value) for value in check), 'little')


class Observations:
    def __init__(self, rows, feature, budget_ns):
        self.fast = [float(row[f'{feature}_score']) for row in rows]
        self.precise = [float(row['precise_score']) for row in rows]
        fast_time = [int(row[f'{feature}_ns']) for row in rows]
        self.ok = mask(row['label'] == 'OK' for row in rows)
        self.ko = mask(row['label'] == 'KO' for row in rows)
        self.fast_timely = mask(time <= budget_ns for time in fast_time)
        self.can_start_precise = mask(time < budget_ns for time in fast_time)
        self.precise_timely = mask(time + int(row['precise_ns']) <= budget_ns
                                   for time, row in zip(fast_time, rows, strict=True))

    def fast_masks(self, pass_max, fail_min):
        passed = mask(score <= pass_max for score in self.fast)
        failed = mask(score >= fail_min for score in self.fast)
        precise = self.can_start_precise & ~(passed | failed)
        return passed & self.fast_timely, failed & self.fast_timely, precise

    def count(self, fast, precise_pass, precise_fail):
        fast_pass, fast_fail, precise = fast
        on_time = precise & self.precise_timely
        passed = fast_pass | (on_time & precise_pass)
        failed = fast_fail | (on_time & precise_fail)
        return ((passed & self.ko).bit_count(), (failed & self.ok).bit_count(),
                (passed & self.ok).bit_count(), (passed | failed).bit_count(), precise.bit_count())

    def evaluate(self, config):
        fp, ff, pp, pf = (config[field] for field in FIELDS)
        return self.count(self.fast_masks(fp, ff), mask(score <= pp for score in self.precise),
                          mask(score >= pf for score in self.precise))


def grid(values, original):
    values = sorted(values)
    return sorted({0, 1, *original, *(rank(values, p) for p in range(5, 100, 5))})


def fit(obs, config, limits):
    min_auto, max_calls, min_ok_pass, max_ok_fail = limits
    fast = grid(obs.fast, [config[field] for field in FIELDS[:2]])
    precise = grid(obs.precise, [config[field] for field in FIELDS[2:]])
    pp_masks = {t: mask(score <= t for score in obs.precise) for t in precise}
    pf_masks = {t: mask(score >= t for score in obs.precise) for t in precise}
    best = None
    for fp, ff in itertools.combinations(fast, 2):
        fast_masks = obs.fast_masks(fp, ff)
        if fast_masks[2].bit_count() > max_calls:
            continue
        for pp, pf in itertools.combinations(precise, 2):
            ko_pass, ok_fail, ok_pass, automatic, calls = obs.count(fast_masks, pp_masks[pp], pf_masks[pf])
            if ok_fail <= max_ok_fail and ok_pass >= min_ok_pass and automatic >= min_auto:
                candidate = (ko_pass, ok_fail, -ok_pass, -automatic, calls, (fp, ff, pp, pf))
                if best is None or candidate < best:
                    best = candidate
    if best is None:
        return None
    return dict(config, **dict(zip(FIELDS, best[-1], strict=True)))


def matched_fast_bands(rows, config):
    normal = [row for row in rows if row['label'] == 'OK']
    global_scores = [float(row['global_score']) for row in normal]
    center = sorted(float(row['center_score']) for row in normal)
    n_pass = sum(score <= config['fast_pass_max'] for score in global_scores)
    n_fail = sum(score >= config['fast_fail_min'] for score in global_scores)
    passed = min(set(center), key=lambda cut: (abs(bisect_right(center, cut) - n_pass), cut))
    failed = min({score for score in center if score > passed},
                 key=lambda cut: (abs(len(center) - bisect_left(center, cut) - n_fail), cut))
    return dict(config, fast_pass_max=passed, fast_fail_min=failed)


def run(runner, feature, manifest, config, output):
    subprocess.run([str(runner), feature, str(manifest), str(config), str(output)], check=True)
    expected = [(row['path'], row['label']) for row in read_json(manifest)['rows']]
    rows = read_rows(output)
    if [(row['path'], row['label']) for row in rows] != expected:
        raise ValueError('Runner CSV does not match manifest order and labels')
    return rows


def classify(score, pass_max, fail_min):
    return 'PASS' if score <= pass_max else 'FAIL' if score >= fail_min else 'REVIEW'


def verify_live(rows, config):
    fp, ff, pp, pf = (config[field] for field in FIELDS)
    budget_ns = config['budget_us'] * 1000
    for row in rows:
        called = bool(row['precise_ns'])
        fast_ns = int(row['fast_ns'])
        if not row['fast_score']:
            expected, reason, should_call = 'ERROR', 'fast_analysis_error', False
        elif fast_ns > budget_ns:
            expected, reason, should_call = 'REVIEW', 'budget_exhausted', False
        else:
            expected = classify(float(row['fast_score']), fp, ff)
            should_call = expected == 'REVIEW' and fast_ns < budget_ns
            if expected != 'REVIEW':
                reason = 'fast_threshold'
            elif not should_call:
                reason = 'budget_exhausted'
            elif not row['precise_score']:
                expected, reason = 'ERROR', 'precise_analysis_error'
            elif fast_ns + int(row['precise_ns']) > budget_ns:
                reason = 'budget_exhausted'
            else:
                expected = classify(float(row['precise_score']), pp, pf)
                reason = 'precise_uncertain' if expected == 'REVIEW' else 'precise_threshold'
        if (expected, reason, should_call) != (row['decision'], row['reason'], called):
            raise ValueError(f'Unexpected C++ policy trace: {row}')


def stage(row):
    if row['reason'] == 'budget_exhausted':
        return 'budget_after_precise' if row['precise_ns'] else 'budget_before_precise'
    return 'precise' if row['precise_ns'] else 'fast'


def metrics(rows):
    times = sorted(int(row['fast_ns']) + int(row['precise_ns'] or 0) for row in rows)
    return {'n': len(rows), 'counts': {label: {decision: sum(r['label'] == label and r['decision'] == decision for r in rows)
                                             for decision in DECISIONS} for label in LABELS},
            'precise_calls': sum(bool(row['precise_ns']) for row in rows),
            'analysis_ns_p50_p95': [rank(times, q) for q in (50, 95)],
            'stages': dict(sorted(Counter(f'{r["label"]}/{stage(r)}/{r["decision"]}' for r in rows).items()))}


def compare(control, treatment):
    counts = Counter()
    examples = {}
    for old, new in zip(control, treatment, strict=True):
        if old['path'] != new['path']:
            raise ValueError('Invalid comparison order')
        key = f'{old["label"]}/{old["decision"]}/{new["decision"]}'
        counts[key] += 1
        if old['decision'] != new['decision'] and key not in examples:
            examples[key] = (old, new)
    return {'transitions': dict(sorted(counts.items())), 'examples': examples}


def validate(args):
    args.output.mkdir(parents=True, exist_ok=False)
    inputs = args.inputs
    manifest = inputs / 'validation.json'
    rows = run(args.runner, 'observe', manifest, ORIGINAL, args.output/'observations.csv')
    original = read_json(ORIGINAL)
    observations = {feature: Observations(rows, feature, original['budget_us']*1000) for feature in ('global', 'center')}
    control = observations['global'].evaluate(original)
    _, ok_fail, ok_pass, automatic, _ = control
    n_ok = observations['global'].ok.bit_count()
    limits = (min(math.ceil(len(rows)*.90), automatic), math.floor(len(rows)*.25),
              ok_pass - n_ok//100, ok_fail + n_ok//100)
    profiles = [('original', 'global', original), ('center_matched', 'center', matched_fast_bands(rows, original))]
    fitted = {feature: fit(obs, original, limits) for feature, obs in observations.items()}
    profiles.extend((f'{feature}_fit', feature, policy) for feature, policy in fitted.items() if policy is not None)

    results, frozen = {}, {}
    for name, feature, config in profiles:
        config_path = args.output/f'{name}.json'
        write_json(config_path, config)
        live_rows = run(args.runner, feature, manifest, config_path, args.output/f'validation-{name}.csv')
        verify_live(live_rows, config)
        results[name] = {'config': config, 'observed_calibration_counts': observations[feature].evaluate(config),
                         'live': metrics(live_rows)}
        frozen[name] = {'feature': feature, 'sha256': sha(config_path)}
    report = {'calibration_count_order': ['ko_pass', 'ok_fail', 'ok_pass', 'automatic', 'precise_calls'],
              'limits': dict(zip(('min_automatic','max_precise','min_ok_pass','max_ok_fail'), limits, strict=True)),
              'infeasible_features': [feature for feature, result in fitted.items() if result is None],
              'results': results}
    write_json(args.output/'validation.json', report)
    freeze = {'schema': 1, 'environment': platform.platform(), 'runner': sha(args.runner),
              'original': sha(ORIGINAL), 'profiles': frozen, 'validation_report': sha(args.output/'validation.json'),
              'observations': sha(args.output/'observations.csv'),
              'manifests': {name: sha(inputs/name) for name in ('validation.json', 'test.json', 'test-primary.json')}}
    write_json(args.output/'freeze.json', freeze)
    print(f'Frozen {len(profiles)} profiles before test execution', flush=True)


def checked_freeze(args):
    freeze_path = args.output/'freeze.json'
    freeze = read_json(freeze_path)
    if (freeze['schema'] != 1 or freeze['environment'] != platform.platform() or freeze['runner'] != sha(args.runner)
            or freeze['original'] != sha(ORIGINAL) or freeze['validation_report'] != sha(args.output/'validation.json')
            or freeze['observations'] != sha(args.output/'observations.csv')):
        raise ValueError('Validation freeze mismatch')
    for name, expected in freeze['manifests'].items():
        if sha(args.inputs/name) != expected:
            raise ValueError(f'Frozen manifest changed: {name}')
    profiles = freeze['profiles']
    for name, profile in profiles.items():
        if sha(args.output/f'{name}.json') != profile['sha256']:
            raise ValueError(f'Frozen profile changed: {name}')
    return freeze


def test(args):
    freeze = checked_freeze(args)
    profiles = freeze['profiles']
    primary = {row['path'] for row in read_json(args.inputs/'test-primary.json')['rows']}
    manifest = args.inputs/'test.json'
    official = {row['path'] for row in read_json(manifest)['rows']}
    if not primary <= official:
        raise ValueError('Primary test is not a subset of the official test manifest')
    report = {'freeze_sha256': sha(args.output/'freeze.json'), 'runs': []}
    for number, order in enumerate((list(profiles), list(reversed(profiles))), 1):
        run_folder = args.output/f'test-{number}'
        run_folder.mkdir()
        observed = {}
        for name in order:
            config_path = args.output/f'{name}.json'
            rows = run(args.runner, profiles[name]['feature'], manifest, config_path, run_folder/f'{name}.csv')
            verify_live(rows, read_json(config_path))
            observed[name] = rows
        datasets = {'official': lambda row: True, 'primary': lambda row: row['path'] in primary,
                    'primary_width_385_415': lambda row: row['path'] in primary and 385 <= int(row['width']) <= 415}
        aggregates = {}
        for dataset, included in datasets.items():
            subset = {name: [row for row in rows if included(row)] for name, rows in observed.items()}
            aggregates[dataset] = {'profiles': {name: metrics(rows) for name, rows in subset.items()},
                                   'vs_original': {name: compare(subset['original'], rows)
                                                   for name, rows in subset.items() if name != 'original'}}
        report['runs'].append({'order': order, 'datasets': aggregates})
    write_json(args.output/'test.json', report)


def diagnose(args):
    """Calculate the same frozen rules with the time checks removed, from measured scores."""
    freeze = checked_freeze(args)
    test_report = read_json(args.output/'test.json')
    if test_report['freeze_sha256'] != sha(args.output/'freeze.json') or len(test_report['runs']) != 2:
        raise ValueError('Finish both measured test runs before diagnosis')
    primary = {row['path'] for row in read_json(args.inputs/'test-primary.json')['rows']}
    measured = run(args.runner, 'observe', args.inputs/'test.json', ORIGINAL, args.output/'test-observations.csv')
    cohorts = {'official': measured, 'primary': [r for r in measured if r['path'] in primary],
               'primary_width_385_415': [r for r in measured if r['path'] in primary and 385 <= int(r['width']) <= 415]}
    report = {}
    for cohort, images in cohorts.items():
        report[cohort] = {}
        for name, profile in freeze['profiles'].items():
            config = read_json(args.output/f'{name}.json')
            fp, ff, pp, pf = (config[field] for field in FIELDS)
            rows = []
            for image in images:
                fast = image[profile['feature']+'_score']
                decision = classify(float(fast), fp, ff) if fast else 'ERROR'
                precise = decision == 'REVIEW'
                if precise:
                    score = image['precise_score']
                    decision = classify(float(score), pp, pf) if score else 'ERROR'
                rows.append(dict(image, decision=decision, reason='score_only', fast_ns='0',
                                 precise_ns='0' if precise else ''))
            summary = metrics(rows)
            del summary['analysis_ns_p50_p95']
            report[cohort][name] = summary
    write_json(args.output/'test-without-budget.json', {'freeze_sha256': test_report['freeze_sha256'], 'datasets': report})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase', choices=('validate', 'test', 'diagnose'))
    parser.add_argument('--runner', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--inputs', type=Path, default=ROOT/'artifacts/S11/evaluation/inputs')
    args = parser.parse_args()
    {'validate': validate, 'test': test, 'diagnose': diagnose}[args.phase](args)


if __name__ == '__main__':
    main()
