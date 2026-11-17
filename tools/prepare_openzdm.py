"""Reproducible OPENZDM inventory; no inspection algorithm or split changes."""

import argparse
from collections import Counter, defaultdict
import csv
from datetime import datetime, timezone
import hashlib
import io
import json
import os
from pathlib import Path, PurePosixPath
import platform
import stat
import sys
import time
import urllib.request
from zipfile import BadZipFile, ZipFile

import PIL
from PIL import Image, ImageDraw, ImageFont, ImageOps


SOURCE_URL = "https://zenodo.org/records/14025630"
API_URL = "https://zenodo.org/api/records/14025630"
ARCHIVE_NAME = "Weld Aesthetical Defects.zip"
SOURCE_TITLE = "2D image dataset for the presence or absence of aesthetical defects on EV battery welds."
ARCHIVE_SIZE = 1153183844
ARCHIVE_MD5 = "f187dac063cf656bb627327fa9771d16"
EXPECTED_COUNTS = {"train": 43169, "validation": 9250, "test": 9251}
LABELS = ("OK", "KO")
FIELDS = ("path", "split", "label", "bytes", "width", "height", "mode",
          "file_sha256", "pixel_sha256", "dhash", "status", "error")


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def validate_members(members):
    """Reject unsafe ZIP names even though auditing does not extract files."""
    seen = set()
    total_size = 0
    reserved = {"CON", "PRN", "AUX", "NUL"} | {f"{prefix}{number}" for prefix in ("COM", "LPT") for number in range(1, 10)}
    for member in members:
        name = member.orig_filename
        parts = name.rstrip("/").split("/")
        key = name.rstrip("/").casefold()
        has_unsafe_parts = any(
            part in ("", ".", "..") or part.endswith((" ", "."))
            or part.split(".")[0].upper() in reserved
            or any(ord(char) < 32 for char in part)
            for part in parts
        )
        has_unsafe_name = not name or name.startswith("/") or "\\" in name or ":" in name
        is_symbolic_link = stat.S_ISLNK(member.external_attr >> 16)
        if has_unsafe_name or has_unsafe_parts or key in seen or is_symbolic_link:
            raise ValueError(f"Unsafe or colliding ZIP path: {name!r}")
        if member.flag_bits & 1 or member.file_size > 16 * 1024 * 1024:
            raise ValueError(f"Encrypted or oversized ZIP entry: {name!r}")
        seen.add(key)
        total_size += member.file_size
        if total_size > 8 * 1024 ** 3 or len(seen) > 100000:
            raise ValueError("ZIP exceeds OPENZDM inventory limits")


def verify_archive(path, expected_size=ARCHIVE_SIZE, expected_md5=ARCHIVE_MD5):
    if path.stat().st_size != expected_size:
        raise ValueError("Archive size mismatch")
    md5, sha256 = hashlib.md5(), hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            md5.update(chunk)
            sha256.update(chunk)
    if md5.hexdigest() != expected_md5:
        raise ValueError("Archive MD5 mismatch")
    return sha256.hexdigest()


def download_archive(data_dir: Path) -> tuple[Path, str]:
    """Return the official ZIP path and its verified SHA-256."""
    data_dir.mkdir(parents=True, exist_ok=True)
    target = data_dir / ARCHIVE_NAME
    if target.exists():
        return target, verify_archive(target)
    with urllib.request.urlopen(API_URL, timeout=60) as response:
        metadata = json.load(response)
    if metadata["metadata"]["license"]["id"] != "cc-by-4.0":
        raise ValueError("Source license changed; review before downloading")
    entries = [item for item in metadata["files"] if item["key"] == ARCHIVE_NAME]
    if len(entries) != 1 or entries[0]["size"] != ARCHIVE_SIZE or entries[0]["checksum"] != "md5:" + ARCHIVE_MD5:
        raise ValueError("Source archive changed; review before downloading")
    metadata_path = data_dir / "source-metadata.json"
    if not metadata_path.exists():
        write_json(metadata_path, metadata)
    sha256 = download_verified(entries[0]["links"]["self"], target, ARCHIVE_SIZE, ARCHIVE_MD5)
    return target, sha256


