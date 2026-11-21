"""Fit, calibrate, freeze and compare a pooled brightness/texture classifier."""
import argparse
import hashlib
import io
import json
import os
import struct
import subprocess
import urllib.request
import zipfile
from collections import Counter
from pathlib import Path

from tools.evaluate_openzdm_patterns import INPUTS, calibrate, counts, manifest, paired
from tools.evaluate_openzdm_policy import ORIGINAL, ROOT, metrics, read_json, read_rows, run, sha, verify_live, write_json


STUDY = ROOT/'artifacts/OPENZDM-EDA'
PRIOR = ROOT/'docs/algorithms/evidence/openzdm-spatial-model'
VALIDATION = STUDY/'width-budget/validation.json'
FIT = STUDY/'validity-final/pooled-fit.json'
RUNNER = STUDY/'boost-build/Release/openzdm_boost.exe'
TRAINER = STUDY/'boost-build/Release/openzdm_boost_fit.exe'
SPATIAL = STUDY/'budget-build/Release/openzdm_shape.exe'
CONTROL = STUDY/'shape-build/Release/openzdm_eda_final.exe'
WHEEL_HASH = '37089ee95664b6550a7189d887dbf098e3eadab03537e411f52c63c121e3ba4b'
SIZES = (100, 220, 400)

# 이 커밋에서 작성된 동결 기록에는 공용 모델 코드의 이전 경로가 들어 있다.
PRE_SHARED_MODEL_REVISION = '7102863ec4ff9216e4549c81e31913625866ef9d'
MOVED_RESEARCH_SOURCES = {
    'tools/evaluate_openzdm_boost.py',
    'tools/evaluate_openzdm_boost_widths.py',
    'tools/evaluate_openzdm_smooth.py',
    'tools/openzdm_boost/PooledTexture.h',
    'tools/openzdm_boost/runner.cpp',
    'tools/openzdm_boost_guard/CompactBoostedTrees.h',
    'tools/openzdm_boost_guard/runner.cpp',
    'tools/openzdm_boost_smooth/InterpolatedScoreBand.h',
    'tools/openzdm_boost_smooth/runner.cpp',
}


def verify_research_freeze(frozen):
    """동결 입력을 검증한다. 구형 기록에서 이동한 소스만 당시 Git 바이트로 검증한다."""
    for name, expected in frozen['hashes'].items():
        path = Path(name)
        if path.is_file() and sha(path) == expected:
            continue
        relative = path.relative_to(ROOT).as_posix() if path.is_relative_to(ROOT) else ''
        if 'source_layout' not in frozen and relative in MOVED_RESEARCH_SOURCES:
            git = os.environ.get('WELDSCOPE_GIT', 'git')
            try:
                blob = subprocess.run(
                    [git, 'show', f'{PRE_SHARED_MODEL_REVISION}:{relative}'],
                    cwd=ROOT, capture_output=True, check=True,
                ).stdout
            except (FileNotFoundError, subprocess.CalledProcessError) as error:
                raise ValueError('Cannot read the historical research source. Check Git and WELDSCOPE_GIT.') from error
            # 동결 기록은 작업 트리의 줄바꿈 바이트까지 해시했다.
            windows_bytes = blob.replace(b'\r\n', b'\n').replace(b'\n', b'\r\n')
            if any(hashlib.sha256(data).hexdigest() == expected for data in (blob, windows_bytes)):
                continue
        raise ValueError(f'Frozen research input changed or is missing: {path}')


def fit(out):
    dll = out/'lib_lightgbm.dll'
    if not dll.exists():
        release = json.load(urllib.request.urlopen('https://pypi.org/pypi/lightgbm/4.6.0/json'))
        wheel = next(f for f in release['urls'] if f['filename'] == 'lightgbm-4.6.0-py3-none-win_amd64.whl')
        data = urllib.request.urlopen(wheel['url']).read()
        if hashlib.sha256(data).hexdigest() != WHEEL_HASH:
            raise ValueError('Training wheel checksum mismatch')
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            dll.write_bytes(archive.read('lightgbm/lib/lib_lightgbm.dll'))
    subprocess.run([RUNNER,'extract',FIT,ORIGINAL,out/'train.bin'],check=True)
    subprocess.run([TRAINER,dll,out/'train.bin',str(max(SIZES)),out/'trained.txt'],check=True)


