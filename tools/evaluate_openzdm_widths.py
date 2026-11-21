"""Test transport and local training outside the original 385–415 pixel cohort."""

import argparse
import subprocess
from collections import Counter
from pathlib import Path

from tools.evaluate_openzdm_patterns import INPUTS, calibrate, counts, manifest, paired
from tools.evaluate_openzdm_policy import ORIGINAL, ROOT, metrics, read_json, read_rows, run, sha, verify_live, write_json


def outside(r):
    return not 385<=int(r['width'])<=415


def budget_study(args):
    out=args.output
    out.mkdir(parents=True,exist_ok=False)
    parent=read_json(args.width/'freeze.json')
    if parent['selected']!='local' or any(sha(Path(p))!=h for p,h in parent['hashes'].items()):
        raise ValueError('Expected the frozen width-specific model')
    precise={r['source_path']:r for r in read_rows(args.precise)}
    original=read_json(ORIGINAL)
    for area,base,source in [('inside',args.tile/'whole.json',args.pattern/'calibration.json'),
                             ('outside',args.width/'local.json',args.width/'cal.json')]:
        inherited=read_json(base)
        draft=inherited|{'all_widths':1,'packed':1,'tree_count':48,
                         'forests':str((base.parent/inherited['forests']).resolve())}
        write_json(out/'probe.json',draft)
        rows=run(args.runner,'whole',source,out/'probe.json',out/f'cal-{area}.csv')
        images=read_json(source)['rows']
        for r,im in zip(rows,images,strict=True):
            p=precise[r['path']]
            if p['image_sha256']!=im['sha256'] or p['label']!=r['label']:raise ValueError('Calibration input mismatch')
            r.update(score=r['fast_score'],precise_score=p['score'])
        if area=='inside':
            targets={'KO/PASS':sum(r['label']=='KO' for r in rows)*243/1346,
                     'OK/FAIL':sum(r['label']=='OK' for r in rows)*598/1829}
        else:
            baseline=read_rows(args.width/'cal-original.csv')
            for r,t in zip(baseline,rows,strict=True):
                if r['path']!=t['path']:raise ValueError('S12 calibration input mismatch')
                r.update(score=r['fast_score'],precise_score=t['precise_score'])
            targets=counts(baseline,original,'score')
        cfg=calibrate(rows,'score',draft,targets,7)
        write_json(out/(area+'.json'),cfg)
    integrated=read_json(out/'inside.json')|{'all_widths':0,'outside_config':'outside.json'}
    write_json(out/'integrated.json',integrated)
    cohort=read_json(args.pattern/'validation.json')['rows']
    cohort.extend(read_json(args.width/'val.json')['rows'])
    manifest(sorted(cohort,key=lambda r:r['path']),INPUTS/'images',out/'validation.json')
    primary={r['path'] for r in read_json(INPUTS/'test-primary.json')['rows']}
    def measure(phase,inputs,order):
        data={}
        for name in order:
            base=name=='S12'
            cfg=ORIGINAL if base else (args.width if name=='full96' else out)/'integrated.json'
            rows=run(args.control if base else args.runner,'global' if base else 'whole',inputs,cfg,out/f'{phase}-{name}.csv')
            verify_live([r for r in rows if not outside(r)],read_json(cfg))
            cfg2=ORIGINAL if base else (args.width/'local.json' if name=='full96' else out/'outside.json')
            verify_live([r for r in rows if outside(r)],read_json(cfg2))
            data[name]=rows
        reports={}
        for group in ('all','inside','outside','primary'):
            if phase=='validation' and group=='primary': continue
            subset={n:[r for r in rows if group=='all' or (phase=='validation' or r['path'] in primary) and
                       (group=='primary' or (group=='outside')==outside(r))] for n,rows in data.items()}
            reports[group]={'metrics':{n:metrics(rows) for n,rows in subset.items()},
                            'paired_vs_S12':paired(subset['S12'],subset['half48']),
                            'paired_vs_96':paired(subset['full96'],subset['half48'])}
        return reports
    modes=('S12','full96','half48')
    validation=measure('validation',out/'validation.json',modes)
    files=[args.runner,args.control,ORIGINAL,out/'inside.json',out/'outside.json',out/'integrated.json',
           args.tile/'forests/whole.xml.gz',args.width/'forests/whole.xml.gz',args.width/'freeze.json',
           out/'validation.json',INPUTS/'test.json',INPUTS/'test-primary.json']
    freeze={'files':{str(f.resolve()):sha(f) for f in files},'validation':validation}
    write_json(out/'freeze.json',freeze)
    tests=[]
    for i,order in enumerate((modes,tuple(reversed(modes))),1):
        if any(sha(Path(p))!=h for p,h in freeze['files'].items()):raise ValueError('Changed width study freeze')
        tests.append(measure(f'test-{i}',INPUTS/'test.json',order))
    write_json(out/'results.json',{'freeze':freeze,'test':tests,'traces':{f.name:sha(f) for f in out.glob('*.csv')}})


