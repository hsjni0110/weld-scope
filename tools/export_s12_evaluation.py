"""Rebuild the local S11 evidence ZIP from a preserved frozen evaluation."""

import argparse
from collections import Counter
import os
from pathlib import Path
from shutil import copyfile
from tempfile import TemporaryDirectory
from zipfile import ZIP_DEFLATED, ZipFile

from tools.evaluate_s11 import (DECISIONS, MODES, checksum, read_inspection_result, read_json,
                                test_comparisons, verified_inputs, verify_frozen_evidence)


REPO = Path(__file__).resolve().parents[1]
OPENCV = REPO / ".tools/opencv/install-x64"
NOTICE_FILES = {
    "THIRD-PARTY-NOTICES.md": REPO / "docs/steps/S12-third-party-notices.md",
    "licenses/OpenCV.txt": OPENCV / "LICENSE",
    "licenses/libpng.txt": OPENCV / "etc/licenses/libpng-LICENSE",
    "licenses/zlib.txt": OPENCV / "etc/licenses/zlib-LICENSE",
}
RUNS = ("run-01", "run-02")


def result_files(directory):
    yield "report.json", directory / "report.json"
    for run in RUNS:
        for mode in MODES:
            for name in ("results.csv", "summary.json"):
                path = f"{run}/{mode}/{name}"
                yield path, directory / path


def verify_test_report(directory, evaluation, inputs, freeze):
    manifest = inputs / "test.json"
    primary = {row["path"] for row in read_json(inputs / "test-primary.json")["rows"]}
    results = []
    for run in RUNS:
        modes = {}
        for mode in MODES:
            mode_dir = directory / run / mode
            summary = read_json(mode_dir / "summary.json")
            rows = read_inspection_result(mode, evaluation / "config" / f"{mode}.json", manifest, mode_dir, summary)
            counts = Counter(row["decision"] for row in rows)
            if (summary.get("scope") != "test-frozen" or summary.get("evaluation_split") != "test"
                    or summary.get("algorithm_version") != freeze["algorithm_version"][mode]
                    or summary.get("requested") != len(rows)
                    or any(summary.get(decision) != counts[decision] for decision in DECISIONS)):
                raise ValueError(f"Run summary does not match frozen results: {directory.name}/{run}/{mode}")
            modes[mode] = rows
        results.append(modes)
    report = read_json(directory / "report.json")
    if (report.get("freeze_sha256") != checksum(evaluation / "freeze.json")
            or report.get("cohorts") != test_comparisons(results, primary)):
        raise ValueError(f"Test report does not match frozen results: {directory.name}")


def export_evaluation(evaluation, inspect, calibrator, output, replay=None):
    if output.suffix.lower() != ".zip":
        raise ValueError("Output must be a .zip file")
    if output.exists() or output.is_symlink():
        raise FileExistsError(f"Output already exists: {output}")

    members = dict(NOTICE_FILES)
    members.update({
        "original/weldscope_inspect.exe": inspect,
        "original/weldscope_calibrate_s11.exe": calibrator,
        "evaluation/freeze.json": evaluation / "freeze.json",
        "evaluation/validation/selection.json": evaluation / "validation/selection.json",
    })
    for name in ("validation", "test", "test-primary", "excluded", "provenance"):
        members[f"evaluation/inputs/{name}.json"] = evaluation / "inputs" / f"{name}.json"
    for mode in MODES:
        members[f"evaluation/config/{mode}.json"] = evaluation / "config" / f"{mode}.json"
    members.update((f"evaluation/test/{name}", path) for name, path in result_files(evaluation / "test"))
    if replay is not None:
        members.update((f"replay/{name}", path) for name, path in result_files(replay))

    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix=".s12-export-", dir=output.parent) as temporary:
        staging = Path(temporary)
        for name, source in members.items():
            target = staging / name
            target.parent.mkdir(parents=True, exist_ok=True)
            copyfile(source, target)
        frozen = staging / "evaluation"
        inputs, provenance = verified_inputs(frozen)
        freeze = read_json(frozen / "freeze.json")
        verify_frozen_evidence(frozen, inputs, staging / "original/weldscope_inspect.exe", freeze)
        if freeze["source_archive_sha256"] != provenance["source_archive_sha256"]:
            raise ValueError("Prepared archive does not match freeze")
        if checksum(staging / "original/weldscope_calibrate_s11.exe") != freeze["calibrator_sha256"]:
            raise ValueError("Historical calibrator does not match freeze")
        verify_test_report(frozen / "test", frozen, inputs, freeze)
        if replay is not None:
            verify_test_report(staging / "replay", frozen, inputs, freeze)

        archive_path = staging / "evaluation.zip"
        with ZipFile(archive_path, "w", compression=ZIP_DEFLATED) as archive:
            hashes = []
            for name in sorted(members):
                archive.write(staging / name, name)
                hashes.append(f"{checksum(staging / name)}  {name}\n")
            archive.writestr("SHA256SUMS.txt", "".join(hashes))
        # Publish the complete archive without replacing an output created concurrently.
        os.link(archive_path, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evaluation", required=True, type=Path)
    parser.add_argument("--inspect", required=True, type=Path)
    parser.add_argument("--calibrator", required=True, type=Path)
    parser.add_argument("--replay", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    try:
        export_evaluation(args.evaluation, args.inspect, args.calibrator, args.output, args.replay)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"Export failed: {error}\n")
    print(args.output)


if __name__ == "__main__":
    main()
