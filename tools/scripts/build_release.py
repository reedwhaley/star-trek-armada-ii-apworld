"""Build the standalone Armada II Archipelago release archive and checksums."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import tempfile
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


ROOT = Path(__file__).resolve().parents[2]
VERSION = "0.1.0"
FILES = {
    ROOT / "out" / "star_trek_armada_ii.apworld": "star_trek_armada_ii.apworld",
    # This workspace uses the standalone x86 observer build tree because the
    # historical VS CMake cache is not portable between local installations.
    ROOT / "build-observer-nebula" / "armada2_observer.dll": "armada2_observer.dll",
    ROOT / "build-injector" / "Release" / "armada2_injector.exe": "armada2_injector.exe",
    ROOT / "release" / "README.md": "README.md",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, default=ROOT / "out" / f"StarTrekArmadaII-AP-{VERSION}.zip")
    args = parser.parse_args()
    missing = [path for path in FILES if not path.is_file()]
    if missing:
        raise SystemExit("missing release input(s): " + ", ".join(str(path) for path in missing))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temporary:
        stage = Path(temporary)
        manifest: list[str] = []
        for source, name in FILES.items():
            target = stage / name
            shutil.copy2(source, target)
            manifest.append(f"{sha256(target)}  {name}")
        (stage / "SHA256SUMS.txt").write_text("\n".join(manifest) + "\n", encoding="utf-8")
        with ZipFile(args.output, "w", ZIP_DEFLATED) as archive:
            for path in sorted(stage.iterdir()):
                archive.write(path, path.name)
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
