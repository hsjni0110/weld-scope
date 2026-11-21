"""Compare fixed pooled tree scores with two train-calibrated operating regions."""
import argparse
import shutil
from collections import Counter
from pathlib import Path

from tools import evaluate_openzdm_boost as pooled
from tools.evaluate_openzdm_patterns import calibrate, counts, paired
from tools.evaluate_openzdm_policy import classify, metrics, read_json, read_rows, run, sha, verify_live, write_json

PRIOR = pooled.STUDY/'boost-study'
RUNNER = pooled.STUDY/'boost-guard-build/Release/openzdm_boost_guard.exe'
EDGE = pooled.STUDY/'validity-final/edge.json'
SIZES = pooled.SIZES
CONTROLS = ('S12','spatial','pooled')


def inside(row):
    return 385 <= int(row['width']) <= 415


def regional(config, is_inside):
    if is_inside:
        return config
    return dict(config,fast_pass_max=config['outside_pass'],fast_fail_min=config['outside_fail'])


def fit(out):
    pooled.verify_research_freeze(read_json(PRIOR/'freeze.json'))
    source = {r['path']:r for r in read_json(PRIOR/'calibration.json')['rows']}
    precise = {r['source_path']:r for r in read_rows(pooled.ROOT/'artifacts/S07/review-train-01/precise/results.csv')}
    def add_precise(rows):
        for r in rows:
            p = precise[r['path']]
            if p['image_sha256'] != source[r['path']]['sha256'] or r['label'] != p['label']:
                raise ValueError('Unpaired calibration source')
            r['precise_score'] = p['score']
    report = {}
    for size in SIZES:
        model = out/f'model-{size}.txt'
        shutil.copyfile(PRIOR/model.name,model)
        scores = read_rows(PRIOR/f'cal-{size}.csv')
        add_precise(scores)
        config = read_json(PRIOR/f'config-{size}.json')
        final = dict(config)
        report[size] = {}
        for area, file in [('inside','config'),('outside','outside')]:
            local = read_rows(pooled.STUDY/f'width-budget/cal-{area}.csv')
            add_precise(local)
            selected = [r for r in scores if inside(r) == (area == 'inside')]
            if {r['path'] for r in selected} != {r['path'] for r in local}:
                raise ValueError('Calibration width membership mismatch')
            reference = counts(local,read_json(pooled.PRIOR/(file+'.json')),'fast_score')
            candidate = calibrate(selected,'fast_score',config,reference,7)
            keys = ('fast_pass_max','fast_fail_min') if area=='inside' else ('outside_pass','outside_fail')
            final[keys[0]],final[keys[1]] = candidate['fast_pass_max'],candidate['fast_fail_min']
            report[size][area] = dict(reference=reference,candidate=counts(selected,candidate,'fast_score'))
        write_json(out/f'config-{size}.json',final)
    write_json(out/'calibration-report.json',report)


def measure(out, phase, source, mode):
    if mode in ('S12','spatial'):
        return pooled.measure(out,phase,source,mode)
    if mode == 'pooled':
        cfg = PRIOR/'config-400.json'
        rows = run(pooled.RUNNER,'measure',source,cfg,out/f'{phase}-{mode}.csv')
        verify_live(rows,read_json(cfg))
        return rows
    cfg = out/f'config-{mode}.json'
    rows = run(RUNNER,'measure',source,cfg,out/f'{phase}-{mode}.csv')
    for area in (True,False):
        verify_live([r for r in rows if inside(r)==area],regional(read_json(cfg),area))
    return rows


def summary(data):
    return {'metrics':{key:metrics(rows) for key,rows in data.items()},
            'paired':{f'{k}-to-{size}':paired(data[k],rows) for k in CONTROLS for size,rows in data.items()
                      if size not in CONTROLS}}


def by_group(data):
    return {name:summary({k:[r for r in rows if predicate(r)] for k,rows in data.items()})
            for name,predicate in [('all',lambda r:True),('inside',inside),('outside',lambda r:not inside(r))]}


