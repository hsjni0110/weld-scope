"""Summarize actual C++ output, keeping development counts and timing sizes explicit."""
import argparse
from collections import defaultdict
import csv
import hashlib
import json
import math
from pathlib import Path
import platform
from tempfile import TemporaryDirectory

from PIL import Image, ImageDraw, ImageFont


def percentiles(values):
    values = sorted(values)
    def value_at(fraction):
        position = (len(values) - 1) * fraction
        low, high = math.floor(position), math.ceil(position)
        return values[low] + (values[high] - values[low]) * (position - low)
    return dict(p50=value_at(0.5), p95=value_at(0.95))


def aggregate(rows):
    counts = dict(PASS=0, FAIL=0, ERROR=0)
    by_label = defaultdict(lambda: dict(PASS=0, FAIL=0, ERROR=0))
    timings = defaultdict(lambda: dict(load_ms=[], analysis_ms=[]))
    for row in rows:
        decision = row['decision']
        counts[decision] += 1
        by_label[row['label'] or 'unlabelled'][decision] += 1
        if decision != 'ERROR':
            size = f"{row['width']}x{row['height']}"
            for field in ('load_ms', 'analysis_ms'):
                timings[size][field].append(float(row[field]))
    return dict(attempted=len(rows), decisions=counts, by_label=dict(by_label),
                independent_evaluation=False, scope='train development, not held-out accuracy',
                by_size={size: dict(count=len(times['load_ms']), **{field: percentiles(values) for field, values in times.items()})
                         for size, times in sorted(timings.items())})


def has_openzdm_provenance(rows):
    selected = [row for row in rows if row.get('artifact_path') and row.get('decision') != 'ERROR']
    if not selected:
        return False
    evidence = Path(__file__).resolve().parents[1] / 'docs/steps/S02-evidence/manifest.csv'
    with evidence.open(encoding='utf-8-sig', newline='') as stream:
        known = {row['path']: (row['file_sha256'], row['label']) for row in csv.DictReader(stream) if row['split'] == 'train'}
    return all(known.get(row['source_path']) == (row['image_sha256'], row['label']) for row in selected)


def draw_examples(run, rows, destination, openzdm):
    # Static evidence: actual output pixels, equal columns, native aspect ratio, explicit text labels.
    selected = []
    seen = set()
    for row in rows:
        category = (row['label'], row['decision'])
        if row['artifact_path'] and category not in seen:
            selected.append(row)
            seen.add(category)
    if not selected:
        return []
    font = ImageFont.truetype('C:/Windows/Fonts/malgun.ttf', 18)
    heading = ImageFont.truetype('C:/Windows/Fonts/malgunbd.ttf', 24)
    canvas = Image.new('RGB', (1280, 100 + 210 * len(selected) + 110), 'white')
    draw = ImageDraw.Draw(canvas)
    draw.text((24, 16), 'S04 개발 결과', font=heading, fill='#172334')
    for column, title in enumerate(('원본', '흑백', '후보 마스크', '오버레이')):
        draw.text((24 + column * 314, 62), title, font=font, fill='#172334')
    for index, row in enumerate(selected):
        top = 100 + index * 210
        draw.text((24, top), row['source_path'], font=font, fill='#172334')
        for column, name in enumerate(('original.png', 'gray.png', 'mask.png', 'overlay.png')):
            with Image.open(run / row['artifact_path'] / name) as original:
                image = original.convert('RGB')
            scale = min(294 / image.width, 110 / image.height)
            display_size = (max(1, round(image.width * scale)), max(1, round(image.height * scale)))
            image = image.resize(display_size, Image.Resampling.NEAREST if name == 'mask.png' else Image.Resampling.BILINEAR)
            x = 24 + column * 314
            draw.rectangle((x, top + 36, x + 294, top + 146), fill='#131c29')
            canvas.paste(image, (x + (294 - image.width) // 2, top + 36 + (110 - image.height) // 2))
        correct = (row['label'] == 'KO' and row['decision'] == 'FAIL') or (row['label'] == 'OK' and row['decision'] == 'PASS')
        comparison = ('라벨 일치' if correct else '라벨 불일치') if row['label'] else '정답 라벨 없음'
        caption = f"원본 라벨 {row['label'] or '미지정'}   판정 {row['decision']}   후보 비율 {float(row['score']):.4f}   {comparison}"
        draw.text((24, top + 158), caption, font=font, fill='#172334')
    bottom = 100 + 210 * len(selected)
    attribution = 'OPENZDM | DOI 10.5281/zenodo.14025630 | CC BY 4.0' if openzdm else '사용자 입력 | 데이터 출처 미지정'
    title = '2D image dataset for the presence or absence of aesthetical defects on EV battery welds.' if openzdm else '데이터셋 라이선스 표시 없음'
    draw.text((24, bottom), attribution, font=font, fill='#172334')
    draw.text((24, bottom + 28), title, font=font, fill='#172334')
    draw.text((24, bottom + 56), '원본 확대 배치, 흑백 변환, 임계값 마스크와 오버레이. 결함 위치 정답과 독립 성능 평가 없음.', font=font, fill='#172334')
    canvas.save(destination)
    return [dict(source_path=row['source_path'], decision=row['decision'], label=row['label'], score=float(row['score'])) for row in selected]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run', type=Path, required=True)
    args = parser.parse_args()
    destination = args.run / 'report'
    if destination.exists():
        raise FileExistsError(f'Completed report already exists: {destination}')
    with (args.run / 'results.csv').open(encoding='utf-8', newline='') as stream:
        rows = list(csv.DictReader(stream))
    summary = json.loads((args.run / 'summary.json').read_text(encoding='utf-8'))
    report = aggregate(rows)
    if report['attempted'] != summary['requested'] or report['decisions'] != {key: summary[key] for key in ('PASS', 'FAIL', 'ERROR')}:
        raise ValueError('Result count and C++ summary mismatch')
    report['config_sha256'] = summary['config_sha256']
    report['scope'] = summary['scope']
    report['opencv_version'] = summary['opencv_version']
    report['threads'] = summary['threads']
    report['source_csv_sha256'] = hashlib.sha256((args.run / 'results.csv').read_bytes()).hexdigest()
    report['environment'] = dict(windows=platform.win32_ver(), machine=platform.machine(), timing_repetitions_per_input=1)
    report['openzdm_attribution_verified'] = has_openzdm_provenance(rows)
    with TemporaryDirectory(prefix='.report-preparing-', dir=args.run) as temporary:
        staging = Path(temporary)
        report['examples'] = draw_examples(args.run, rows, staging / 'examples.png', report['openzdm_attribution_verified'])
        (staging / 'development-summary.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        staging.rename(destination)
    print(json.dumps({key: report[key] for key in ('attempted', 'decisions', 'by_label', 'independent_evaluation')}, ensure_ascii=False, indent=2))
    print(f'Report: {destination}')


if __name__ == '__main__': main()
