"""Crash-safe local state for the Armada II network client.

This file deliberately contains no game writes.  It only records Archipelago
state and observer events so reconnects cannot duplicate checks or items.
"""

from __future__ import annotations

import json
import os
import tempfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


class ClientLedger:
    """A small atomic JSON ledger, loaded once for the lifetime of a client."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.data: dict[str, Any] = {
            "checks": {}, "received_items": {}, "traps": {}, "helpful": {}, "mission_state": {},
        }
        if path.is_file():
            loaded = json.loads(path.read_text(encoding="utf-8"))
            if not isinstance(loaded, dict):
                raise ValueError(f"invalid Armada II ledger: {path}")
            self.data.update(loaded)

    def _save(self) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, temporary = tempfile.mkstemp(prefix=self.path.name + ".", suffix=".tmp", dir=self.path.parent)
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as stream:
                json.dump(self.data, stream, indent=2, sort_keys=True)
                stream.write("\n")
            os.replace(temporary, self.path)
        except BaseException:
            try:
                os.unlink(temporary)
            except FileNotFoundError:
                pass
            raise

    def record_received_items(self, start_index: int, items: list[Any], item_names: dict[int, str]) -> None:
        """Persist server ReceivedItems indexes exactly once before effects."""
        received = self.data["received_items"]
        changed = False
        for offset, item in enumerate(items):
            index = str(start_index + offset)
            if index in received:
                continue
            item_id = int(item.item) if hasattr(item, "item") else int(item["item"] if isinstance(item, dict) else item[0])
            received[index] = {"item_id": item_id, "name": item_names.get(item_id, str(item_id)), "recorded_at": _now()}
            changed = True
        if changed:
            self._save()

    def item_count(self, name: str) -> int:
        return sum(1 for record in self.data["received_items"].values() if record.get("name") == name)

    def pending_traps(self, trap_names: set[str]) -> list[tuple[int, str]]:
        consumed = self.data["traps"]
        return [(int(index), str(record["name"])) for index, record in self.data["received_items"].items()
                if str(record.get("name", "")) in trap_names and index not in consumed]

    def mark_trap_dispatched(self, index: int, mission_module: str, command: str) -> None:
        key = str(index)
        if key in self.data["traps"]:
            return
        self.data["traps"][key] = {"status": "dispatched", "mission_module": mission_module,
                                   "command": command, "dispatched_at": _now()}
        self._save()

    def pending_helpful(self, item_names: set[str]) -> list[tuple[int, str]]:
        consumed = self.data["helpful"]
        return [(int(index), str(record["name"])) for index, record in self.data["received_items"].items()
                if str(record.get("name", "")) in item_names and index not in consumed]

    def mark_helpful_dispatched(self, index: int, mission_module: str, command: str) -> None:
        key = str(index)
        if key in self.data["helpful"]:
            return
        self.data["helpful"][key] = {"status": "dispatched", "mission_module": mission_module,
                                      "command": command, "dispatched_at": _now()}
        self._save()

    def record_check(self, location_name: str, source_event: dict[str, Any]) -> bool:
        """Write a pending check before the network packet is sent."""
        checks = self.data["checks"]
        if location_name in checks:
            return False
        checks[location_name] = {"status": "pending", "source_event": source_event, "recorded_at": _now()}
        self._save()
        return True

    def pending_check_names(self) -> list[str]:
        return [name for name, record in self.data["checks"].items() if record.get("status") == "pending"]

    def mark_submitted(self, names: list[str]) -> None:
        changed = False
        for name in names:
            record = self.data["checks"].get(name)
            if record and record.get("status") == "pending":
                record["status"] = "submitted"
                record["submitted_at"] = _now()
                changed = True
        if changed:
            self._save()

    def submitted_mission_count(self, mission_location_names: set[str]) -> int:
        return sum(1 for name in mission_location_names if self.data["checks"].get(name, {}).get("status") == "submitted")
