"""Compare MFC/DB first-stage scores with a frozen CLI trace for the same PNGs."""

import argparse
import csv
from collections import Counter
from pathlib import Path
from shutil import copyfile

from tools.evaluate_openzdm_policy import read_json, read_rows, sha, write_json
from tools.verify_folder_e2e import REGISTRATION, read_database, verify_folder


def verify(app, output, manifest, reference, version):
    if version == "openzdm-" + REGISTRATION:
        config_path = Path("config/pooled-smooth-220.json")
        fast_stage = "POOLED_WIDTH"
        tcp_config = REGISTRATION
    elif version == "local-contrast-selective-v1":
        config_path = Path("docs/steps/S11-evidence/config/selective.json")
        fast_stage = "FAST"
        tcp_config = "baseline-v1"
    else:
        raise ValueError(f'Unsupported cohort model: {version}')
    expected_config = sha(config_path)

    source = read_json(manifest)
    source_root = (manifest.parent / source['root']).resolve(strict=True)
    rows = {row['path']: row for row in source['rows']}
    if not rows or len(rows) != len(source['rows']):
        raise ValueError('Expected unique, nonempty manifest paths')
    input_folder = output.parent / 'inputs'
    input_folder.mkdir(exist_ok=False)
    hashes = {}
    for name, row in rows.items():
        file = (source_root / name).resolve(strict=True)
        target = (input_folder / name).resolve()
        if not file.is_relative_to(source_root) or not target.is_relative_to(input_folder.resolve()):
            raise ValueError(f'Invalid manifest path: {name}')
        target.parent.mkdir(parents=True, exist_ok=True)
        copyfile(file, target)
        if sha(target) != row['sha256'] or row['label'] not in ('OK', 'KO'):
            raise ValueError(f'Invalid manifest identity: {name}')
        hashes[name] = row['sha256']
    verify_folder(app, input_folder, output, None, None, 900, expected_hashes=hashes, tcp_config=tcp_config)

    gui = read_json(output / 'verification.json')
    batch_id = gui['batch_id']  # verified as a UUID by verify_folder
    query = read_database()
    records = query(
        "SELECT i.source_path,i.source_sha256,i.config_sha256,i.algorithm_version,i.decision,i.reason,"
        "a.stage,a.score,a.duration_ms,IFNULL(p.stage,''),IFNULL(p.score,''),IFNULL(p.duration_ms,'') "
        "FROM inspection_batch_members b JOIN inspections i USING(inspection_id) "
        "JOIN analysis_attempts a ON a.inspection_id=i.inspection_id AND a.sequence_no=1 "
        "LEFT JOIN analysis_attempts p ON p.inspection_id=i.inspection_id AND p.sequence_no=2 "
        f"WHERE b.batch_id='{batch_id}' ORDER BY b.sequence_no;")
    cli_rows = read_rows(reference)
    cli = {r['path']: r for r in cli_rows}
    if len(cli) != len(cli_rows) or not rows.keys() <= cli.keys():
        raise ValueError('CLI reference does not cover the unique image paths')
    seen = set()
    reasons, decisions, transitions = Counter(), Counter(), Counter()
    precise_calls = 0
    evidence = output / 'cohort.csv'
    with evidence.open('x', encoding='utf8', newline='') as file:
        writer = csv.writer(file)
        writer.writerow(['path', 'sha256', 'label', 'fast_stage', 'fast_score', 'fast_ms', 'precise_stage',
                         'precise_score', 'precise_ms', 'decision', 'reason', 'cli_decision'])
        for path, digest, config_hash, algorithm, decision, reason, stage, score, ms, second, pscore, pms in records:
            name = Path(path).relative_to(input_folder.resolve()).as_posix()
            row = cli[name]
            if (name in seen or digest != hashes[name] or version != algorithm or config_hash != expected_config
                    or stage != fast_stage or not score or
                    float(score) != float(row['fast_score']) or row['label'] != rows[name]['label']):
                raise ValueError(f'MFC/CLI identity or first-stage score differs: {name}')
            if second and row['precise_score'] and float(pscore) != float(row['precise_score']):
                raise ValueError(f'Precise score differs: {name}')
            seen.add(name)
            decisions[row['label'] + '/' + decision] += 1
            transitions[row['decision'] + '/' + decision] += 1
            reasons[reason] += 1
            precise_calls += bool(second)
            writer.writerow([name, digest, row['label'], stage, score, ms, second, pscore, pms,
                             decision, reason, row['decision']])
    if seen != rows.keys():
        raise ValueError('Persisted cohort differs from the manifest')
    write_json(output / 'cohort.json', {'batch_id': batch_id, 'n': len(seen), 'file_sha256': sha(evidence),
               'reference_sha256': sha(reference), 'manifest_sha256': sha(manifest), 'executable_sha256': sha(app),
               'algorithm_version': version, 'config_sha256': expected_config, 'decisions': decisions,
               'cli_to_mfc': transitions, 'reasons': reasons, 'precise_calls': precise_calls,
               'identical_first_scores': len(seen)})


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('app', 'output', 'manifest', 'reference'):
        parser.add_argument(key, type=Path)
    parser.add_argument('version')
    args = parser.parse_args()
    verify(args.app, args.output, args.manifest, args.reference, args.version)
