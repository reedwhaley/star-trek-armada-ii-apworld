"""Package the local Star Trek: Armada II custom world as a .apworld archive."""

from __future__ import annotations

import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path("apworld/star_trek_armada_ii"))
    parser.add_argument("--output", type=Path, default=Path("out/star_trek_armada_ii.apworld"))
    args = parser.parse_args()
    source = args.source.resolve()
    if not (source / "archipelago.json").is_file():
        raise SystemExit(f"not an apworld source directory: {source}")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(args.output, "w", ZIP_DEFLATED) as archive:
        for path in sorted(source.rglob("*")):
            if path.is_file() and "__pycache__" not in path.parts:
                archive.write(path, path.relative_to(source.parent).as_posix())
    print(args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
