#!/usr/bin/env python3
"""Generate the static campaign build-check eligibility catalog.

This reads the stock campaign technology trees and ODF inheritance only.  It
does not claim that an object was constructed at runtime: the observer must
still enforce the pinned local queue-to-completion rule before recording a
first-build check.
"""

from __future__ import annotations

import argparse
import json
import re
from collections import Counter
from pathlib import Path


MISSION_TREE = re.compile(r"^a2_(fed|kling|borg)(\d{2})\.tt$", re.IGNORECASE)
INCLUDE = re.compile(r"^\s*#include\s+([\w.-]+\.tt)\s*$", re.IGNORECASE)
TECH_ENTRY = re.compile(r"^\s*([\w.-]+\.odf)\s+(-?\d+)\b", re.IGNORECASE)
ODF_INCLUDE = re.compile(r"^\s*#include\s+[\"<]?([\w.-]+\.odf)[\">]?\s*$", re.IGNORECASE)
ASSIGNMENT = re.compile(r"^\s*([A-Za-z][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$")
FACTIONS = {"fed": "Federation", "kling": "Klingon", "borg": "Borg"}
PREFIXES = {"Federation": "f", "Klingon": "k", "Borg": "b"}


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--techtree", type=Path, required=True)
    parser.add_argument("--odf", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    return parser.parse_args()


def without_comment(line: str) -> str:
    return line.split("//", 1)[0].strip()


def parse_tree(path: Path, seen: set[Path] | None = None) -> dict[str, int]:
    """Resolve ordered includes/overrides; later entries replace earlier ones."""
    seen = set() if seen is None else seen
    resolved = path.resolve()
    if resolved in seen:
        raise ValueError(f"recursive technology-tree include: {path}")
    seen.add(resolved)
    entries: dict[str, int] = {}
    for raw in path.read_text(encoding="latin-1").splitlines():
        line = without_comment(raw)
        if not line:
            continue
        include = INCLUDE.fullmatch(line)
        if include:
            entries.update(parse_tree(path.parent / include.group(1), seen))
            continue
        entry = TECH_ENTRY.match(line)
        if entry:
            entries[entry.group(1).casefold()] = int(entry.group(2))
    seen.remove(resolved)
    return entries


def index_odfs(root: Path) -> dict[str, Path]:
    return {path.name.casefold(): path for path in root.rglob("*.odf")}


def odf_metadata(path: Path, indexed: dict[str, Path], seen: set[Path] | None = None) -> dict[str, str]:
    """Merge included ODF values before the including file's local values."""
    seen = set() if seen is None else seen
    resolved = path.resolve()
    if resolved in seen:
        raise ValueError(f"recursive ODF include: {path}")
    seen.add(resolved)
    values: dict[str, str] = {}
    for raw in path.read_text(encoding="latin-1").splitlines():
        line = without_comment(raw)
        include = ODF_INCLUDE.fullmatch(line)
        if include:
            parent = indexed.get(include.group(1).casefold())
            if parent:
                values.update(odf_metadata(parent, indexed, seen))
            continue
        assignment = ASSIGNMENT.match(line)
        if assignment:
            values[assignment.group(1).casefold()] = assignment.group(2).strip().strip('"')
    seen.remove(resolved)
    return values


def category(path: Path, metadata: dict[str, str]) -> str | None:
    relative = {part.casefold() for part in path.parts}
    name = path.stem.casefold()
    if metadata.get("isshipupgrade") == "1":
        return "upgrade_pod"
    if "ships" in relative:
        if metadata.get("shipclass", "").casefold() == "special":
            return "special_unit"
        return "ship"
    if "stations" not in relative:
        return None
    if "turret" in name:
        return "defense"
    if metadata.get("classlabel", "").casefold() == "research":
        return "research_structure"
    return "station"


def main() -> int:
    args = arguments()
    indexed = index_odfs(args.odf)
    metadata_cache: dict[Path, dict[str, str]] = {}
    entries: list[dict[str, object]] = []
    excluded: list[dict[str, str]] = []

    for tree in sorted(args.techtree.glob("a2_*.tt"), key=lambda item: item.name.casefold()):
        match = MISSION_TREE.fullmatch(tree.name)
        if not match:
            continue
        faction_key, number = match.groups()
        faction = FACTIONS[faction_key.casefold()]
        prefix = PREFIXES[faction]
        states = parse_tree(tree)
        for odf_name, state in sorted(states.items()):
            if state < 0 or not odf_name.startswith(prefix):
                continue
            path = indexed.get(odf_name)
            if not path:
                excluded.append({"mission_tree": tree.name, "odf": odf_name, "reason": "odf_not_found"})
                continue
            metadata = metadata_cache.setdefault(path, odf_metadata(path, indexed))
            kind = category(path, metadata)
            if kind is None:
                continue
            entries.append({
                "mission_module": f"a2_{faction_key.casefold()}{number}S.dsl",
                "faction": faction,
                "mission_number": int(number),
                "tech_tree": tree.name,
                "odf": odf_name.removesuffix(".odf"),
                "sod": metadata.get("sodfilename", ""),
                "display_name": metadata.get("unitname", odf_name.removesuffix(".odf")),
                "category": kind,
                "tech_requirement": state,
                "class_label": metadata.get("classlabel", ""),
                "is_ship_upgrade": metadata.get("isshipupgrade") == "1",
                "upgrade_system": metadata.get("upgradesystem", ""),
                "upgrade_level": metadata.get("upgradelevel", ""),
            })

    counts = Counter(str(entry["category"]) for entry in entries)
    mission_counts = Counter(str(entry["mission_module"]) for entry in entries)
    document = {
        "schema_version": 1,
        "identity": "mission_module + faction + odf",
        "source": "resolved campaign .tt include/override chain plus ODF inheritance",
        "eligibility_rule": "only player-faction ship/station/defense/research/upgrade entries with final tech state >= 0",
        "runtime_rule": "eligible does not itself create a check; require a pinned local normal queue-to-completion event",
        "entry_count": len(entries),
        "entry_count_by_category": dict(sorted(counts.items())),
        "entry_count_by_mission": dict(sorted(mission_counts.items())),
        "entries": entries,
        "excluded": excluded,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    lines = ["# Generated mission build eligibility", "", f"- Eligible build identities: **{len(entries)}**"]
    lines.extend(f"- {kind}: **{count}**" for kind, count in sorted(counts.items()))
    lines.extend(["", "Entries come from each mission's resolved campaign technology tree.",
                  "Eligibility is only a generation filter; runtime still requires local normal queue completion.", ""])
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    args.summary.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {len(entries)} eligible build identities across {len(mission_counts)} missions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
