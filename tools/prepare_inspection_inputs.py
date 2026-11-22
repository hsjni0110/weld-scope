"""Prepare verified OPENZDM validation and test images with the frozen split rules."""

import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path, PurePosixPath
from tempfile import TemporaryDirectory
from zipfile import ZipFile

from tools.prepare_openzdm import validate_members, verify_archive, write_json

PREFIX = {"train": "train", "validation": "val", "test": "test"}
LABELS = ("OK", "KO")


def checksum(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def hex_value(value, size):
    if len(value) != size or any(char not in "0123456789abcdef" for char in value):
        raise ValueError(f"Invalid {size}-digit hash")
    return int(value, 16)


def image_path(path, prefix, label):
    parts = PurePosixPath(path).parts
    if (label not in LABELS or len(parts) != 3 or parts[:2] != (prefix, label)
            or path != "/".join(parts) or PurePosixPath(path).suffix.lower() != ".png"
            or "\\" in path or ":" in path):
        raise ValueError(f"Invalid {prefix} row: {path}")
    return parts


def make_cohorts(rows, pairs):
    """Use connected components of the recorded S02 dHash graph across all splits."""
    parents = {}
    seen = set()
    for row in rows:
        path, split = row["path"], row["split"]
        if split not in PREFIX or row["status"] != "valid" or path.casefold() in seen:
            raise ValueError(f"Invalid or duplicate S02 row: {path}")
        image_path(path, PREFIX[split], row["label"])
        hex_value(row["dhash"], 16)
        hex_value(row["file_sha256"], 64)
        seen.add(path.casefold())
        parents[row["dhash"]] = row["dhash"]

    def root(key):
        while key != parents[key]:
            parents[key] = parents[parents[key]]
            key = parents[key]
        return key

    for left, right, distance in pairs:
        if left not in parents or right not in parents or (int(left, 16) ^ int(right, 16)).bit_count() != distance:
            raise ValueError(f"Unknown or invalid S02 pair: {left}, {right}")
        if distance <= 5:
            a, b = sorted((root(left), root(right)))
            parents[b] = a

    groups = defaultdict(set)
    for row in rows:
        groups[root(row["dhash"])].add(row["split"])
    cohorts = {"validation": [], "test-primary": [], "test": []}
    excluded = []
    for row in sorted(rows, key=lambda item: item["path"]):
        split, label, path = row["split"], row["label"], row["path"]
        if split == "train":
            continue
        candidate = dict(path=path, split=split, label=label, sha256=row["file_sha256"], debug=0)
        if split == "test":
            cohorts["test"].append(candidate)
        involved = groups[root(row["dhash"])]
        if len(involved) == 1:
            cohorts["validation" if split == "validation" else "test-primary"].append(candidate)
        else:
            excluded.append(dict(path=path, split=split, label=label, group=root(row["dhash"]), splits=sorted(involved),
                                 reason="cross_split_dhash_component"))
    return cohorts, excluded


def materialize(archive, rows, destination):
    """Stage only one split and verify bytes before publishing its directory."""
    if not rows or destination.exists():
        raise ValueError(f"Input split is empty or output already exists: {destination}")
    validate_members(archive.infolist())
    destination.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix=f".{destination.name}-preparing-", dir=destination.parent) as temporary:
        staging = Path(temporary)
        images = staging / "verified"
        seen = set()
        for row in rows:
            path = row["path"]
            parts = image_path(path, destination.name, path.split("/")[1] if "/" in path else "")
            if path.casefold() in seen:
                raise ValueError(f"Duplicate archive row: {path}")
            seen.add(path.casefold())
            data = archive.read(path)
            if hashlib.sha256(data).hexdigest() != row["sha256"]:
                raise ValueError(f"Image checksum mismatch: {path}")
            target = images.joinpath(*parts[1:])
            if not target.resolve().is_relative_to(staging.resolve()):
                raise ValueError(f"Unsafe archive destination: {path}")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
        images.rename(destination)


def match_archive(archive_path, expected):
    actual = verify_archive(archive_path)
    if actual != expected:
        raise ValueError("S02 archive SHA-256 mismatch")
    return actual


def prepare(output, evidence, archive_path):
    if output.exists():
        raise FileExistsError(f"Output already exists: {output}")
    manifest = evidence / "manifest.csv"
    pair_list = evidence / "similar_hash_pairs.csv"
    rows = read_csv(manifest)
    pairs = [(row["left_dhash"], row["right_dhash"], int(row["distance"])) for row in read_csv(pair_list)]
    cohorts, excluded = make_cohorts(rows, pairs)
    if not all({row["label"] for row in selection} == set(LABELS) for selection in cohorts.values()):
        raise ValueError("Both labels must be represented in each S11 cohort")
    archive_hash = match_archive(archive_path, read_json(evidence / "archive-checksum.json")["sha256"])
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix=f".{output.name}-preparing-", dir=output.parent) as temporary:
        staging = Path(temporary) / "evaluation"
        inputs = staging / "inputs"
        inputs.mkdir(parents=True)
        for name, selection in cohorts.items():
            write_json(inputs / f"{name}.json", dict(schema_version=1, root="images", rows=selection,
                                                    source_archive_sha256=archive_hash))
        write_json(inputs / "excluded.json", excluded)
        summary = {
            "source_archive_sha256": archive_hash,
            "s02_manifest_sha256": checksum(manifest),
            "s02_similar_pairs_sha256": checksum(pair_list),
            "cohorts": {name: dict(Counter(row["label"] for row in selection)) for name, selection in cohorts.items()},
            "excluded": {split: dict(Counter(row["label"] for row in excluded if row["split"] == split))
                         for split in ("validation", "test")},
            "sha256": {file.name: checksum(file) for file in sorted(inputs.glob("*.json"))},
        }
        write_json(inputs / "provenance.json", summary)
        with ZipFile(archive_path) as archive:
            materialize(archive, cohorts["validation"], inputs / "images" / "val")
            materialize(archive, cohorts["test"], inputs / "images" / "test")
        staging.rename(output)
    print(json.dumps(summary["cohorts"], sort_keys=True))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, default=Path("docs/steps/S02-evidence"))
    parser.add_argument("--archive", type=Path, default=Path("data/openzdm/Weld Aesthetical Defects.zip"))
    args = parser.parse_args()
    prepare(args.output, args.evidence, args.archive)
