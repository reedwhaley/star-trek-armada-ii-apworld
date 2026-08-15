"""Metadata-only snapshots of likely mutable Armada II state files."""
from __future__ import annotations
import argparse, hashlib, json
from datetime import datetime, timezone
from pathlib import Path

EXTENSIONS = {".set", ".cfg", ".sav", ".dat", ".ini", ".prf", ".profile"}
HINTS = ("shell", "save", "profile", "campaign", "armada")
SKIP_DIRS = {"bzn", "sounds", "textures", "sprites", "sod", "animations", "bitmaps"}

def digest(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--game-root", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()
    root, output = Path(args.game_root).resolve(), Path(args.output)
    entries = []
    for path in root.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in EXTENSIONS:
            continue
        rel = path.relative_to(root)
        if any(part.lower() in SKIP_DIRS for part in rel.parts):
            continue
        if not (any(hint in path.name.lower() for hint in HINTS) or rel.parts[:1] == ("save",)):
            continue
        stat = path.stat()
        item = {"path": str(path), "relative_path": str(rel), "size": stat.st_size,
                "mtime_ns": stat.st_mtime_ns, "sha256": digest(path)}
        if stat.st_size <= 65536:
            item["content_hex"] = path.read_bytes().hex()
        entries.append(item)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps({"game_root": str(root), "captured_utc": datetime.now(timezone.utc).isoformat(),
                                  "files": sorted(entries, key=lambda row: row["path"].lower())}, indent=2) + "\n",
                      encoding="utf-8")
    print(f"snapshotted {len(entries)} mutable-state candidates")

if __name__ == "__main__":
    main()

