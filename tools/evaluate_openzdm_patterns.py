"""Fit, freeze and evaluate the concentration/repetition ablation on OPENZDM."""

import argparse
import math
import shutil
import subprocess
from collections import Counter
from pathlib import Path

from tools.evaluate_openzdm_policy import (ROOT, ORIGINAL, Observations, classify, mask, metrics, rank, read_json,
                                          read_rows, run, sha, verify_live, write_json)

PROFILES = ('S12','A','B','C')
AUDIT = ROOT/'docs/algorithms/evidence/openzdm-blind-audit.json'
INPUTS = ROOT/'artifacts/S11/evaluation/inputs'


def manifest(rows, root, path):
    if not rows or len({r['path'] for r in rows}) != len(rows):
        raise ValueError('Expected nonempty unique image paths')
    write_json(path, {'root':str(root.resolve()),'rows':[
        {k:r[k] for k in ('path','split','label','sha256')} for r in rows]})


def prepare(args):
    out = args.output
    out.mkdir(parents=True,exist_ok=False)
    if args.reuse_fit:
        prior = read_json(args.reuse_fit/'freeze.json')
        if sha(args.seam)!=prior['seam'] or sha(AUDIT)!=prior['audit']:
            raise ValueError('Different training input')
        for name in ('fit.json','calibration.json','validation.json','models.json'):
            if sha(args.reuse_fit/name)!=prior['files'][name]:
                raise ValueError('Training freeze mismatch: '+name)
            shutil.copyfile(args.reuse_fit/name,out/name)
        write_json(out/'score-config.json',read_json(ORIGINAL)|read_json(out/'models.json'))
        return
    rows = read_rows(args.seam)
    reviewed = {r['path'] for r in read_json(AUDIT)['cases'].values()}
    train = [r for r in rows if r['split']=='train']
    val = [r for r in rows if r['split']=='validation']
    if len(train)+len(val)!=len(rows) or not reviewed <= {r['path'] for r in val}:
        raise ValueError('Unexpected split or blind-review cohort')
    fit = [r for r in train if int(r['sha256'][:2],16)>=48]
    calibration = [r for r in train if int(r['sha256'][:2],16)<48]
    manifest(fit,args.train_images,out/'fit.json')
    manifest(calibration,args.train_images,out/'calibration.json')
    manifest([r for r in val if r['path'] not in reviewed],INPUTS/'images',out/'validation.json')
    training_mode = 'pattern-train-stable' if args.converge else 'pattern-train'
    subprocess.run([str(args.fit_runner or args.runner),training_mode,str(out/'fit.json'),str(ORIGINAL),
                    str(out/'models.json')],check=True)
    write_json(out/'score-config.json',read_json(ORIGINAL)|read_json(out/'models.json'))


def counts(rows, cfg, field):
    totals = Counter()
    for r in rows:
        d = classify(float(r[field]),cfg['fast_pass_max'],cfg['fast_fail_min'])
        if d=='REVIEW':
            totals['calls'] += 1
            d = classify(float(r['precise_score']),cfg['precise_pass_max'],cfg['precise_fail_min'])
        totals[r['label']+'/'+d] += 1
    return totals


def calibrate(rows, name, original, reference, review_percent):
    observations = [dict(r, study_score=r[name],study_ns=0,precise_ns=0) for r in rows]
    obs = Observations(observations,'study',original['budget_us']*1000)
    passed = mask(float(r['precise_score'])<=original['precise_pass_max'] for r in rows)
    failed = mask(float(r['precise_score'])>=original['precise_fail_min'] for r in rows)
    scores = sorted(obs.fast)
    cuts = sorted({0.,1.,*(rank(scores,i) for i in range(1,100))})
    # Precompute each threshold mask once.
    boundaries = {c:obs.fast_masks(c,c)[:2] for c in cuts}
    all_rows = obs.ok | obs.ko
    best = None
    for i, lo in enumerate(cuts):
        for hi in cuts[i+1:]:
            low, high = boundaries[lo][0], boundaries[hi][1]
            mid = all_rows & ~(low|high)
            if mid.bit_count()>len(rows)*.25:
                continue
            ko,ok,_,auto,_ = obs.count((low,high,mid),passed,failed)
            review = len(rows)-auto
            if review*100>len(rows)*review_percent:
                continue
            ratios = ko/reference['KO/PASS'],ok/reference['OK/FAIL']
            value = (max(ratios),sum(ratios),review,lo,hi)
            if best is None or value<best:
                best = value
    if best is None:
        raise ValueError(f'No calibration thresholds for {name}')
    return dict(original,fast_pass_max=best[-2],fast_fail_min=best[-1],
                outside_pass=original['fast_pass_max'],outside_fail=original['fast_fail_min'])


def paired(control,treatment):
    result = {}
    if [(r['path'],r['label']) for r in control]!=[(r['path'],r['label']) for r in treatment]:
        raise ValueError('Unpaired images')
    transitions = Counter((a['label'],a['decision'],b['decision']) for a,b in zip(control,treatment,strict=True))
    for label,error in [('KO','PASS'),('OK','FAIL')]:
        n = sum(r['label']==label for r in control)
        added = sum(c for (l,old,new),c in transitions.items() if l==label and old!=error and new==error)
        removed = sum(c for (l,old,new),c in transitions.items() if l==label and old==error and new!=error)
        delta = (added-removed)/n
        se = math.sqrt(((added+removed)/n-delta*delta)/(n-1))
        result[label+'_'+error] = {'added':added,'removed':removed,'delta_pp':100*delta,
                                   'ci95_pp':[100*(delta-1.96*se),100*(delta+1.96*se)]}
    result['transitions'] = {'/'.join(k):v for k,v in sorted(transitions.items())}
    return result


