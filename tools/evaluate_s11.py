"""Prepare OPENZDM cohorts, freeze policy on validation, and compare S11 on test."""

import argparse
from collections import Counter, defaultdict
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import subprocess
from tempfile import TemporaryDirectory
from uuid import uuid4
from zipfile import ZipFile

from tools.prepare_openzdm import validate_members, verify_archive, write_json


MODES = ("baseline", "fast", "precise", "selective")
BOUNDARIES = ("fast_pass_max", "fast_fail_min", "precise_pass_max", "precise_fail_min")
DECISIONS = ("PASS", "FAIL", "REVIEW", "ERROR")
PREFIX = {"train": "train", "validation": "val", "test": "test"}
LABELS = ("OK", "KO")
MIN_AUTOMATIC_PERCENT = 90
MAX_PRECISE_PERCENT = 25
DEMO_IMAGE = "train/OK/image_10513.png"


def checksum(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def write_checkpoint(path, value):
    with TemporaryDirectory(prefix=f".{path.stem}-writing-", dir=path.parent) as temporary:
        staged = Path(temporary) / path.name
        write_json(staged, value)
        staged.replace(path)


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
        staging.rename(output)
    print(json.dumps(summary["cohorts"], sort_keys=True))


def nearest_rank(values, percentile):
    if not values or not 0 <= percentile <= 100:
        raise ValueError("Expected measurements and a percentile between 0 and 100")
    return sorted(values)[max(0, math.ceil(len(values) * percentile / 100) - 1)]


def ratio(count, denominator):
    return dict(count=count, denominator=denominator, rate=count / denominator if denominator else None)


def evaluate_metrics(rows):
    counts = {label: {decision: 0 for decision in DECISIONS} for label in LABELS}
    timings = defaultdict(lambda: dict(analysis_ms=[], load_ms=[]))
    precise = 0
    for row in rows:
        counts[row["label"]][row["decision"]] += 1
        precise += row["precise_elapsed_us"] != ""
        if row["width"] and row["height"]:
            for field, values in timings[f"{row['width']}x{row['height']}"].items():
                values.append(float(row[field]))
    total = len(rows)
    automatic = sum(c["PASS"] + c["FAIL"] for c in counts.values())
    ko_pass, ok_fail = counts["KO"]["PASS"], counts["OK"]["FAIL"]
    return dict(
        confusion=counts,
        ko_leak=ratio(ko_pass, sum(counts["KO"].values())),
        ok_reject=ratio(ok_fail, sum(counts["OK"].values())),
        automatic=ratio(automatic, total),
        automatic_error=ratio(ko_pass + ok_fail, automatic),
        review=ratio(sum(c["REVIEW"] for c in counts.values()), total),
        error=ratio(sum(c["ERROR"] for c in counts.values()), total),
        precise=ratio(precise, total),
        timing_by_size={size: {"n": len(values["analysis_ms"]),
            **{field: {"p50": nearest_rank(times, 50), "p95": nearest_rank(times, 95)}
               for field, times in values.items()}} for size, values in sorted(timings.items())},
    )


def run_inspection(inspect, mode, configuration, manifest, output, split, freeze=None):
    output.parent.mkdir(parents=True, exist_ok=True)
    args = [str(inspect), "--algorithm", mode, "--config", str(configuration), "--manifest", str(manifest),
            "--output", str(output), "--evaluation-split", split]
    if freeze:
        args.extend(("--freeze", str(freeze)))
    result = subprocess.run(args, capture_output=True, encoding="utf-8", errors="replace", timeout=900)
    if result.returncode not in (0, 2):
        raise RuntimeError(f"{mode} {split} failed: {result.stderr.strip()}")
    return read_inspection_result(mode, configuration, manifest, output)


def read_inspection_result(mode, configuration, manifest, output, summary=None):
    if summary is None:
        summary = read_json(output / "summary.json")
    if summary.get("input_manifest_valid") != 1 or summary.get("manifest_sha256") != checksum(manifest):
        raise ValueError(f"{mode}: input manifest failed verification")
    if summary.get("config_sha256") != checksum(configuration) or summary.get("threads") != 1:
        raise ValueError(f"{mode}: config or OpenCV thread count changed")
    rows = read_csv(output / "results.csv")
    expected = read_json(manifest)["rows"]
    if len(rows) != len(expected) or any(
            any(actual[a] != source[e] for a, e in (("source_path", "path"), ("split", "split"),
                                                  ("label", "label"), ("image_sha256", "sha256")))
            for actual, source in zip(rows, expected, strict=True)):
        raise ValueError(f"{mode}: incomplete or mismatched result rows")
    return rows


def highest_threshold(rows, ko_cap):
    observations = sorted(float(row["score"]) for row in rows if row["label"] == "KO" and row["score"] != "")
    if len(observations) <= ko_cap:
        return 1.0
    # At equality the single-stage paths classify as FAIL.
    return observations[ko_cap]


def candidate_fields(rows, stage):
    for row in rows:
        score = row[f"{stage}_score"]
        elapsed = row[f"{stage}_elapsed_ns"]
        yield (float(score) if score else -1.0, int(elapsed) if elapsed else 0)


def constraints(metrics, ko_cap):
    return {
        "ko_leak": metrics["ko_leak"]["count"] <= ko_cap,
        "automatic": 100 * metrics["automatic"]["count"] >= MIN_AUTOMATIC_PERCENT * metrics["automatic"]["denominator"],
        "precise": 100 * metrics["precise"]["count"] <= MAX_PRECISE_PERCENT * metrics["precise"]["denominator"],
    }


def candidate_rank(row):
    ok, ko = row["counts"]
    return ok[1], ko[0], row["precise_count"], ok[2] + ko[2], *(row[name] for name in BOUNDARIES)


def policy_config(candidate, contrast_threshold, budget):
    return dict(schema_version=1, contrast_threshold=contrast_threshold, budget_us=budget,
                **{name: candidate[name] for name in BOUNDARIES})


def run_validation_path(inspect, mode, configuration, manifest, output):
    if output.exists():
        if (output / "summary.json").is_file():
            return read_inspection_result(mode, configuration, manifest, output)
        # Earlier runners could leave a partial output directly in this directory.
        output.rename(output.with_name(f"{output.name}-incomplete-{uuid4().hex}"))
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix=f".{output.name}-running-", dir=output.parent) as temporary:
        staging = Path(temporary) / "run"
        rows = run_inspection(inspect, mode, configuration, manifest, staging, "validation")
        staging.rename(output)
        return rows


