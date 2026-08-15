#!/usr/bin/env python3
"""Build the reviewed static Armada II campaign objective candidate catalog."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections import Counter, defaultdict
from pathlib import Path


FILE_PATTERN = re.compile(r"^a2_(fed|kling|borg)(\d{2}).*\.txt$", re.IGNORECASE)
FAILURE_PATTERN = re.compile(
    r"\bmust survive\b|\bmust not\b|\bdo not lose\b|\bcannot be destroyed\b|\bmust remain\b",
    re.IGNORECASE,
)


def normalize(text: str) -> str:
    return " ".join(text.casefold().split())


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--objectives", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    raw_entries: list[dict[str, object]] = []
    excluded: list[dict[str, object]] = []

    for path in sorted(args.objectives.glob("a2_*.txt"), key=lambda item: item.name.casefold()):
        match = FILE_PATTERN.fullmatch(path.name)
        if not match:
            continue
        faction, number_text = match.groups()
        faction = faction.capitalize() if faction != "kling" else "Klingon"
        faction = {"Fed": "Federation", "Borg": "Borg", "Klingon": "Klingon"}[faction]
        number = int(number_text)
        module_stem = f"a2_{match.group(1).casefold()}{number:02d}"
        mission_module = f"{module_stem}S.dsl"
        lines = path.read_text(encoding="latin-1").splitlines()
        for index, line in enumerate(lines):
            stripped = line.lstrip()
            if not stripped.startswith("*"):
                continue
            text = stripped[1:].strip()
            source = {
                "objective_file": path.name,
                "objective_index": sum(1 for prior in lines[:index] if prior.lstrip().startswith("*")),
                "display_text": text,
            }
            if FAILURE_PATTERN.search(text):
                excluded.append({"faction": faction, "mission_number": number,
                                 "mission_module": mission_module, **source,
                                 "reason": "failure_only_language"})
                continue
            raw_entries.append({"faction": faction, "mission_number": number,
                                "mission_module": mission_module, **source,
                                "normalized_text": normalize(text)})

    grouped: dict[tuple[str, str], list[dict[str, object]]] = defaultdict(list)
    for entry in raw_entries:
        grouped[(str(entry["mission_module"]).casefold(), str(entry["normalized_text"]))].append(entry)

    entries: list[dict[str, object]] = []
    for (module, normalized_text), aliases in sorted(grouped.items()):
        primary = aliases[0]
        key_material = f"{module}|{normalized_text}".encode("utf-8")
        entry_id = f"{module.removesuffix('.dsl')}:objective:{hashlib.sha1(key_material).hexdigest()[:12]}"
        entries.append({
            "id": entry_id,
            "faction": primary["faction"],
            "mission_number": primary["mission_number"],
            "mission_module": primary["mission_module"],
            "objective_file": primary["objective_file"],
            "objective_index": primary["objective_index"],
            "display_text": primary["display_text"],
            "normalized_text": normalized_text,
            "aliases": [{key: alias[key] for key in ("objective_file", "objective_index", "display_text")}
                        for alias in aliases],
        })

    by_faction = Counter(str(entry["faction"]) for entry in entries)
    document = {
        "schema_version": 1,
        "identity": "mission_module + objective_file + objective_index",
        "deduplication": "same normalized text within one mission module",
        "exclusion": "failure-only wording: must survive, must not, do not lose, cannot be destroyed, must remain",
        "candidate_count": len(entries),
        "candidate_count_by_faction": dict(sorted(by_faction.items())),
        "entries": entries,
        "excluded_failure_conditions": excluded,
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    summary = "\n".join([
        "# Generated objective catalog summary",
        "",
        f"- Candidate progress objectives: **{len(entries)}**",
        f"- Federation: **{by_faction['Federation']}**",
        f"- Klingon: **{by_faction['Klingon']}**",
        f"- Borg: **{by_faction['Borg']}**",
        f"- Excluded failure-only entries: **{len(excluded)}**",
        "",
        "Runtime keys are `mission_module + objective_file + objective_index`;",
        "aliases retain revised objective-list variants for the same semantic location.",
        "",
        "The JSON catalog is the machine-readable source of truth.",
        "",
    ])
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    args.summary.write_text(summary, encoding="utf-8")
    print(f"wrote {len(entries)} candidates and {len(excluded)} excluded failure conditions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
