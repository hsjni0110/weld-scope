"""Copy two approved S02 train members verbatim into the S03 app resources."""
import csv
import hashlib
import json
from pathlib import Path
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
target = ROOT / "src/WeldScope.App/res/samples"
target.mkdir(parents=True, exist_ok=True)
with (ROOT / "docs/steps/S02-evidence/manifest.csv").open(encoding="utf-8-sig", newline="") as stream:
    manifest = {row["path"]: row for row in csv.DictReader(stream)}
records = []
with ZipFile(ROOT / "data/openzdm/Weld Aesthetical Defects.zip") as archive:
    for member, name in [("train/OK/image_1.png", "normal.png"), ("train/KO/image_1.png", "defect.png")]:
        content = archive.read(member)
        digest = hashlib.sha256(content).hexdigest()
        if digest != manifest[member]["file_sha256"]:
            raise SystemExit(f"S02 manifest checksum mismatch: {member}")
        destination = target / name
        if destination.exists() and destination.read_bytes() != content:
            raise SystemExit(f"Refusing to replace different bytes: {destination}")
        destination.write_bytes(content)
        records.append({"resource": name, "source_member": member, "sha256": digest,
                        "width": int(manifest[member]["width"]), "height": int(manifest[member]["height"])})
(target / "corrupt.png").write_bytes(b"S03 synthetic invalid PNG fixture\n")
(target / "provenance.json").write_text(json.dumps({
    "author": "OPENZDM", "title": "2D image dataset for the presence or absence of aesthetical defects on EV battery welds.",
    "source": "https://zenodo.org/records/14025630", "doi": "10.5281/zenodo.14025630",
    "license": "https://creativecommons.org/licenses/by/4.0/", "changes": "Original PNG bytes unchanged. Display scales proportionally. Candidate rectangle is a synthetic UI overlay.",
    "images": records, "synthetic": ["corrupt.png"],
    "review_fixture": "defect.png reused with a predefined review verdict; no review ground truth claim"
}, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
print(json.dumps(records, indent=2))