def download_verified(url: str, target: Path, expected_size: int, expected_md5: str) -> str:
    """Resume bounded HTTP ranges, preserving partial bytes on transfer failure."""
    if target.exists():
        return verify_archive(target, expected_size, expected_md5)
    partial = target.with_suffix(target.suffix + ".part")
    offset = partial.stat().st_size if partial.exists() else 0
    if offset > expected_size:
        raise ValueError("Partial archive is larger than the expected source")
    while offset < expected_size:
        end = min(offset + 16 * 1024 * 1024, expected_size) - 1
        request = urllib.request.Request(url, headers={"Range": f"bytes={offset}-{end}"})
        with urllib.request.urlopen(request, timeout=30) as response:
            if response.status != 206 or response.headers.get("Content-Range") != f"bytes {offset}-{end}/{expected_size}":
                raise ValueError("Server did not honor the requested byte range")
            with partial.open("ab" if partial.exists() else "xb") as stream:
                while offset <= end:
                    chunk = response.read(min(1024 * 1024, end + 1 - offset))
                    if not chunk:
                        raise OSError("Incomplete archive range; rerun to resume")
                    stream.write(chunk)
                    offset += len(chunk)
        print(f"Downloaded {offset}/{expected_size} bytes", flush=True)
    sha256 = verify_archive(partial, expected_size, expected_md5)
    partial.rename(target)
    return sha256


def difference_hash(image):
    pixels = image.convert("L").resize((9, 8), Image.Resampling.LANCZOS).tobytes()
    result = 0
    for row in range(8):
        for column in range(8):
            result = (result << 1) | (pixels[row * 9 + column] > pixels[row * 9 + column + 1])
    return result


def find_similar_pairs(hashes, max_distance=5):
    """Exact radius search via the pigeonhole principle, not all-pairs scanning."""
    if not 0 <= max_distance <= 5:
        raise ValueError("Supported Hamming radius: 0..5")
    blocks = ((0, 11), (11, 11), (22, 11), (33, 11), (44, 10), (54, 10))
    index = defaultdict(list)
    for right, value in enumerate(hashes):
        keys = [(offset, (value >> offset) & ((1 << width) - 1)) for offset, width in blocks]
        candidates = set()
        for key in keys:
            candidates.update(index[key])
        for left in sorted(candidates):
            distance = (value ^ hashes[left]).bit_count()
            if distance <= max_distance:
                yield left, right, distance
        for key in keys:
            index[key].append(right)


def inspect_member(archive, member):
    row = dict.fromkeys(FIELDS, "")
    row.update(path=member.filename, bytes=member.file_size, status="invalid")
    parts = PurePosixPath(member.filename).parts
    split_names = {"train": "train", "val": "validation", "validation": "validation", "test": "test"}
    if len(parts) == 3 and parts[0] in split_names and parts[1] in LABELS:
        row.update(split=split_names[parts[0]], label=parts[1])
    try:
        content = archive.read(member)
        row["file_sha256"] = hashlib.sha256(content).hexdigest()
        if not row["split"]:
            raise ValueError("Unknown split/label/path structure")
        with Image.open(io.BytesIO(content)) as image:
            row.update(width=image.width, height=image.height, mode=image.mode)
            if image.width * image.height > 16 * 1024 * 1024:
                raise ValueError("Decoded image exceeds 16 megapixels")
            if getattr(image, "n_frames", 1) != 1:
                raise ValueError("Multi-frame image unsupported")
            image.verify()
        with Image.open(io.BytesIO(content)) as image:
            image.load()
            pixel_header = f"{image.width}x{image.height}:RGBA\0".encode("ascii")
            row["pixel_sha256"] = hashlib.sha256(pixel_header + image.convert("RGBA").tobytes()).hexdigest()
            row["dhash"] = f"{difference_hash(image):016x}"
        row["status"] = "valid"
    except (OSError, ValueError, SyntaxError, BadZipFile, Image.DecompressionBombError, Image.DecompressionBombWarning) as error:
        row["error"] = f"{type(error).__name__}: {error}"
    return row


def duplicate_groups(rows, field):
    groups = defaultdict(list)
    for row in rows:
        if row[field]:
            groups[row[field]].append(row)
    duplicates = []
    for key, group in sorted(groups.items()):
        if len(group) < 2:
            continue
        duplicates.append({
            "hash": key,
            "paths": [row["path"] for row in group],
            "splits": sorted({row["split"] for row in group if row["split"]}),
            "labels": sorted({row["label"] for row in group if row["label"]}),
        })
    return duplicates