def run_candidate(inspect, manifest, checked, config):
    setting = checked / "candidate.json"
    if checked.exists():
        if checked.is_symlink() or checked.resolve().parent != checked.parent.resolve():
            raise ValueError(f"Unsafe candidate directory: {checked}")
        try:
            if read_json(setting) != config:
                raise ValueError("Candidate config changed")
            return read_inspection_result("selective", setting, manifest, checked / "run")
        except (AttributeError, KeyError, OSError, TypeError, ValueError, csv.Error):
            # Preserve output from an interrupted pre-staging attempt for diagnosis.
            checked.rename(checked.with_name(f"{checked.name}-incomplete-{uuid4().hex}"))

    checked.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix=f".{checked.name}-running-", dir=checked.parent) as temporary:
        staging = Path(temporary) / "candidate"
        staging.mkdir()
        staging_config = staging / "candidate.json"
        write_json(staging_config, config)
        rows = run_inspection(inspect, "selective", staging_config, manifest, staging / "run", "validation")
        staging.rename(checked)
        return rows


def confirm_ranked(output, inspect, manifest, search, selection, contrast_threshold, metrics):
    validation = output / "validation"
    ko_cap = selection["constraints"]["ko_leak_max_count"]
    budget = selection["budget_us"]
    ranked = sorted((row for row in search["candidates"] if row["eligible"]), key=candidate_rank)
    if not ranked:
        return None
    attempts = list(selection.get("live_candidates", []))
    if selection.get("status") == "accepted":
        return selection
    checks = validation / "rechecks"
    for rank, candidate in enumerate(ranked[len(attempts):], start=len(attempts) + 1):
        checked = checks / f"candidate-{rank:04d}"
        measured = evaluate_metrics(run_candidate(inspect, manifest, checked, policy_config(candidate, contrast_threshold, budget)))
        setting = checked / "candidate.json"
        passed = constraints(measured, ko_cap)
        attempts.append(dict(rank=rank, calibration=candidate, constraints=passed,
                             metrics={name: measured[name] for name in ("ko_leak", "automatic", "precise", "ok_reject", "review", "error")}))
        eligible = all(passed.values())
        print(f"Live candidate {rank}/{len(ranked)}: automatic={measured['automatic']['count']}, "
              f"KO PASS={measured['ko_leak']['count']}, precise={measured['precise']['count']}, accepted={eligible}", flush=True)
        if eligible:
            shutil.copyfile(setting, output / "config" / "selective.json")
            metrics["selective"] = measured
        snapshot = dict(selection, status="accepted" if eligible else "held_live_constraints",
                        calibration=candidate if eligible else selection["calibration"],
                        measured_constraints=passed, live_candidates=attempts, modes=metrics)
        write_checkpoint(validation / "selection.json", snapshot)
        if eligible:
            return snapshot
    return None


