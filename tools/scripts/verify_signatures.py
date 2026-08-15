"""Read-only signature verifier for the pinned Armada II GOG build."""
from __future__ import annotations
import argparse, hashlib, json
from pathlib import Path
import pefile

ap = argparse.ArgumentParser()
ap.add_argument("--executable", required=True)
ap.add_argument("--signatures", required=True)
args = ap.parse_args()

binary = Path(args.executable)
catalog = json.loads(Path(args.signatures).read_text(encoding="utf-8"))
actual = hashlib.sha256(binary.read_bytes()).hexdigest()
expected = catalog["binary"]["sha256"]
if actual.lower() != expected.lower():
    raise SystemExit(f"SHA-256 mismatch: {actual}")
pe = pefile.PE(str(binary), fast_load=True)
for entry in catalog["signatures"]:
    pattern = bytes.fromhex(entry["pattern"])
    found = pe.get_data(int(entry["rva"], 16), len(pattern))
    if found != pattern:
        raise SystemExit(f"signature mismatch: {entry['name']} at {entry['rva']}")
print(f"verified {len(catalog['signatures'])} signatures for {binary.name}")