def convert(native, output, limit):
    before, *sections = native.read_text().split('\nTree=')
    if 'objective=binary sigmoid:1' not in before or 'max_feature_idx=517' not in before:
        raise ValueError('Unsupported LightGBM model')
    if len(sections) < limit:
        raise ValueError('Incomplete model')
    with output.open('x',encoding='ascii',newline='\n') as file:
        print('pooled-texture-v1', 518, limit, file=file)
        for tree in sections[:limit]:
            fields = dict(line.split('=',1) for line in tree.split('\n\n',1)[0].splitlines()[1:] if '=' in line)
            if fields['num_cat'] != '0' or set(fields['decision_type'].split()) - {'2'}:
                raise ValueError('Unsupported categorical/missing value rule')
            print(fields['num_leaves'],file=file)
            for node in zip(*(fields[k].split() for k in ('split_feature','threshold','left_child','right_child')),strict=True):
                print(*node,file=file)
            print(fields['leaf_value'],file=file)


def calibration_manifest(out):
    rows = []
    for file in (STUDY/'pattern-converged/calibration.json', STUDY/'width-study/cal.json'):
        source = read_json(file)
        rows.extend(source['rows'])
    rows.sort(key=lambda r:r['path'])
    fit_paths = {r['path'] for r in read_json(FIT)['rows']}
    if len(fit_paths) + len(rows) != 43169 or fit_paths.intersection(r['path'] for r in rows):
        raise ValueError('Unexpected train partition')
    manifest(rows,Path(source['root']),out/'calibration.json')
    return {r['path']:r for r in rows}