def environment(inspect):
    with inspect.open("rb") as stream:
        stream.seek(60)
        stream.seek(int.from_bytes(stream.read(4), "little") + 4)
        machine = hex(int.from_bytes(stream.read(2), "little"))
    return dict(os=platform.platform(), processor=platform.processor(), host_arch=platform.machine(),
                executable_machine=machine, cpu_count=os.cpu_count(), opencv_threads=1)


def verified_inputs(output):
    inputs = output / "inputs"
    report = read_json(inputs / "provenance.json")
    for name in ("validation.json", "test.json", "test-primary.json", "excluded.json"):
        if report["sha256"][name] != checksum(inputs / name):
            raise ValueError(f"Prepared input changed: {name}")
    return inputs, report


def freeze_validation(output, inspect, calibrator, inputs, provenance, runtime, inspect_hash, calibrator_hash):
    validation, configs = output / "validation", output / "config"
    manifest = inputs / "validation.json"
    if checksum(inspect) != inspect_hash or checksum(calibrator) != calibrator_hash:
        raise ValueError("An evaluation executable changed during validation")
    selection_file = validation / "selection.json"
    selection_hash = checksum(selection_file)
    selection = read_json(selection_file)
    if selection["status"] != "accepted":
        raise ValueError("Validation did not approve the frozen configuration")
    ko_cap = selection["constraints"]["ko_leak_max_count"]
    winner = selection["live_candidates"][-1]
    selected_path = validation / "rechecks" / f"candidate-{winner['rank']:04d}" / "run"
    # Rank 1 used the confirmed path before all selective candidates were staged identically.
    if winner["rank"] == 1 and not selected_path.exists():
        selected_path = validation / "confirmed" / "selective"
    mode_summaries = {}
    for mode in MODES:
        run = selected_path if mode == "selective" else validation / "confirmed" / mode
        summary = read_json(run / "summary.json")
        rows = read_inspection_result(mode, configs / f"{mode}.json", manifest, run, summary)
        metrics = evaluate_metrics(rows)
        if metrics != selection["modes"][mode]:
            raise ValueError(f"{mode}: confirmed metrics differ from validation selection")
        if mode == "baseline" and metrics["ko_leak"]["count"] != ko_cap:
            raise ValueError("Confirmed baseline differs from validation KO leak ceiling")
        if mode == "selective" and not all(constraints(metrics, ko_cap).values()):
            raise ValueError("Selective confirmation did not meet validation constraints")
        mode_summaries[mode] = summary
    if checksum(selection_file) != selection_hash:
        raise ValueError("Validation selection changed during freeze")
    freeze = dict(
        schema_version=1, established_utc=datetime.now(timezone.utc).isoformat(),
        executable_sha256=inspect_hash, calibrator_sha256=calibrator_hash,
        test_manifest_sha256=checksum(inputs / "test.json"), test_primary_sha256=checksum(inputs / "test-primary.json"),
        validation_manifest_sha256=checksum(manifest), excluded_sha256=checksum(inputs / "excluded.json"),
        source_archive_sha256=provenance["source_archive_sha256"], provenance_sha256=checksum(inputs / "provenance.json"),
        config_sha256={mode: summary["config_sha256"] for mode, summary in mode_summaries.items()},
        validation_selection_sha256=selection_hash, environment=runtime,
        algorithm_version={mode: summary["algorithm_version"] for mode, summary in mode_summaries.items()},
        metric_denominators=dict(ko_leak="KO", ok_reject="OK", automatic_error="PASS+FAIL",
                                 review="all", error="all", automatic="all", precise="all"),
    )
    write_checkpoint(output / "freeze.json", freeze)
    print("Validation constraints satisfied. Freeze written.")
    return 0


