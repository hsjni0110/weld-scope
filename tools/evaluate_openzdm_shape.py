"""Compare cross-section geometry with texture on frozen OPENZDM cohorts."""

import argparse
import subprocess
from pathlib import Path

from tools.evaluate_openzdm_patterns import INPUTS, calibrate, manifest, paired
from tools.evaluate_openzdm_policy import auc, ORIGINAL, metrics, read_json, read_rows, run, sha, verify_live, write_json

TARGETS = {'KO':243/1346,'OK':598/1829}
ROOT = Path(__file__).resolve().parents[1]


def live(args, mode, inputs, path):
    if mode in args.forests:
        runner,feature,cfg=args.runner,mode,args.output/(mode+'.json')
    else:
        runner=args.control
        feature='global' if mode=='S12' else 'pattern-C'
        cfg={'S12':ORIGINAL,'linear':args.output/'linear.json','prior_low':args.prior/'joint.json'}[mode]
    rows=run(runner,feature,inputs,cfg,path)
    verify_live([r for r in rows if 385<=int(r['width'])<=415],read_json(cfg))
    verify_live([r for r in rows if not 385<=int(r['width'])<=415],read_json(ORIGINAL))
    return rows


def summarize(data):
    result={}
    for name,rows in data.items():
        m=metrics(rows)
        m['auc']=auc([dict(r,score=float(r['fast_score'])) for r in rows],'score',1)
        m['timeouts']=sum(r['reason']=='budget_exhausted' for r in rows)
        result[name]=m
    pairs=[('linear','texture'),('texture','combined'),('shape','combined'),('prior_low','combined'),
           ('S12','combined'),('prior_low','texture'),('prior_low','shape'),('center','whole'),('outer','whole'),
           ('prior_low','whole'),('prior_low','center'),('prior_low','outer'),('S12','whole'),('linear','whole')]
    return {'metrics':result,'paired':{f'{a}_to_{b}':paired(data[a],data[b]) for a,b in pairs if a in data and b in data}}


def eligible(m):
    ko,ok=m['counts']['KO'],m['counts']['OK']
    ko_ratio=(ko['PASS']/sum(ko.values()))/TARGETS['KO']
    ok_ratio=(ok['FAIL']/sum(ok.values()))/TARGETS['OK']
    meets=(ko_ratio<=1 and ok_ratio<1 and (ko['REVIEW']+ok['REVIEW'])*10<=m['n']
           and m['precise_calls']*4<=m['n'] and ok['ERROR']+ko['ERROR']==0)
    return meets,max(ko_ratio,ok_ratio)


def train(args):
    args.output.mkdir(parents=True,exist_ok=False)
    write_json(args.output/'train.json',read_json(ORIGINAL)|{'descriptor':args.descriptor})
    subprocess.run([str(args.runner),'fit',str(args.pattern/'fit.json'),str(args.output/'train.json'),str(args.output/'forests')],check=True)


