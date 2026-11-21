"""Compare the same physical PNGs recorded by two real MFC folder inspections."""

import argparse
from collections import Counter, defaultdict
from math import ceil
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from tools.evaluate_openzdm_patterns import paired
from tools.evaluate_openzdm_policy import read_json, read_rows, sha, write_json


def records(directory):
    report = read_json(directory / 'cohort.json')
    file = directory / 'cohort.csv'
    if sha(file) != report['file_sha256']:
        raise ValueError('MFC trace hash mismatch')
    rows = read_rows(file)
    data = {r['path']: r for r in rows}
    if len(rows) != len(data) or len(data) != report['n']:
        raise ValueError('Missing or duplicate MFC records')
    return report, data


def summarize(items):
    counts = Counter(r['label'] + '/' + r['decision'] for r in items)
    times = sorted(1000 * (float(r['fast_ms']) + float(r['precise_ms'] or 0)) for r in items)
    return {'n': len(items), 'counts': dict(counts),
            'analysis_us_p50_p95': [times[ceil(f * len(times)) - 1] for f in (.5, .95)]}


def render(examples, root, output):
    canvas = Image.new('RGB', (940, 126 * len(examples)), 'white')
    draw = ImageDraw.Draw(canvas)
    font = ImageFont.load_default(size=18)
    for index, case in enumerate(examples):
        source = (root / case['path']).resolve(strict=True)
        if not source.is_relative_to(root) or sha(source) != case['sha256']:
            raise ValueError('Case source mismatch')
        y = 126 * index
        draw.text((12, y + 3), f"{case['path']}    {case['baseline']}  ->  {case['candidate']}",
                  font=font, fill='black')
        with Image.open(source) as image:
            # Preserve aspect ratio. Never crop, stretch or adjust contrast.
            factor = min(2, 914 / image.width, 87 / image.height)
            size = tuple(round(n * factor) for n in image.size)
            canvas.paste(image.convert('RGB').resize(size, Image.Resampling.NEAREST), (12, y + 30))
    canvas.save(output)


def compare(baseline, candidate, manifest, output, repeat=None):
    old_info, old = records(baseline)
    new_info, new = records(candidate)
    source = read_json(manifest)
    paths = [r['path'] for r in source['rows']]
    if (len(paths) != len(set(paths)) or set(paths) != old.keys() or old.keys() != new.keys()
            or sha(manifest) != old_info['manifest_sha256'] or sha(manifest) != new_info['manifest_sha256']):
        raise ValueError('Cohorts differ from manifest')
    cases = defaultdict(list)
    by_batch = defaultdict(list)
    part_ids = set()
    width_groups = defaultdict(list)
    root = (manifest.parent / source['root']).resolve(strict=True)
    for src in source['rows']:
        path = src['path']
        a, b = old[path], new[path]
        if any(r['sha256'] != src['sha256'] or r['label'] != src['label'] for r in (a, b)):
            raise ValueError(f'Different source or label: {path}')
        file = (root / path).resolve(strict=True)
        if not file.is_relative_to(root) or sha(file) != src['sha256']:
            raise ValueError(f'Original image differs from the manifest: {path}')
        with Image.open(file) as image:
            width_groups['385_to_415' if 385 <= image.width <= 415 else 'outside'].append(path)
        batch = src.get('acquisition_batch_id')
        part = src.get('physical_part_id')
        if batch and part:
            by_batch[batch].append(b)
            part_ids.add(part)
        if {a['decision'], b['decision']} == {'PASS', 'FAIL'}:
            case = {'path': path, 'sha256': src['sha256'], 'label': src['label'],
                    'baseline': a['decision'], 'candidate': b['decision'],
                    'old_fast_score': a['fast_score'], 'new_fast_score': b['fast_score']}
            cases[src['label'], a['decision'], b['decision']].append(case)
    examples = [min(group, key=lambda r: r['sha256']) for _, group in sorted(cases.items())]
    group_coverage = sum(map(len, by_batch.values()))
    report = {'n': len(paths), 'manifest_sha256': sha(manifest), 'baseline': old_info, 'candidate': new_info,
              'paired': paired([old[k] for k in paths], [new[k] for k in paths]),
              'by_width': {name: {'baseline': summarize([old[k] for k in group]),
                                  'candidate': summarize([new[k] for k in group])}
                           for name, group in {'all': paths, **width_groups}.items()},
              'examples': examples, 'examples_selection': 'lowest file SHA256 in each direct PASS/FAIL transition',
              'with_part_and_batch_ids': group_coverage, 'identified_parts': len(part_ids),
              'identified_batch_descriptive_counts': {k: summarize(v) for k, v in by_batch.items()}}
    if repeat is not None:
        repeat_info, again = records(repeat)
        for name in ('manifest_sha256', 'executable_sha256', 'config_sha256', 'algorithm_version'):
            if repeat_info[name] != new_info[name]:
                raise ValueError(f'Repeat changed {name}')
        if again.keys() != new.keys() or repeat_info['batch_id'] == new_info['batch_id']:
            raise ValueError('Expected a separate run of the same cohort')
        for k in paths:
            if any(again[k][name] != new[k][name] for name in ('label', 'sha256', 'fast_score')):
                raise ValueError(f'Repeat changed image or first score: {k}')
        repeated = [again[k] for k in paths]
        report['repeat'] = {'cohort': repeat_info, 'summary': summarize(repeated),
                            'baseline_to_repeat': paired([old[k] for k in paths], repeated),
                            'candidate_to_repeat': paired([new[k] for k in paths], repeated)}
    output.mkdir(parents=True, exist_ok=False)
    if examples:
        render(examples, root, output / 'cases.png')
    write_json(output / 'comparison.json', report)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'candidate', 'manifest', 'output'):
        parser.add_argument(name, type=Path)
    parser.add_argument('--repeat', type=Path)
    args = parser.parse_args()
    compare(args.baseline, args.candidate, args.manifest, args.output, args.repeat)