def calibrate_validation(output, inspect, calibrator, baseline, local_contrast, settings, manifest):
    configs, validation = output / "config", output / "validation"
    shutil.copyfile(baseline, configs / "baseline.json")
    observations = {mode: run_validation_path(inspect, mode, baseline if mode == "baseline" else local_contrast,
                                              manifest, validation / "observations" / mode)
                    for mode in MODES[:3]}
    ko_cap = evaluate_metrics(observations["baseline"])["ko_leak"]["count"]
    for mode in ("fast", "precise"):
        write_checkpoint(configs / f"{mode}.json", dict(settings, fail_ratio=highest_threshold(observations[mode], ko_cap)))
    paired = list(zip(candidate_fields(observations["fast"], "fast"),
                      candidate_fields(observations["precise"], "precise"), strict=True))
    times_ns = [fast_ns + precise_ns for ((fast, fast_ns), (precise, precise_ns)) in paired
                if min(fast, precise) >= 0]
    budget = max(1, (nearest_rank(times_ns, 95) + 999) // 1_000)
    payload = [f"nanoseconds {len(paired)} {budget} {ko_cap} {MIN_AUTOMATIC_PERCENT} {MAX_PRECISE_PERCENT}\n"]
    for row, ((fast, fast_ns), (precise, precise_ns)) in zip(observations["baseline"], paired, strict=True):
        payload.append(f"{int(row['label'] == 'KO')} {fast:.17g} {fast_ns} {precise:.17g} {precise_ns}\n")
    result = subprocess.run([str(calibrator)], input="".join(payload), capture_output=True,
                            encoding="utf-8", check=True, timeout=300)
    search = json.loads(result.stdout)
    write_checkpoint(validation / "calibration.json", search)
    margins = search["selected"]
    selection = {"constraints": {"ko_leak_max_count": ko_cap, "automatic_min_percent": MIN_AUTOMATIC_PERCENT,
                                 "precise_max_percent": MAX_PRECISE_PERCENT},
                 "budget_us": budget, "valid_pairs": len(times_ns), "calibration": margins,
                 "config_sha256": {mode: checksum(configs / f"{mode}.json") for mode in MODES[:3]}}
    if not margins["eligible"]:
        write_json(validation / "closest-selective.json", policy_config(margins, settings["contrast_threshold"], budget))
    selection["status"] = "calibrated" if margins["eligible"] else "held_no_eligible_candidate"
    write_checkpoint(validation / "selection.json", selection)
    return search, selection


def validate(output, inspect, calibrator, baseline, local_contrast, resume=False):
    if (output / "freeze.json").exists():
        raise FileExistsError("Validation is already frozen. Use a new evaluation output.")
    inputs, provenance = verified_inputs(output)
    inspect_hash, calibrator_hash = checksum(inspect), checksum(calibrator)
    runtime = environment(inspect)
    if runtime["executable_machine"] != "0x8664":
        raise ValueError("S11 evaluation requires the Release x64 runner")
    manifest = inputs / "validation.json"
    configs, validation = output / "config", output / "validation"
    lock = dict(executable_sha256=inspect_hash, calibrator_sha256=calibrator_hash, environment=runtime,
                manifest_sha256=checksum(manifest), baseline_sha256=checksum(baseline),
                local_contrast_sha256=checksum(local_contrast))
    if resume:
        if read_json(validation / "evaluation.json") != lock:
            raise ValueError("Cannot resume validation with a changed runner, observations, or environment")
    else:
        configs.mkdir()
        validation.mkdir()
        write_checkpoint(validation / "evaluation.json", lock)

    settings = read_json(local_contrast)
    selection_file = validation / "selection.json"
    if selection_file.is_file():
        search = read_json(validation / "calibration.json")
        selection = read_json(selection_file)
    else:
        search, selection = calibrate_validation(output, inspect, calibrator, baseline, local_contrast, settings, manifest)
    if selection["status"] == "held_no_eligible_candidate":
        print("Trial held: no eligible policy on fixed validation observations.")
        return 3
    for mode, expected in selection.get("config_sha256", {}).items():
        if checksum(configs / f"{mode}.json") != expected:
            raise ValueError(f"{mode}: calibrated config changed")
    metrics = selection.get("modes")
    if metrics is None:
        metrics = {mode: evaluate_metrics(run_validation_path(inspect, mode, configs / f"{mode}.json", manifest,
                                                              validation / "confirmed" / mode))
                   for mode in MODES[:3]}
    selected = confirm_ranked(output, inspect, manifest, search, selection, settings["contrast_threshold"], metrics)
    if selected is None:
        print("Trial held: no eligible policy on live validation.")
        return 3
    return freeze_validation(output, inspect, calibrator, inputs, provenance, runtime, inspect_hash, calibrator_hash)


def verify_frozen_evidence(output, inputs, inspect, freeze):
    checks = {
        "executable_sha256": inspect,
        "test_manifest_sha256": inputs / "test.json",
        "test_primary_sha256": inputs / "test-primary.json",
        "validation_manifest_sha256": inputs / "validation.json",
        "excluded_sha256": inputs / "excluded.json",
        "provenance_sha256": inputs / "provenance.json",
        "validation_selection_sha256": output / "validation" / "selection.json",
    }
    if freeze.get("schema_version") != 1 or any(checksum(path) != freeze.get(field) for field, path in checks.items()):
        raise ValueError("Test requires matching frozen evidence")
    if read_json(checks["validation_selection_sha256"]).get("status") != "accepted":
        raise ValueError("Validation did not approve the frozen configuration")
    if any(checksum(output / "config" / f"{mode}.json") != freeze["config_sha256"][mode] for mode in MODES):
        raise ValueError("Frozen config changed")


def verify_freeze(output, inputs, inspect, freeze):
    verify_frozen_evidence(output, inputs, inspect, freeze)
    if environment(inspect) != freeze.get("environment"):
        raise ValueError("Frozen evaluation environment changed")


def evaluate_test(output, inspect, archive_path, test_output=None):
    inputs, report = verified_inputs(output)
    freeze_file = output / "freeze.json"
    if not freeze_file.is_file():
        raise ValueError("Test requires an accepted validation freeze")
    freeze_hash = checksum(freeze_file)
    freeze = read_json(freeze_file)
    verify_freeze(output, inputs, inspect, freeze)
    if freeze["source_archive_sha256"] != report["source_archive_sha256"]:
        raise ValueError("Prepared archive does not match freeze")
    destination = test_output if test_output is not None else output / "test"
    if destination.exists() or destination.is_symlink():
        raise FileExistsError(f"Test output already exists: {destination}. Use a new --test-output.")
    manifest = inputs / "test.json"
    primary = {row["path"] for row in read_json(inputs / "test-primary.json")["rows"]}
    official = read_json(manifest)["rows"]
    test_images = inputs / "images" / "test"
    if not test_images.exists():
        match_archive(archive_path, freeze["source_archive_sha256"])
        with ZipFile(archive_path) as archive:
            materialize(archive, official, test_images)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix=f".{destination.name}-running-", dir=destination.parent) as temporary:
        staging = Path(temporary) / "results"
        results = []
        for index, order in enumerate((MODES, MODES[::-1]), start=1):
            folder = staging / f"run-{index:02d}"
            modes = {mode: run_inspection(inspect, mode, output / "config" / f"{mode}.json",
                                         manifest, folder / mode, "test", freeze_file) for mode in order}
            results.append(modes)
        if checksum(freeze_file) != freeze_hash:
            raise ValueError("Freeze changed during test")
        write_json(staging / "report.json", dict(freeze_sha256=freeze_hash, cohorts=test_comparisons(results, primary)))
        staging.rename(destination)
    print(f"Completed four paths across two frozen test repetitions in {destination}.")