def validate(out):
    order = (*CONTROLS,*SIZES)
    reports = []
    for i,modes in enumerate((order,tuple(reversed(order))),1):
        data = {k:measure(out,f'validation-{i}',pooled.VALIDATION,k) for k in modes}
        for size in SIZES:
            old = read_rows(PRIOR/f'validation-1-{size}.csv')
            if [(r['path'],r['fast_score']) for r in old] != [(r['path'],r['fast_score']) for r in data[size]]:
                raise ValueError('Packed tree predictions differ from frozen pooled scorer')
        reports.append(by_group(data))
    choices = []
    for size in SIZES:
        ratios = []
        for report in reports:
            total = report['all']['metrics'][size]
            c = total['counts']
            if c['KO']['ERROR']+c['OK']['ERROR'] or (c['KO']['REVIEW']+c['OK']['REVIEW'])*10 > total['n'] or (
                    total['precise_calls']*4 > total['n']):
                break
            for group in ('inside','outside'):
                now = report[group]['metrics'][size]['counts']
                prev = report[group]['metrics']['spatial']['counts']
                ratios.extend((now['KO']['PASS']/prev['KO']['PASS'],now['OK']['FAIL']/prev['OK']['FAIL']))
        if len(ratios)==8 and max(ratios)<=1:
            choices.append((max(ratios),sum(ratios),size))
    selected = min(choices)[2] if choices else None
    files = [RUNNER,pooled.RUNNER,pooled.CONTROL,pooled.SPATIAL,pooled.ORIGINAL,pooled.VALIDATION,pooled.FIT,
             pooled.INPUTS/'test.json',pooled.INPUTS/'test-primary.json',PRIOR/'freeze.json',PRIOR/'config-400.json',
             PRIOR/'model-400.txt',PRIOR/'calibration.json',out/'calibration-report.json',EDGE,Path(__file__).resolve(),
             *(p for p in pooled.PRIOR.rglob('*') if p.is_file()),*(out/f'config-{s}.json' for s in SIZES),
             *(out/f'model-{s}.txt' for s in SIZES),*(pooled.ROOT/'tools/openzdm_boost_guard'/f for f in
               ('CMakeLists.txt','runner.cpp')),
             *(pooled.ROOT/'src/WeldScope.FileInspection'/f for f in ('CompactBoostedTrees.h','PooledTexture.h'))]
    write_json(out/'freeze.json',{'selected':selected,'validation':reports,'source_layout':'shared',
                                  'hashes':{str(f.resolve()):sha(f) for f in files},
                                  'score_parity_rows_per_size':len(read_json(pooled.VALIDATION)['rows'])})
    print('Selected',selected,flush=True)


def boundary(out, size):
    config = read_json(out/f'config-{size}.json')
    originals = [r for r in read_rows(out/f'validation-1-{size}.csv') if int(r['width']) in (384,385,415,416)]
    edited = measure(out,'edge',EDGE,size)
    def action(row, area):
        settings = regional(config,area)
        return classify(float(row['fast_score']),settings['fast_pass_max'],settings['fast_fail_min'])
    pairs = []
    for i,(old,new) in enumerate(zip(originals,edited,strict=True)):
        if new['path'] != f'edge-{i:03d}.png' or new['label'] != old['label'] or inside(new)==inside(old):
            raise ValueError('Edge order/membership mismatch')
        pairs.append({'source':old['path'],'label':old['label'],'original':action(old,inside(old)),
                      'edited_fixed':action(new,inside(old)),'edited_routed':action(new,inside(new))})
    write_json(out/'boundary.json',{'cases':pairs,'original_to_routed_changes':sum(p['original']!=p['edited_routed'] for p in pairs),
                                    'fixed_to_routed_changes':sum(p['edited_fixed']!=p['edited_routed'] for p in pairs)})


def test(out):
    frozen = read_json(out/'freeze.json')
    size = frozen['selected']
    if not size:
        raise ValueError('No candidate passed validation')
    results = []
    primary = {r['path'] for r in read_json(pooled.INPUTS/'test-primary.json')['rows']}
    order = (*CONTROLS,size)
    for i,modes in enumerate((order,tuple(reversed(order))),1):
        pooled.verify_research_freeze(frozen)
        data = {k:measure(out,f'test-{i}',pooled.INPUTS/'test.json',k) for k in modes}
        results.append({'official':summary(data),'primary':by_group({k:[r for r in rows if r['path'] in primary]
                                                                     for k,rows in data.items()})})
    boundary(out,size)
    write_json(out/'results.json',{'freeze_sha256':sha(out/'freeze.json'),'test':results,
                                   'trace_hashes':{f.name:sha(f) for f in out.glob('*.csv')}})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase',choices=['fit','validate','test'])
    parser.add_argument('--output',required=True,type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    {'fit':fit,'validate':validate,'test':test}[args.phase](args.output.resolve())