def write_similarity(rows, output_dir):
    groups = defaultdict(list)
    for row in rows:
        if row["status"] == "valid":
            groups[row["dhash"]].append(row)
    keys = sorted(groups)
    write_json(output_dir / "dhash_groups.json", {key: [row["path"] for row in groups[key]] for key in keys})
    split_counts = {key: Counter(row["split"] for row in group) for key, group in groups.items()}
    same_hash_pairs = 0
    same_hash_cross_split = 0
    for key, group in groups.items():
        group_pairs = len(group) * (len(group) - 1) // 2
        same_split_pairs = sum(count * (count - 1) // 2 for count in split_counts[key].values())
        same_hash_pairs += group_pairs
        same_hash_cross_split += group_pairs - same_split_pairs
    image_pairs, cross_split_pairs, hash_pairs = same_hash_pairs, same_hash_cross_split, 0
    with (output_dir / "similar_hash_pairs.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(("left_dhash", "right_dhash", "distance", "image_pair_count", "cross_split_pair_count"))
        for left, right, distance in find_similar_pairs([int(key, 16) for key in keys]):
            left_counts = split_counts[keys[left]]
            right_counts = split_counts[keys[right]]
            count = len(groups[keys[left]]) * len(groups[keys[right]])
            cross_count = count - sum(number * right_counts[split] for split, number in left_counts.items())
            writer.writerow((keys[left], keys[right], distance, count, cross_count))
            image_pairs += count
            cross_split_pairs += cross_count
            hash_pairs += 1
    return {"method": "64-bit dHash (LANCZOS 9x8), Hamming <= 5; exhaustive six-block index",
            "scope": "all valid images, including exact duplicates; candidate similarity only",
            "unique_hash_count": len(keys), "similar_distinct_hash_pairs": hash_pairs,
            "same_hash_image_pairs": same_hash_pairs, "candidate_image_pairs": image_pairs,
            "cross_split_candidate_pairs": cross_split_pairs,
            "storage": "same-hash pairs implicit in dhash_groups.json; other pairs in similar_hash_pairs.csv"}


def load_font(size):
    candidates = [Path(os.environ.get("WINDIR", "C:/Windows")) / "Fonts/malgun.ttf",
                  Path("/System/Library/Fonts/AppleSDGothicNeo.ttc")]
    for path in candidates:
        if path.exists():
            return ImageFont.truetype(str(path), size)
    raise ValueError("Korean font unavailable (Malgun Gothic / Apple SD Gothic Neo)")


def write_contact_sheet(archive, rows, output_dir):
    selected = {label: [] for label in LABELS}
    for row in rows:
        if row["status"] != "valid" or row["split"] != "train":
            continue
        samples = selected[row["label"]]
        if len(samples) < 8:
            samples.append(row)
    sample_count = max(1, len(selected["OK"]), len(selected["KO"]))
    height = 240 + sample_count * 98
    sheet = Image.new("RGB", (1100, height), "white")
    draw = ImageDraw.Draw(sheet)
    title, heading, body, small = (load_font(size) for size in (28, 22, 18, 15))
    draw.text((30, 20), "용접 외관 샘플", fill="#171717", font=title)
    for column, (label, caption) in enumerate((("OK", "정상 OK"), ("KO", "불량 KO"))):
        x = 30 + 550 * column
        draw.text((x, 70), caption, fill="#171717", font=heading)
        for index, row in enumerate(selected[label]):
            y = 112 + index * 98
            draw.text((x, y), PurePosixPath(row["path"]).name, fill="#171717", font=body)
            with archive.open(row["path"]) as stream, Image.open(stream) as image:
                preview = ImageOps.contain(image.convert("RGB"), (490, 48), Image.Resampling.NEAREST)
                sheet.paste(preview, (x, y + 30))
    y = height - 98
    draw.text((30, y), "OPENZDM | train 샘플 | 원본 라벨 | 축소 및 배치", fill="#333333", font=small)
    draw.text((30, y + 23), SOURCE_TITLE, fill="#333333", font=small)
    draw.text((30, y + 46), SOURCE_URL + " | CC BY 4.0", fill="#333333", font=small)
    draw.text((30, y + 69), "https://creativecommons.org/licenses/by/4.0/", fill="#333333", font=small)
    sheet.save(output_dir / "contact_sheet.png")
    selection = [row["path"] for label in LABELS for row in selected[label]]
    write_json(output_dir / "contact_selection.json", selection)


def create_contact_sheet(archive_path, output_dir):
    """Render from a completed audit without decoding the entire dataset again."""
    if not (output_dir / "summary.json").is_file():
        raise ValueError("Complete audit required before creating the contact sheet")
    for filename in ("contact_sheet.png", "contact_selection.json"):
        if (output_dir / filename).exists():
            raise FileExistsError(f"Contact output already exists: {output_dir / filename}")
    with (output_dir / "manifest.csv").open(encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    with ZipFile(archive_path) as archive:
        write_contact_sheet(archive, rows, output_dir)


def audit_archive(archive_path, output_dir):
    started = time.monotonic()
    if output_dir.exists() and any(output_dir.iterdir()):
        raise FileExistsError(f"Output directory must be empty: {output_dir}")
    with ZipFile(archive_path) as archive:
        validate_members(archive.infolist())
        output_dir.mkdir(parents=True, exist_ok=True)
        members = sorted((member for member in archive.infolist() if not member.is_dir()), key=lambda member: member.filename)
        rows = []
        for index, member in enumerate(members, 1):
            rows.append(inspect_member(archive, member))
            if index % 10000 == 0:
                print(f"Decoded {index}/{len(members)}", flush=True)
        with (output_dir / "manifest.csv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=FIELDS, lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)
        file_groups = duplicate_groups(rows, "file_sha256")
        pixel_groups = duplicate_groups(rows, "pixel_sha256")
        write_json(output_dir / "exact_duplicates.json", {"file_sha256": file_groups, "pixel_sha256": pixel_groups})
        similarity = write_similarity(rows, output_dir)
    summary = summarize_rows(rows, file_groups, pixel_groups, similarity)
    summary.update(
        source=SOURCE_URL,
        license="CC BY 4.0",
        archive_name=archive_path.name,
        environment={"python": platform.python_version(), "pillow": PIL.__version__,
                     "platform": platform.platform(), "machine": platform.machine()},
        generated_utc=datetime.now(timezone.utc).isoformat(),
        duration_seconds=round(time.monotonic() - started, 3),
    )
    write_json(output_dir / "summary.json", summary)
    return summary


def summarize_rows(rows, file_groups, pixel_groups, similarity):
    """Build report values and hold reasons without file or rendering dependencies."""
    valid = [row for row in rows if row["status"] == "valid"]
    counts = Counter(row["split"] for row in valid)
    invalid = [row for row in rows if row["status"] != "valid"]
    cross_groups = sum(len(group["splits"]) > 1 for group in pixel_groups)
    conflict_groups = sum(len(group["labels"]) > 1 for group in pixel_groups)
    description_mismatches = sum((row["width"], row["height"], row["mode"]) != (409, 40, "L") for row in valid)
    split_label_counts = Counter()
    image_properties = Counter()
    for row in valid:
        split_label_counts[f"{row['split']}/{row['label']}"] += 1
        image_properties[f"{row['width']}x{row['height']}/{row['mode']}"] += 1
    hold_reasons = []
    if invalid:
        hold_reasons.append("Invalid input files")
    if dict(counts) != EXPECTED_COUNTS:
        hold_reasons.append("Official counts differ")
    if description_mismatches:
        hold_reasons.append("Native dimensions/modes differ from the official description")
    if cross_groups:
        hold_reasons.append("Exact pixel duplicates across splits")
    if conflict_groups:
        hold_reasons.append("Conflicting labels")
    if similarity["cross_split_candidate_pairs"] > 0:
        hold_reasons.append("Cross-split similar candidates need review")
    hold_reasons.append("Product group IDs and independence unverified; user acceptance required")
    return {
        "file_count": len(rows), "valid_image_count": len(valid), "invalid_file_count": len(invalid),
        "split_counts": dict(sorted(counts.items())), "expected_split_counts": EXPECTED_COUNTS,
        "split_label_counts": dict(sorted(split_label_counts.items())),
        "image_properties": dict(sorted(image_properties.items())),
        "description_mismatch_count": description_mismatches,
        "duplicate_file_groups": len(file_groups), "duplicate_pixel_groups": len(pixel_groups),
        "cross_split_pixel_groups": cross_groups, "conflicting_pixel_groups": conflict_groups,
        "similarity": similarity, "invalid_files": [{"path": row["path"], "error": row["error"]} for row in invalid],
        "evaluation_ready": False,
        "evaluation_hold_reasons": hold_reasons,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archive", type=Path, help="Existing official ZIP (otherwise download)")
    parser.add_argument("--data-dir", type=Path, default=Path("data/openzdm"))
    parser.add_argument("--output", type=Path, default=Path("artifacts/S02"))
    parser.add_argument("--contact-sheet-only", action="store_true", help="Create the contact sheet from an existing audit")
    arguments = parser.parse_args()
    try:
        if arguments.archive:
            archive = arguments.archive
            sha256 = verify_archive(archive)
        else:
            archive, sha256 = download_archive(arguments.data_dir)
        if arguments.contact_sheet_only:
            create_contact_sheet(archive, arguments.output)
            return 0
        summary = audit_archive(archive, arguments.output)
        write_json(arguments.output / "archive-checksum.json", {"name": ARCHIVE_NAME, "bytes": ARCHIVE_SIZE, "md5": ARCHIVE_MD5, "sha256": sha256})
        print(json.dumps({key: summary[key] for key in ("file_count", "valid_image_count", "invalid_file_count", "evaluation_hold_reasons")}, indent=2))
        try:
            create_contact_sheet(archive, arguments.output)
        except (OSError, ValueError, BadZipFile) as error:
            print(f"Audit saved; contact sheet failed: {error}. Retry with --contact-sheet-only and the same --output.", file=sys.stderr)
            return 1
        # A completed audit with evaluation holds is distinct from input/transfer failure.
        return 2 if summary["invalid_file_count"] else 0
    except (OSError, ValueError, BadZipFile, KeyError) as error:
        print(f"Preparation failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
