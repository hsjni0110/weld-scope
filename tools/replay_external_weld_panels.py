"""Replay Zender et al. Figure 2c-e as published external examples with the frozen OPENZDM CLI."""
import argparse
import csv
import hashlib
from pathlib import Path
from tempfile import TemporaryDirectory
from urllib.request import urlopen

from PIL import Image, ImageChops

from tools.audit_openzdm_validity import EVIDENCE, MODEL, PACKAGE, RUNNER, STUDY
from tools.evaluate_openzdm_patterns import manifest
from tools.evaluate_openzdm_policy import read_json, run, sha, verify_live, write_json

DEFAULT_OUTPUT = STUDY / "external-review"
FILENAME = "sensors-25-07301-g002.png"
SOURCE_URL = "https://mdpi-res.com/sensors/sensors-25-07301/article_deploy/html/images/sensors-25-07301-g002.png"
SOURCE_HASH = "342ce679c4617774f493288cc0f9e40c6893646a7a070d0444891e2e32d425ba"
PANELS = [
    ("c", 887, "OK", "weld with soot; labeled acceptable by authors"),
    ("d", 1760, "KO", "missing weld"),
    ("e", 2634, "KO", "long hole"),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--figure", type=Path, help="Previously downloaded original Figure 2 PNG")
    parser.add_argument("--download-figure", action="store_true", help="Download the pinned public figure if absent")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(exist_ok=True, parents=True)
    source = args.figure or output / FILENAME
    if not source.is_file() and args.download_figure:
        if args.figure:
            parser.error("Use --figure for an existing file")
        with urlopen(SOURCE_URL, timeout=30) as response:
            data = response.read(2_000_001)
        if hashlib.sha256(data).hexdigest() != SOURCE_HASH:
            raise ValueError("Publisher figure checksum mismatch")
        source.write_bytes(data)
    if not source.is_file():
        parser.error("Provide --figure PATH or --download-figure")
    if sha(source) != SOURCE_HASH:
        raise ValueError("Publisher figure checksum mismatch")
    frozen = read_json(EVIDENCE / "openzdm-eda-spatial-policy.json")["package"]
    if sha(RUNNER) != frozen["runner"] or any(sha(PACKAGE / name) != value
                                               for name, value in frozen["files"].items()):
        raise ValueError("Frozen package or runner checksum mismatch")

    rows = []
    with Image.open(source) as figure:
        if figure.size != (3307, 1191):
            raise ValueError("Unexpected publisher rendering size")
        for panel, left, label, description in PANELS:
            box = (left, 401, left + 656, 1057)
            patch = figure.crop(box).convert("RGB")
            red, green, blue = patch.split()
            if ImageChops.difference(red, green).getbbox() or ImageChops.difference(green, blue).getbbox():
                raise ValueError("Unexpected color markup")
            filename = f"panel-{panel}.png"
            patch.resize((128, 128), Image.Resampling.BOX).save(output / filename)
            rows.append(dict(path=filename, split="external_illustration", label=label,
                             sha256=sha(output / filename), source_panel=panel, box_xyxy=box,
                             physical_part_id=None, acquisition_batch_id=None, label_detail=description))
    write_json(output / "source.json", {
        "figure": SOURCE_URL, "figure_sha256": SOURCE_HASH,
        "figure_page": "https://pmc.ncbi.nlm.nih.gov/articles/PMC12693759/figure/sensors-25-07301-f002/",
        "article": "https://doi.org/10.3390/s25237301",
        "title": "Synthetic Data Generation for AI-Based Quality Inspection of Laser Welds in Lithium-Ion Batteries",
        "creators": ["Jonathan Zender", "Stefan Maier", "Alois Herkommer", "Michael Layh"],
        "article_figure_license": "https://creativecommons.org/licenses/by/4.0/",
        "underlying_raw_dataset": "Permission from Swoboda Wiggensbach KG required; not used.",
        "transformation": "Exact square photographic panels cropped from publisher PNG, identical RGB channels as required by the C++ precise path, area (BOX) downsampling from 656x656 to the authors' stated 128x128 input. Figure-rendered pixels, not raw.",
        "exclusions": {"a": "no OK/NOK label in caption", "b": "red overlaid scale bar and text"},
        "scope": "Author-selected, horizontal, unwrapped seam patches; different field of view from OPENZDM strips. No rates, group independence, or production decision.",
        "rows": rows, "frozen_manifest": "docs/algorithms/evidence/openzdm-eda-spatial-policy.json",
        "runner_sha256": frozen["runner"], "model_manifest_sha256": sha(EVIDENCE / "openzdm-eda-spatial-policy.json"),
    })
    score_control = []
    previous = None
    for order_name, ordered in [("forward", rows), ("reverse", list(reversed(rows)))]:
        input_file = output / f"{order_name}-manifest.json"
        manifest(ordered, output, input_file)
        with TemporaryDirectory(dir=output) as temporary:
            trace = Path(temporary) / f"{order_name}.csv"
            observations = run(RUNNER, "whole", input_file, MODEL, trace)
            verify_live(observations, read_json(PACKAGE / "outside.json"))
            trace.replace(output / trace.name)
        scores = {r["path"]: r["fast_score"] for r in observations}
        if previous is not None and scores != previous:
            raise ValueError("Fast score changed across order")
        previous = scores
        score_control.append({r["path"]: (r["label"], r["fast_score"], r["decision"], r["reason"])
                              for r in observations})
    with (output / "panel-results.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(["panel", "publisher_label", "fast_score", "forward_decision", "forward_reason",
                         "reverse_decision", "reverse_reason"])
        for row in rows:
            label, score, decision, reason = score_control[0][row["path"]]
            writer.writerow([row["source_panel"], label, score, decision, reason,
                             *score_control[1][row["path"]][2:]])


if __name__ == "__main__":
    main()