def calibrate_models(out):
    members = calibration_manifest(out)
    precise = {r['source_path']:r for r in read_rows(ROOT/'artifacts/S07/review-train-01/precise/results.csv')}
    def add_precise(rows):
        for r in rows:
            p = precise[r['path']]
            if p['image_sha256'] != members[r['path']]['sha256'] or p['label'] != r['label']:
                raise ValueError('Precise calibration input mismatch')
            r['precise_score'] = p['score']
    reference = Counter()
    for area, config in [('inside','config'),('outside','outside')]:
        rows = read_rows(STUDY/f'width-budget/cal-{area}.csv')
        add_precise(rows)
        reference.update(counts(rows,read_json(PRIOR/(config+'.json')),'fast_score'))
    if sum(v for k,v in reference.items() if '/' in k) != len(members):
        raise ValueError('Calibration reference coverage mismatch')
    reports = {}
    for size in SIZES:
        model = out/f'model-{size}.txt'
        convert(out/'trained.txt',model,size)
        config = out/f'config-{size}.json'
        settings = dict(read_json(ORIGINAL),model=model.name)
        write_json(config,settings)
        rows = run(RUNNER,'measure',out/'calibration.json',config,out/f'cal-{size}.csv')
        add_precise(rows)
        settings = calibrate(rows,'fast_score',settings,reference,7)
        for key in ('outside_pass','outside_fail'):
            settings.pop(key)
        write_json(config,settings)
        reports[size] = dict(counts(rows,settings,'fast_score'))
    write_json(out/'calibration-report.json',{'prior':reference,'candidates':reports})
    # The full numerical model must reproduce native probabilities on actual fit images.
    fixture = read_json(FIT)
    indices = range(0,len(fixture['rows']),max(1,len(fixture['rows'])//256))
    manifest([fixture['rows'][i] for i in indices],Path(fixture['root']),out/'parity-input.json')
    rows = run(RUNNER,'measure',out/'parity-input.json',out/f'config-{max(SIZES)}.json',out/'parity.csv')
    predictions = (out/'trained.txt.probabilities').read_bytes()
    errors = [abs(float(r['fast_score'])-struct.unpack_from('<d',predictions,8*i)[0]) for i,r in zip(indices,rows,strict=True)]
    if max(errors) > 1e-10:
        raise ValueError('Native/exported model probability mismatch')
    write_json(out/'parity.json',{'rows':len(rows),'max_absolute_probability_error':max(errors)})


def measure(out, phase, source, mode):
    if mode == 'S12':
        runner, command, config = CONTROL,'global',ORIGINAL
    elif mode == 'spatial':
        runner, command, config = SPATIAL,'whole',PRIOR/'config.json'
    else:
        runner, command, config = RUNNER,'measure',out/f'config-{mode}.json'
    rows = run(runner,command,source,config,out/f'{phase}-{mode}.csv')
    if mode == 'spatial':
        for inside, file in [(True,config),(False,PRIOR/'outside.json')]:
            verify_live([r for r in rows if (385 <= int(r['width']) <= 415) == inside],read_json(file))
    else:
        verify_live(rows,read_json(config))
    return rows


def summary(data):
    return {'metrics':{k:metrics(v) for k,v in data.items()},
            'paired':{f'{prior}-to-{k}':paired(data[prior],rows) for prior in ('S12','spatial')
                      for k,rows in data.items() if k not in ('S12','spatial')}}


def validate(out):
    order = ('S12','spatial',*SIZES)
    reports = []
    for trial, modes in enumerate((order,tuple(reversed(order))),1):
        data = {name:measure(out,f'validation-{trial}',VALIDATION,name) for name in modes}
        reports.append(summary(data))
    candidates = []
    for size in SIZES:
        ratios = []
        for report in reports:
            previous = report['metrics']['spatial']['counts']
            m = report['metrics'][size]
            c = m['counts']
            if (c['KO']['ERROR']+c['OK']['ERROR'] > 0 or c['KO']['REVIEW']+c['OK']['REVIEW'] > m['n']*.10
                    or m['precise_calls'] > m['n']*.25):
                break
            ratios.extend((c['KO']['PASS']/previous['KO']['PASS'],c['OK']['FAIL']/previous['OK']['FAIL']))
        if len(ratios) == 4 and max(ratios) < 1:
            candidates.append((max(ratios),sum(ratios),size))
    selected = min(candidates)[2] if candidates else None
    files = [RUNNER,TRAINER,CONTROL,SPATIAL,ORIGINAL,FIT,VALIDATION,out/'calibration.json',out/'lib_lightgbm.dll',
             out/'trained.txt',out/'calibration-report.json',out/'parity.json',INPUTS/'test.json',INPUTS/'test-primary.json',
             *(ROOT/'tools/openzdm_boost'/f for f in ('CMakeLists.txt','fit.cpp','runner.cpp','BoostedTrees.h')),
             ROOT/'src/WeldScope.FileInspection/PooledTexture.h',
             Path(__file__).resolve(),*(p for p in PRIOR.rglob('*') if p.is_file()),
             *(out/f'config-{i}.json' for i in SIZES),*(out/f'model-{i}.txt' for i in SIZES)]
    write_json(out/'freeze.json',{'selected':selected,'source_layout':'shared','hashes':{str(f.resolve()):sha(f) for f in files},
                                  'validation':reports})
    print('Validation selected',selected,flush=True)


def test(out):
    freeze = read_json(out/'freeze.json')
    selected = freeze['selected']
    if not selected:
        raise ValueError('Validation gate failed')
    primary = {r['path'] for r in read_json(INPUTS/'test-primary.json')['rows']}
    trials = []
    for trial, order in enumerate([('S12','spatial',selected),(selected,'spatial','S12')],1):
        verify_research_freeze(freeze)
        data = {mode:measure(out,f'test-{trial}',INPUTS/'test.json',mode) for mode in order}
        groups = {}
        for name, predicate in [('official',lambda r:True),('primary',lambda r:r['path'] in primary),
                                ('primary-inside',lambda r:r['path'] in primary and 385<=int(r['width'])<=415),
                                ('primary-outside',lambda r:r['path'] in primary and not 385<=int(r['width'])<=415)]:
            groups[name] = summary({k:[r for r in rows if predicate(r)] for k,rows in data.items()})
        trials.append(groups)
    write_json(out/'results.json',{'freeze_sha256':sha(out/'freeze.json'),'test':trials,
                                  'trace_hashes':{p.name:sha(p) for p in out.glob('*.csv')}})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase',choices=['fit','calibrate','validate','test'])
    parser.add_argument('--output',required=True,type=Path)
    args = parser.parse_args()
    args.output.mkdir(exist_ok=True,parents=True)
    {'fit':fit,'calibrate':calibrate_models,'validate':validate,'test':test}[args.phase](args.output.resolve())