def execute(args):
    out=args.output
    out.mkdir(parents=True,exist_ok=False)
    catalog={r['path']:r for r in read_rows(args.manifest)}
    def entry(r):
        return dict(r,sha256=r['file_sha256'])
    population=[entry(r) for r in catalog.values() if r['split']=='train' and outside(r)]
    fit=[r for r in population if int(r['sha256'][:2],16)>=48]
    cal=[r for r in population if int(r['sha256'][:2],16)<48]
    manifest(fit,args.images,out/'fit.json')
    manifest(cal,args.images,out/'cal.json')
    validation=[r for r in read_json(INPUTS/'validation.json')['rows'] if outside(catalog[r['path']])]
    manifest(validation,INPUTS/'images',out/'val.json')
    common=read_json(ORIGINAL)|{'all_widths':1,'packed':1,'descriptor':'tiles'}
    write_json(out/'train.json',common)
    subprocess.run([str(args.runner),'fit-whole',str(out/'fit.json'),str(out/'train.json'),str(out/'forests')],check=True)
    previous=read_json(args.tile/'whole.json')
    transfer=previous|common|{'forests':str((args.tile/'forests').resolve())}
    for key in ('fast_pass_max','fast_fail_min'):transfer[key]=previous[key]
    write_json(out/'transfer.json',transfer)
    precise={r['source_path']:r for r in read_rows(args.precise)}
    original=run(args.control,'global',out/'cal.json',ORIGINAL,out/'cal-original.csv')
    by_path={r['path']:r for r in cal}
    for r in original:
        p=precise[r['path']]
        if p['image_sha256']!=by_path[r['path']]['sha256'] or r['label']!=p['label']: raise ValueError(r['path'])
        r.update(global_score=r['fast_score'],precise_score=p['score'])
    targets=counts(original,common,'global_score')
    for name,folder in [('recalibrated',transfer['forests']),('local','forests')]:
        draft=common|{'forests':folder}
        write_json(out/'probe.json',draft)
        rows=run(args.runner,'whole',out/'cal.json',out/'probe.json',out/f'cal-{name}.csv')
        for a,b in zip(rows,original,strict=True):
            if a['path']!=b['path']:raise ValueError('Unpaired')
            a.update(score=a['fast_score'],precise_score=b['precise_score'])
        policy=calibrate(rows,'score',draft,targets,7)
        write_json(out/(name+'.json'),policy)
    data={}
    for name in ('S12','transfer','recalibrated','local'):
        cfg=ORIGINAL if name=='S12' else out/(name+'.json')
        rows=run(args.control if name=='S12' else args.runner,'global' if name=='S12' else 'whole',
                 out/'val.json',cfg,out/f'val-{name}.csv')
        verify_live(rows,read_json(cfg))
        data[name]=rows
    candidates=[]
    stats={n:metrics(rows) for n,rows in data.items()}
    for name,m in stats.items():
        ko,ok=m['counts']['KO'],m['counts']['OK']
        ratio=max(ko['PASS']/stats['S12']['counts']['KO']['PASS'],ok['FAIL']/stats['S12']['counts']['OK']['FAIL'])
        if (ratio<1 and (ko['REVIEW']+ok['REVIEW'])*10<=m['n'] and m['precise_calls']*4<=m['n']
                and ko['ERROR']+ok['ERROR']==0):candidates.append((ratio,name))
    selected=min(candidates)[1] if candidates else 'S12'
    print('Outside selection:',selected,flush=True)
    config=previous|{'forests':transfer['forests'],'packed':1}
    if selected!='S12':config['outside_config']=selected+'.json'
    write_json(out/'integrated.json',config)
    files=[ORIGINAL,args.runner,args.control,out/'integrated.json',args.tile/'forests/whole.xml.gz',
           INPUTS/'test.json',INPUTS/'test-primary.json',out/'fit.json',out/'cal.json',out/'val.json',
           out/'forests/whole.xml.gz',*(out/(n+'.json') for n in ('local','transfer','recalibrated'))]
    freeze={'selected':selected,'hashes':{str(p.resolve()):sha(p) for p in files},'validation':stats,
            'validation_paired':{n:paired(data['S12'],rows) for n,rows in data.items() if n!='S12'},
            'train':{n:dict(Counter(r['label'] for r in g)) for n,g in [('fit',fit),('calibration',cal)]}}
    write_json(out/'freeze.json',freeze)
    primary={r['path'] for r in read_json(INPUTS/'test-primary.json')['rows']}
    result={'freeze':freeze,'test':[]}
    inside=read_json(args.tile/'whole.json')
    outside_cfg=read_json(ORIGINAL if selected=='S12' else out/(selected+'.json'))
    for i,order in enumerate((('S12','integrated'),('integrated','S12')),1):
        if any(sha(Path(p))!=h for p,h in freeze['hashes'].items()): raise ValueError('Freeze mismatch')
        live={}
        for mode in order:
            base=mode=='S12'
            rows=run(args.control if base else args.runner,'global' if base else 'whole',INPUTS/'test.json',
                     ORIGINAL if base else out/'integrated.json',out/f'test-{i}-{mode}.csv')
            verify_live([r for r in rows if outside(r)],read_json(ORIGINAL) if base else outside_cfg)
            verify_live([r for r in rows if not outside(r)],read_json(ORIGINAL) if base else inside)
            live[mode]=rows
        report={}
        for group in ('official','primary','inside','outside'):
            subset={n:[r for r in rows if group=='official' or r['path'] in primary and
                       (group=='primary' or (group=='outside')==outside(r))] for n,rows in live.items()}
            report[group]={'metrics':{n:metrics(rows) for n,rows in subset.items()},'paired':paired(subset['S12'],subset['integrated'])}
        result['test'].append(report)
    result['traces']={p.name:sha(p) for p in out.glob('*.csv')}
    write_json(out/'results.json',result)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    base=ROOT/'artifacts/OPENZDM-EDA'
    for n,p in [('runner',base/'width-build/Release/openzdm_shape.exe'),('control',base/'shape-build/Release/openzdm_eda_final.exe'),
                ('tile',base/'tile-study'),('manifest',ROOT/'artifacts/S02/run-01/manifest.csv'),
                ('images',ROOT/'artifacts/S13/OPENZDM'),('precise',ROOT/'artifacts/S07/review-train-01/precise/results.csv'),
                ('width',base/'width-study'),('pattern',base/'pattern-converged')]:
        parser.add_argument('--'+n,type=Path,default=p)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--halve-trees',action='store_true')
    args=parser.parse_args()
    (budget_study if args.halve_trees else execute)(args)