def test_comparisons(results, primary):
    comparisons = {}
    for cohort in ("primary", "official"):
        choose = (lambda row: row["source_path"] in primary) if cohort == "primary" else (lambda row: True)
        repeat = [{mode: evaluate_metrics([row for row in run[mode] if choose(row)]) for mode in MODES} for run in results]
        changes = {}
        for mode in MODES:
            counts = Counter((a["decision"], b["decision"], a["reason"], b["reason"])
                             for a, b in zip(results[0][mode], results[1][mode], strict=True)
                             if choose(a) and a["decision"] != b["decision"])
            changes[mode] = [{"from": old, "to": new, "first_reason": why_old, "second_reason": why_new, "n": n}
                             for (old, new, why_old, why_new), n in sorted(counts.items())]
        comparisons[cohort] = dict(repeat=repeat, repeated_decision_changes=changes)
    return comparisons


def compare_demo(output, inspect, train):
    """Compare one train image after the same eight warm-up rows in each path."""
    inputs, _ = verified_inputs(output)
    freeze = read_json(output / "freeze.json")
    verify_freeze(output, inputs, inspect, freeze)
    reference = read_json(train / "manifest.json")
    preceding = sorted((row for row in reference["rows"] if row["path"] < DEMO_IMAGE), key=lambda row: row["path"])
    sample = next(row for row in reference["rows"] if row["path"] == DEMO_IMAGE)
    if len(preceding) < 8 or reference["root"] != "images":
        raise ValueError("Demo requires the prepared S04 train inputs")
    rows = [dict(row, debug=int(row is sample)) for row in (*preceding[-8:], sample)]
    demo = output / "train-demo-warm"
    demo.mkdir()
    for row in rows:
        path = row["path"]
        image_path(path, "train", row["label"])
        source = train / "images" / path
        if checksum(source) != row["sha256"] or row["split"] != "train":
            raise ValueError(f"S04 train image changed: {path}")
        target = demo / "images" / path
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
    manifest = demo / "manifest.json"
    write_json(manifest, dict(schema_version=1, root="images", rows=rows))
    results = {}
    for mode in MODES:
        run = demo / mode
        subprocess.run([str(inspect), "--algorithm", mode, "--config", str(output / "config" / f"{mode}.json"),
                        "--manifest", str(manifest), "--output", str(run)], check=True, timeout=60,
                        capture_output=True)
        row = read_csv(run / "results.csv")[-1]
        if row["source_path"] != DEMO_IMAGE or not row["artifact_path"]:
            raise ValueError("Missing target image evidence")
        results[mode] = row
    write_json(demo / "comparison.json", dict(freeze_sha256=checksum(output / "freeze.json"), input=DEMO_IMAGE,
                                              warm_up_rows=[row["path"] for row in rows[:-1]], modes=results))
    print(f"Four-mode train demo written to {demo}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stage", choices=("prepare", "validate", "test", "demo"))
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--archive", type=Path, default=Path("data/openzdm/Weld Aesthetical Defects.zip"))
    parser.add_argument("--evidence", type=Path, default=Path("docs/steps/S02-evidence"))
    parser.add_argument("--inspect", type=Path)
    parser.add_argument("--calibrator", type=Path)
    parser.add_argument("--resume-validation", action="store_true")
    parser.add_argument("--test-output", type=Path, help="New directory for test results (default: OUTPUT/test)")
    parser.add_argument("--baseline", type=Path, default=Path("config/baseline.json"))
    parser.add_argument("--local-contrast", type=Path, default=Path("config/local-contrast.json"))
    parser.add_argument("--train-inputs", type=Path, default=Path("artifacts/S04/inputs"))
    args = parser.parse_args()
    if args.stage == "prepare":
        prepare(args.output, args.evidence, args.archive)
    elif args.stage == "validate":
        if not args.inspect or not args.calibrator:
            parser.error("validate requires --inspect and --calibrator")
        return validate(args.output, args.inspect.resolve(strict=True), args.calibrator.resolve(strict=True),
                        args.baseline, args.local_contrast, args.resume_validation)
    elif args.stage == "test":
        if not args.inspect:
            parser.error("test requires --inspect")
        evaluate_test(args.output, args.inspect.resolve(strict=True), args.archive, args.test_output)
    else:
        if not args.inspect:
            parser.error("demo requires --inspect")
        compare_demo(args.output, args.inspect.resolve(strict=True), args.train_inputs)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
