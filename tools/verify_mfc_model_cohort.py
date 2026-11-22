"""Compare MFC/DB single-pass scores and verdicts with the CLI results."""

import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path
from shutil import copyfile

from tools.verify_folder_e2e import REGISTRATION, read_database, verify_folder


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(app, output, manifest, reference, version):
    if version != "openzdm-" + REGISTRATION:
        raise ValueError(f"Unsupported cohort model: {version}")
    expected_config = sha(Path("config/pooled-smooth-220.json"))

    source = json.loads(manifest.read_bytes())
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
    verify_folder(app, input_folder, output, None, None, 900, expected_hashes=hashes, tcp_config=REGISTRATION)

    gui = json.loads((output / 'verification.json').read_bytes())
    batch_id = gui['batch_id']  # verified as a UUID by verify_folder
    query = read_database()
    records = query(
        "SELECT i.source_path,i.source_sha256,i.config_sha256,i.algorithm_version,i.decision,i.reason,"
        "a.stage,a.score,a.duration_ms,IFNULL(p.stage,'') "
        "FROM inspection_batch_members b JOIN inspections i USING(inspection_id) "
        "JOIN analysis_attempts a ON a.inspection_id=i.inspection_id AND a.sequence_no=1 "
        "LEFT JOIN analysis_attempts p ON p.inspection_id=i.inspection_id AND p.sequence_no=2 "
        f"WHERE b.batch_id='{batch_id}' ORDER BY b.sequence_no;")
    with reference.open(encoding='utf8', newline='') as file:
        cli_rows = list(csv.DictReader(file))
    cli = {r['source_path']: r for r in cli_rows}
    if len(cli) != len(cli_rows) or not rows.keys() <= cli.keys():
        raise ValueError('CLI reference does not cover the unique image paths')
    seen = set()
    reasons, decisions, transitions = Counter(), Counter(), Counter()
    evidence = output / 'cohort.csv'
    with evidence.open('x', encoding='utf8', newline='') as file:
        writer = csv.writer(file)
        writer.writerow(['path', 'sha256', 'label', 'stage', 'score', 'analysis_ms', 'decision', 'reason'])
        for path, digest, config_hash, algorithm, decision, reason, stage, score, ms, second in records:
            name = Path(path).relative_to(input_folder.resolve()).as_posix()
            row = cli[name]
            if (name in seen or digest != hashes[name] or version != algorithm or config_hash != expected_config
                    or stage != 'LIGHTGBM' or second or not score or float(score) != float(row['score'])
                    or decision != row['decision'] or reason != row['reason'] or row['label'] != rows[name]['label']):
                raise ValueError(f'MFC/CLI identity, score, verdict or reason differs: {name}')
            seen.add(name)
            decisions[row['label'] + '/' + decision] += 1
            transitions[row['decision'] + '/' + decision] += 1
            reasons[reason] += 1
            writer.writerow([name, digest, row['label'], stage, score, ms, decision, reason])
    if seen != rows.keys():
        raise ValueError('Persisted cohort differs from the manifest')
    result = {'batch_id': batch_id, 'n': len(seen), 'file_sha256': sha(evidence),
               'reference_sha256': sha(reference), 'manifest_sha256': sha(manifest), 'executable_sha256': sha(app),
               'algorithm_version': version, 'config_sha256': expected_config, 'decisions': decisions,
               'cli_to_mfc': transitions, 'reasons': reasons, 'identical_scores_and_verdicts': len(seen)}
    (output / 'cohort.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for key in ('app', 'output', 'manifest', 'reference'):
        parser.add_argument(key, type=Path)
    parser.add_argument('version')
    args = parser.parse_args()
    verify(args.app, args.output, args.manifest, args.reference, args.version)