def validate(args):
    out=args.output
    previous=read_json(args.pattern/'freeze.json')
    for f in ('fit.json','calibration.json','validation.json'):
        if sha(args.pattern/f)!=previous['files'][f]: raise ValueError('Changed partition: '+f)
    cfg=read_json(ORIGINAL)
    write_json(out/'scores.json',cfg|{'forests':'forests','descriptor':args.descriptor})
    rows=run(args.runner,'scores',args.pattern/'calibration.json',out/'scores.json',out/'calibration-scores.csv')
    old={r['path']:r for r in read_rows(args.pattern/'scores-cal.csv')}
    precise={r['path']:r for r in read_rows(args.seam)}
    for r in rows:
        r['linear']=old[r['path']]['C']
        r['precise_score']=precise[r['path']]['precise_score']
    targets={label+'/'+d:rate*sum(r['label']==label for r in rows) for label,d,rate in
             [('KO','PASS',TARGETS['KO']),('OK','FAIL',TARGETS['OK'])]}
    for name in ('linear',*args.forests):
        policy=calibrate(rows,name,cfg,targets,7)
        if name=='linear': policy['models']=read_json(args.pattern/'models.json')['models']
        else: policy.update(forests='forests',descriptor=args.descriptor)
        write_json(out/(name+'.json'),policy)
    validation={n:live(args,n,args.pattern/'validation.json',out/f'val-{n}.csv') for n in args.modes}
    result=summarize(validation)
    prior={r['path']:r for r in read_rows(args.regions/'validation.csv')}
    result['vs_prior_regions']={n:paired([dict(prior[r['path']],decision=prior[r['path']]['both_decision']) for r in rows],rows)
                                for n,rows in validation.items()}
    candidates=[(eligible(result['metrics'][n])[1],n) for n in args.forests if eligible(result['metrics'][n])[0]]
    result['selected']=min(candidates)[1] if candidates else None
    write_json(out/'validation-report.json',result)
    lookup={r['path']:r for r in read_rows(args.manifest)}
    primary=read_json(INPUTS/'test-primary.json')['rows']
    manifest([r for r in primary if 385<=int(lookup[r['path']]['width'])<=415],INPUTS/'images',out/'test-target.json')
    files=['validation-report.json','test-target.json',*(name+'.json' for name in ('linear',*args.forests)),
           *('forests/'+name+'.xml.gz' for name in args.forests)]
    write_json(out/'freeze.json',{'files':{f:sha(out/f) for f in files},
        'external':{str(p.resolve()):sha(p) for p in (args.runner,args.control,INPUTS/'test.json',INPUTS/'test-primary.json',
                                                       ORIGINAL,args.prior/'joint.json',args.pattern/'freeze.json')},
        'selection':result['selected']})
    print('Validation selection:',result['selected'],flush=True)


def test(args):
    out=args.output
    freeze=read_json(out/'freeze.json')
    if any(sha(out/f)!=h for f,h in freeze['files'].items()) or any(sha(Path(p))!=h for p,h in freeze['external'].items()):
        raise ValueError('The frozen evaluation inputs changed')
    target={r['path'] for r in read_json(out/'test-target.json')['rows']}
    primary={r['path'] for r in read_json(INPUTS/'test-primary.json')['rows']}
    result={'freeze':freeze,'validation':read_json(out/'validation-report.json'),'test':[]}
    for i,order in enumerate((args.modes,tuple(reversed(args.modes))),1):
        full={}
        for mode in order:
            inputs=out/'test-target.json' if mode=='prior_low' else INPUTS/'test.json'
            full[mode]=live(args,mode,inputs,out/f'test-{i}-{mode}.csv')
        part={name:[r for r in rows if r['path'] in target] for name,rows in full.items()}
        report={'target':summarize(part)}
        for cohort in ('primary','official'):
            report[cohort]={name:metrics([r for r in rows if cohort=='official' or r['path'] in primary])
                            for name,rows in full.items() if name!='prior_low'}
        result['test'].append(report)
    keys=read_rows(args.regions/'answer-key.csv')
    ref={r['path']:r for r in read_rows(args.seam)}
    manifest([ref[r['path']] for r in keys],INPUTS/'images',out/'examples.json')
    examples={r['case']:{'path':r['path'],'label':r['label']} for r in keys}
    for mode in args.modes:
        for k,r in zip(keys,live(args,mode,out/'examples.json',out/f'examples-{mode}.csv'),strict=True):
            examples[k['case']][mode]={key:r[key] for key in ('decision','fast_score','reason')}
    result['examples']=examples
    result['traces']={p.name:sha(p) for p in sorted(out.glob('*.csv'))}
    write_json(out/'results.json',result)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('phase',choices=('train','validate','test'))
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--descriptor',choices=('sections','tiles','sorted'),default='sections')
    base=ROOT/'artifacts/OPENZDM-EDA'
    for name,path in [('runner',base/'shape-build/Release/openzdm_shape.exe'),('control',base/'shape-build/Release/openzdm_eda_final.exe'),
                      ('pattern',base/'pattern-converged'),('prior',base/'pattern-joint'),('regions',base/'regions-verified'),
                      ('seam',base/'seam-controls/scores.csv'),('manifest',ROOT/'artifacts/S02/run-01/manifest.csv')]:
        parser.add_argument('--'+name,type=Path,default=path)
    args=parser.parse_args()
    args.forests=('texture','shape','combined') if args.descriptor=='sections' else ('outer','center','whole')
    args.modes=('S12','prior_low','linear',*args.forests)
    {'train':train,'validate':validate,'test':test}[args.phase](args)
