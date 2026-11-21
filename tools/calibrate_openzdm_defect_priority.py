"""Fit the last width-calibration candidate with a predeclared KO safety margin."""
import argparse
import shutil
from pathlib import Path

from tools import evaluate_openzdm_boost_widths as widths
from tools.evaluate_openzdm_patterns import calibrate, counts
from tools.evaluate_openzdm_policy import read_json, read_rows, sha, write_json


def calibrate_priority(out, previous):
    prior_freeze = read_json(previous/'freeze.json')
    if prior_freeze['selected'] is not None or any(sha(Path(p)) != h for p,h in prior_freeze['hashes'].items()):
        raise ValueError('Expected the failed unmodified width calibration')
    reports = read_json(previous/'calibration-report.json')
    precise = {r['source_path']:r for r in read_rows(widths.pooled.ROOT/'artifacts/S07/review-train-01/precise/results.csv')}
    members = {r['path']:r for r in read_json(widths.PRIOR/'calibration.json')['rows']}
    out.mkdir(parents=True,exist_ok=False)
    for size in widths.SIZES:
        model = out/f'model-{size}.txt'
        shutil.copyfile(previous/model.name,model)
        settings = read_json(previous/f'config-{size}.json')
        rows = read_rows(widths.PRIOR/f'cal-{size}.csv')
        for r in rows:
            p = precise[r['path']]
            if p['label'] != r['label'] or p['image_sha256'] != members[r['path']]['sha256']:
                raise ValueError('Calibration image mismatch')
            r['precise_score'] = p['score']
        for group in ('inside','outside'):
            reference = reports[str(size)][group]['reference']
            targets = dict(reference)
            targets['KO/PASS'] *= .8
            selected = [r for r in rows if widths.inside(r)==(group=='inside')]
            policy = calibrate(selected,'fast_score',settings,targets,7)
            keys = ('fast_pass_max','fast_fail_min') if group=='inside' else ('outside_pass','outside_fail')
            settings[keys[0]],settings[keys[1]] = policy['fast_pass_max'],policy['fast_fail_min']
            reports[str(size)][group].update(targets=targets,candidate=counts(selected,policy,'fast_score'))
        write_json(out/f'config-{size}.json',settings)
    write_json(out/'calibration-report.json',{'previous_validation_freeze_sha256':sha(previous/'freeze.json'),
               'calibration_code_sha256':sha(Path(__file__)),'KO_target_fraction':.8,'reports':reports})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--previous',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    calibrate_priority(args.output,args.previous)
