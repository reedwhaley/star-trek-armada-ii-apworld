"""Version-pinned, adapter-neutral Armada II observer event contract.

The observer DLL is deliberately not implemented here.  This contract lets the
network client reject malformed or unpinned events before they can reach the
durable check ledger.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any


OBSERVER_PIPE = r"\\.\pipe\archipelago_armada2_observer_v1"
CONTROL_PIPE = r"\\.\pipe\archipelago_armada2_control_v1"
STOCK_EXE_SHA256 = "c01ff40248bc4c711ea2cde60deda2b9862a8274b18b5537e618fa7b61957ae0"


def validate_event(raw: Any) -> dict[str, Any] | None:
    """Return a safe observer event or reject it before ledger mutation."""
    if not isinstance(raw, dict):
        return None
    event_type = raw.get("type")
    if event_type == "adapter_status":
        return dict(raw) if str(raw.get("adapter", "")) == "armada2_observer" else None
    if event_type not in {"objective_complete", "mission_result", "build_complete"}:
        return None
    if str(raw.get("adapter", "")) != "armada2_observer":
        return None
    if str(raw.get("executable_sha256", "")).casefold() != STOCK_EXE_SHA256:
        return None
    module = Path(str(raw.get("mission_module") or raw.get("caller_module", ""))).name
    if not module.casefold().endswith(".dsl"):
        return None
    event = dict(raw)
    event["mission_module"] = module
    if event_type == "objective_complete":
        if not isinstance(raw.get("objective_index"), int):
            return None
        if not str(raw.get("objective_file", "")):
            return None
        if raw.get("complete") is not True or raw.get("initial") is not False:
            return None
    elif event_type == "mission_result" and raw.get("result") not in {"success", "failure"}:
        return None
    elif event_type == "build_complete":
        odf = str(raw.get("odf", "")).casefold()
        if not odf or any(character not in "abcdefghijklmnopqrstuvwxyz0123456789_-" for character in odf):
            return None
        if raw.get("normal_queue") is not True or raw.get("local_player") is not True or raw.get("owner_verified") is not True:
            return None
        event["odf"] = odf
    return event
