"""Load the generated, reviewed campaign objective catalog."""

from __future__ import annotations

import json
from importlib.resources import files


def load_catalog() -> dict:
    # Archipelago loads .apworld packages directly from a ZIP.  importlib
    # resources works for both that loader and a source-tree test run.
    catalog_path = files(__package__).joinpath("data", "campaign-objectives.json")
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    if catalog.get("schema_version") != 1:
        raise ValueError("Unsupported Armada II objective catalog schema")
    if catalog.get("candidate_count") != 125:
        raise ValueError("Armada II base catalog must contain 125 objective candidates")
    return catalog


def load_build_eligibility() -> dict:
    """Load the generated mission-tech build eligibility matrix."""
    catalog_path = files(__package__).joinpath("data", "mission-build-eligibility.json")
    catalog = json.loads(catalog_path.read_text(encoding="utf-8"))
    if catalog.get("schema_version") != 1 or catalog.get("entry_count") != 1062:
        raise ValueError("Unsupported Armada II build-eligibility catalog")
    return catalog
