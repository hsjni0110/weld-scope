"""Prepare immutable train-only inputs from the verified S02 archive."""
import argparse
import csv
import hashlib
import json
from pathlib import Path, PurePosixPath
from tempfile import TemporaryDirectory
from zipfile import ZipFile

from tools.prepare_openzdm import validate_members, verify_archive, write_json


def extract_train(archive, rows, output, debug_paths, archive_hash=''):
    if output.exists():
        raise FileExistsError(f'Output already exists: {output}')
    validate_members(archive.infolist())
    selected = sorted(rows, key=lambda row: row['path'])
    seen = set()
    for row in selected:
        path = row['path']
        parts = PurePosixPath(path).parts
        if (len(parts) != 3 or parts[0] != 'train' or parts[1] != row['label'] or path != '/'.join(parts)
                or row['label'] not in ('OK', 'KO') or row['split'] != 'train'
                or row['status'] != 'valid' or '\\' in path or ':' in path
                or path.casefold() in seen):
            raise ValueError(f'Invalid or duplicate train row: {path}')
        seen.add(path.casefold())
    if not selected or not debug_paths.issubset({row['path'] for row in selected}):
        raise ValueError('Empty train list or debug selection outside train')
    output.parent.mkdir(parents=True, exist_ok=True)
    result = {'schema_version': 1, 'root': 'images', 'rows': [], 'source_archive_sha256': archive_hash}
    with TemporaryDirectory(prefix=f'.{output.name}-preparing-', dir=output.parent) as temporary:
        staging = Path(temporary)
        for row in selected:
            path = row['path']
            data = archive.read(path)
            digest = hashlib.sha256(data).hexdigest()
            if digest != row['file_sha256']:
                raise ValueError(f'Image checksum mismatch: {path}')
            destination = staging / 'images' / path
            if not destination.resolve().is_relative_to(staging.resolve()):
                raise ValueError(f'Unsafe destination: {path}')
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(data)
            result['rows'].append(dict(path=path, split='train', label=row['label'],
                                       sha256=digest, debug=int(path in debug_paths)))
        write_json(staging / 'manifest.json', result)
        staging.rename(output)
    return result


def validate_prepared(output, expected_rows, debug_paths, archive_hash):
    actual = json.loads((output / 'manifest.json').read_text(encoding='utf-8'))
    expected = [dict(path=row['path'], split='train', label=row['label'], sha256=row['file_sha256'],
                     debug=int(row['path'] in debug_paths)) for row in sorted(expected_rows, key=lambda row: row['path'])]
    if (actual.get('schema_version') != 1 or actual.get('root') != 'images'
            or actual.get('source_archive_sha256') != archive_hash or actual.get('rows') != expected):
        raise ValueError('Prepared manifest does not match the expected S02 train selection')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, default=Path('data/openzdm/Weld Aesthetical Defects.zip'))
    parser.add_argument('--output', type=Path, default=Path('artifacts/S04/inputs'))
    parser.add_argument('--samples-only', action='store_true')
    parser.add_argument('--verify-only', action='store_true')
    args = parser.parse_args()
    evidence = Path(__file__).resolve().parents[1] / 'docs/steps/S02-evidence'
    debug_paths = set(json.loads((evidence / 'contact_selection.json').read_text(encoding='utf-8')))
    with (evidence / 'manifest.csv').open(encoding='utf-8-sig', newline='') as stream:
        rows = [row for row in csv.DictReader(stream) if row['split'] == 'train']
    if args.samples_only:
        rows = [row for row in rows if row['path'] in debug_paths]
    expected_hash = json.loads((evidence / 'archive-checksum.json').read_text(encoding='utf-8'))['sha256']
    if args.verify_only:
        validate_prepared(args.output, rows, debug_paths, expected_hash)
        print(f'Verified {len(rows)} expected train inputs')
        return
    archive_hash = verify_archive(args.archive)
    if archive_hash != expected_hash:
        raise ValueError('S02 archive SHA-256 mismatch')
    with ZipFile(args.archive) as archive:
        result = extract_train(archive, rows, args.output, debug_paths, archive_hash)
    print(f"Prepared {len(result['rows'])} train images: {args.output / 'manifest.json'}")


if __name__ == '__main__':
    main()
