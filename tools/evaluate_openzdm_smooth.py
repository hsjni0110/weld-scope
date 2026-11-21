"""Evaluate one fixed six-pixel interpolation of the frozen 220-tree operating bands."""
import argparse
import shutil
from pathlib import Path

from tools import evaluate_openzdm_boost_widths as widths
from tools.evaluate_openzdm_patterns import paired
from tools.evaluate_openzdm_policy import classify, metrics, read_json, read_rows, run, sha, verify_live, write_json

pooled = widths.pooled
HARD = pooled.STUDY/'boost-priority'
RUNNER = pooled.STUDY/'boost-smooth-build/Release/openzdm_smooth.exe'
MODES = ('S12','spatial','hard','smooth')


def interpolated(config,width):
    distance = min(width-384.5,415.5-width)
    if distance >= 6:
        return config
    if distance <= -6:
        return widths.regional(config,False)
    weight = (distance+6)/12
    return dict(config,fast_pass_max=config['outside_pass']+weight*(config['fast_pass_max']-config['outside_pass']),
                fast_fail_min=config['outside_fail']+weight*(config['fast_fail_min']-config['outside_fail']))


def measure(out,phase,source,mode):
    if mode in ('S12','spatial'):
        return pooled.measure(out,phase,source,mode)
    hard = mode=='hard'
    config = HARD/'config-220.json' if hard else out/'config.json'
    settings = read_json(config)
    rows = run(widths.RUNNER if hard else RUNNER,'measure',source,config,out/f'{phase}-{mode}.csv')
    for r in rows:
        band = widths.regional(settings,widths.inside(r)) if hard else interpolated(settings,int(r['width']))
        verify_live([r],band)
    return rows


def compare(data):
    def summary(rows):
        return {'metrics':{k:metrics(v) for k,v in rows.items()},
                'paired':{k:paired(rows[k],rows['smooth']) for k in MODES if k!='smooth'}}
    if [(r['path'],r['fast_score']) for r in data['hard']] != [(r['path'],r['fast_score']) for r in data['smooth']]:
        raise ValueError('Score changed with threshold interpolation')
    return {name:summary({k:[r for r in rows if f(r)] for k,rows in data.items()}) for name,f in
            [('all',lambda r:True),('inside',widths.inside),('outside',lambda r:not widths.inside(r))]}


def boundary(out, rows):
    edited = measure(out,'edge',widths.EDGE,'smooth')
    originals = [r for r in rows if int(r['width']) in (384,385,415,416)]
    previous = read_json(HARD/'boundary.json')
    config = read_json(out/'config.json')
    def action(row, width):
        settings = interpolated(config,width)
        return classify(float(row['fast_score']),settings['fast_pass_max'],settings['fast_fail_min'])
    cases = []
    for i,(orig,mod,old) in enumerate(zip(originals,edited,previous['cases'],strict=True)):
        ow,mw = int(orig['width']),int(mod['width'])
        if mod['path']!=f'edge-{i:03d}.png' or orig['path']!=old['source'] or abs(ow-mw)!=1:
            raise ValueError('Unexpected original/edit pair')
        cases.append({'source':orig['path'],'label':orig['label'],'width':ow,'edited_width':mw,
                      'original':action(orig,ow),'edited_fixed':action(mod,ow),'edited_routed':action(mod,mw)})
    def transitions(left):
        mismatches = [r for r in cases if r[left]!=r['edited_routed']]
        return {'changes':len(mismatches),'direct_pass_fail':sum({r[left],r['edited_routed']}=={'PASS','FAIL'} for r in mismatches)}
    result = {'cases':cases,'same_edited_score':transitions('edited_fixed'),'original_to_edited':transitions('original'),
              'hard_same_edited_score_changes':previous['fixed_to_routed_changes']}
    write_json(out/'boundary.json',result)
    return result


def validation(out):
    inherited = read_json(HARD/'freeze.json')
    if inherited['selected']!=220 or any(sha(Path(f))!=h for f,h in inherited['hashes'].items()):
        raise ValueError('Inherited 220 freeze changed')
    out.mkdir(parents=True,exist_ok=False)
    config = read_json(HARD/'config-220.json')
    shutil.copyfile(HARD/config['model'],out/config['model'])
    write_json(out/'config.json',dict(config,width_calibration='linear-6px-v1'))
    reports = []
    passed = True
    for i,order in enumerate((MODES,tuple(reversed(MODES))),1):
        data = {k:measure(out,f'validation-{i}',pooled.VALIDATION,k) for k in order}
        report = compare(data)
        reports.append(report)
        m = report['all']['metrics']['smooth']
        c = m['counts']
        passed &= (c['KO']['ERROR']+c['OK']['ERROR']==0 and (c['KO']['REVIEW']+c['OK']['REVIEW'])*10<=m['n']
                   and m['precise_calls']*4<=m['n'])
        for group in ('inside','outside'):
            stats = report[group]['metrics']
            passed &= all(stats['smooth']['counts'][l][d]<=stats['spatial']['counts'][l][d]
                          for l,d in [('KO','PASS'),('OK','FAIL')])
    edge = boundary(out,data['smooth'])
    passed &= (edge['same_edited_score']['changes']*2<=edge['hard_same_edited_score_changes'] and
               edge['same_edited_score']['direct_pass_fail']==0 and edge['original_to_edited']['direct_pass_fail']==0)
    sources = pooled.ROOT/'tools/openzdm_boost_smooth'
    files = [RUNNER,widths.RUNNER,pooled.SPATIAL,pooled.CONTROL,pooled.ORIGINAL,HARD/'model-220.txt',HARD/'config-220.json',
             HARD/'boundary.json',HARD/'freeze.json',out/config['model'],out/'config.json',out/'boundary.json',widths.EDGE,
             pooled.VALIDATION,pooled.INPUTS/'test.json',pooled.INPUTS/'test-primary.json',Path(__file__).resolve(),
             *(sources/k for k in ('runner.cpp','InterpolatedScoreBand.h','CMakeLists.txt')),
             *(p for p in pooled.PRIOR.rglob('*') if p.is_file())]
    write_json(out/'freeze.json',{'passed':passed,'validation':reports,'hashes':{str(f.resolve()):sha(f) for f in files}})
    print('Validation gate:',passed,flush=True)


def test(out):
    frozen = read_json(out/'freeze.json')
    if not frozen['passed']:
        raise ValueError('Interpolation failed validation')
    primary = {r['path'] for r in read_json(pooled.INPUTS/'test-primary.json')['rows']}
    reports = []
    for i,order in enumerate((MODES,tuple(reversed(MODES))),1):
        if any(sha(Path(f))!=h for f,h in frozen['hashes'].items()):
            raise ValueError('Interpolation freeze mismatch')
        data = {k:measure(out,f'test-{i}',pooled.INPUTS/'test.json',k) for k in order}
        reports.append({'official':compare(data),'primary':compare({k:[r for r in rows if r['path'] in primary]
                                                                    for k,rows in data.items()})})
    write_json(out/'results.json',{'freeze_sha256':sha(out/'freeze.json'),'test':reports,
                                   'trace_hashes':{f.name:sha(f) for f in out.glob('*.csv')}})


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase',choices=['validate','test'])
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    (validation if args.phase=='validate' else test)(args.output.resolve())
