"""Version-pinned native campaign-route descriptions.

These are *not* direct map-load commands.  A route is eligible only after its
faction selector value and zero-based mission ordinal have been observed at the
stock ``SetupMission`` -> selector -> ``SetMissionFilename`` handoff.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class NativeCampaignRoute:
    mission_module: str
    bzn_filename: str
    mission_ordinal: int


NATIVE_ROUTES = {}
for _prefix in ("fed", "kling", "borg"):
    for _number in range(1, 11):
        _module = f"a2_{_prefix}{_number:02d}S.dsl"
        NATIVE_ROUTES[_module.casefold()] = NativeCampaignRoute(
            mission_module=_module,
            bzn_filename=f"a2_{_prefix}{_number:02d}.bzn",
            mission_ordinal=_number - 1,
        )


def route_for_module(module: str) -> NativeCampaignRoute | None:
    """Return only a route with a known stock controller mapping."""
    return NATIVE_ROUTES.get(Path(module).name.casefold())


def launch_request(route: NativeCampaignRoute) -> dict[str, int | str]:
    """Stable payload for the future UI-thread native launcher bridge.

    The bridge must apply these selectors through the stock campaign controller;
    it must never invoke a raw BZN/map loader.
    """
    return {
        "mission_module": route.mission_module,
        "bzn_filename": route.bzn_filename,
        "selected_mission": route.mission_ordinal,
        "controller": "SetupMission/native selector/QueueNextFilename/SetMissionFilename",
    }