def live(args, name, source, path):
    cfg_path = ORIGINAL if name=='S12' else args.output/(name+'.json')
    c = read_json(cfg_path)
    rows = run(args.runner,'global' if name=='S12' else 'pattern-'+name,source,cfg_path,path)
    target = [r for r in rows if 385<=int(r['width'])<=415]
    verify_live(target,c)
    verify_live([r for r in rows if not 385<=int(r['width'])<=415],read_json(ORIGINAL))
    return rows


def report(data):
    return {'metrics':{k:metrics(v) for k,v in data.items()},
            'paired':{a+'_'+b:paired(data[a],data[b]) for a,b in [('S12','A'),('S12','B'),('S12','C'),
                                                                   ('A','B'),('B','C')]}}


def validate(args):
    out = args.output
    src = {r['path']:r for r in read_rows(args.seam)}
    cfg = read_json(ORIGINAL)
    rows = run(args.runner,'pattern-scores',out/'calibration.json',out/'score-config.json',out/'scores-cal.csv')
    for r in rows:
        r.update({k:src[r['path']][k] for k in ('original_fast','precise_score')})
    ref = counts(rows,cfg,'original_fast')
    target = ref
    if args.prior_target:
        prior = read_json(ROOT/'docs/algorithms/evidence/openzdm-eda-regions.json')['probes']['both']['validation']['counts']
        target = {label+'/'+d:sum(r['label']==label for r in rows)*prior[label][d]/sum(prior[label].values())
                  for label,d in [('KO','PASS'),('OK','FAIL')]}
    models = read_json(out/'models.json')
    calibration = {'S12':dict(ref)}
    for name in PROFILES[1:]:
        c = calibrate(rows,name,cfg,target,args.review_percent)
        calibration[name] = dict(counts(rows,c,name))
        write_json(out/(name+'.json'),c|models)
    data = {name:live(args,name,out/'validation.json',out/('val-'+name+'.csv')) for name in PROFILES}
    result = report(data)
    stats = result['metrics']
    eligible = []
    for name in PROFILES[1:]:
        m = stats[name]
        n = m['n']
        ko,ok = m['counts']['KO'],m['counts']['OK']
        if args.prior_target:
            ratios = (ko['PASS']/sum(ko.values())/(prior['KO']['PASS']/sum(prior['KO'].values())),
                      ok['FAIL']/sum(ok.values())/(prior['OK']['FAIL']/sum(prior['OK'].values())))
        else:
            ratios = ko['PASS']/stats['S12']['counts']['KO']['PASS'],ok['FAIL']/stats['S12']['counts']['OK']['FAIL']
        if (max(ratios)<1 and ko['REVIEW']+ok['REVIEW']<=n*.10 and m['precise_calls']<=n*.25
                and ko['ERROR']+ok['ERROR']==0):
            eligible.append((max(ratios),name))
    result['representative'] = min(eligible)[1] if eligible else None
    write_json(out/'validation-report.json',result)
    freeze = {'runner':sha(args.runner),'original':sha(ORIGINAL),'seam':sha(args.seam),'audit':sha(AUDIT),
              'files':{f:sha(out/f) for f in ['fit.json','calibration.json','validation.json','models.json',
                       'validation-report.json',*(n+'.json' for n in PROFILES[1:])]},
              'test_manifests':{f:sha(INPUTS/f) for f in ('test.json','test-primary.json')},
              'calibration':calibration,'calibration_review_percent':args.review_percent,
              'target_reference':'published_regions' if args.prior_target else 'S12',
              'sizes':{k:len(read_json(out/(k+'.json'))['rows']) for k in ('fit','calibration','validation')}}
    write_json(out/'freeze.json',freeze)
    print('Validation representative:',result['representative'],flush=True)


def test(args):
    out = args.output
    freeze = read_json(out/'freeze.json')
    if (sha(args.runner)!=freeze['runner'] or sha(ORIGINAL)!=freeze['original'] or
        any(sha(out/f)!=h for f,h in freeze['files'].items()) or
        any(sha(INPUTS/f)!=h for f,h in freeze['test_manifests'].items())):
        raise ValueError('Study freeze mismatch')
    primary = {r['path'] for r in read_json(INPUTS/'test-primary.json')['rows']}
    results = []
    for number,order in enumerate((PROFILES,tuple(reversed(PROFILES))),1):
        data = {n:live(args,n,INPUTS/'test.json',out/f'test-{number}-{n}.csv') for n in order}
        result = {}
        for cohort in ('official','primary','target'):
            subset = {n:[r for r in rows if cohort=='official' or r['path'] in primary and
                         (cohort=='primary' or 385<=int(r['width'])<=415)] for n,rows in data.items()}
            result[cohort] = report(subset)
        results.append(result)
    write_json(out/'results.json',{'freeze':freeze,'validation':read_json(out/'validation-report.json'),
                                  'test':results,'trace_sha256':{f.name:sha(f) for f in sorted(out.glob('*.csv'))}})


if __name__=='__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase',choices=('prepare','validate','test'))
    parser.add_argument('--runner',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--seam',type=Path,required=True)
    parser.add_argument('--train-images',type=Path,default=ROOT/'artifacts/S13/OPENZDM')
    parser.add_argument('--reuse-fit',type=Path)
    parser.add_argument('--fit-runner',type=Path)
    parser.add_argument('--converge',action='store_true')
    parser.add_argument('--prior-target',action='store_true')
    parser.add_argument('--review-percent',type=int,choices=(6,10),default=10)
    options = parser.parse_args()
    {'prepare':prepare,'validate':validate,'test':test}[options.phase](options)
