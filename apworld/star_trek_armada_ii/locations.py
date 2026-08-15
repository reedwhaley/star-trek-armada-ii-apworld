"""Armada II campaign and mission-aware first-build locations."""

from __future__ import annotations

from dataclasses import dataclass

from .catalog import load_build_eligibility, load_catalog
from .items import FACTIONS


LOCATION_OFFSET = 7_863_000
RIFTS_MODULE = "a2_borg10S.dsl"

# Stock campaign titles, transcribed from the campaign objective headers.  The
# numeric faction/mission identity remains stable for AP access items and
# locations; these titles are the player-facing launcher labels.
MISSION_TITLES = {
    "Federation": (
        "Invasion", "Lifelines", "Recovery", "Along the Neutral Zone",
        "Into the Breach", "Inferno", "The Cavalry", "Data Retrieval",
        "Staging Grounds", "There and Back Again",
    ),
    "Klingon": (
        "Uprising: A Dagger at Zarush", "Executioner", "Brave New Worlds",
        "Blockade", "The Unknown Prize", "The Romulan Connection",
        "Battle of Crucis Major", "Trojan Horse", "To Cardassia and Victory",
        "The Final Battle",
    ),
    "Borg": (
        "Werewolf Pack", "Pink Slips", "Interdiction",
        "There Goes the Neighborhood", "The Catch", "Interception",
        "Strange Bedfellows", "Tidal Wave", "The Maw", "Rifts",
    ),
}


@dataclass(frozen=True)
class LocationData:
    code: int
    faction: str
    mission_number: int
    kind: str
    mission_module: str
    objective_file: str | None = None
    objective_index: int | None = None
    catalog_id: str | None = None
    build_odf: str | None = None
    eligible_mission_modules: tuple[str, ...] = ()

    @property
    def mission_name(self) -> str:
        return MISSION_TITLES[self.faction][self.mission_number - 1]


catalog = load_catalog()
location_table: dict[str, LocationData] = {}
for faction in FACTIONS:
    for number in range(1, 11):
        prefix = {"Federation": "fed", "Klingon": "kling", "Borg": "borg"}[faction]
        module = f"a2_{prefix}{number:02d}S.dsl"
        name = f"{faction} Mission {number} Complete"
        location_table[name] = LocationData(LOCATION_OFFSET + len(location_table), faction, number, "mission", module)

objective_entries = sorted(
    catalog["entries"],
    key=lambda entry: (entry["faction"], entry["mission_number"], entry["objective_index"], entry["display_text"]),
)
objective_location_by_event: dict[str, str] = {}
for entry in objective_entries:
    faction = str(entry["faction"])
    number = int(entry["mission_number"])
    module = str(entry["mission_module"])
    ordinal = 1 + sum(
        1 for existing in location_table.values()
        if existing.kind == "objective" and existing.mission_module.casefold() == module.casefold()
    )
    name = f"{faction} Mission {number} Objective {ordinal}"
    location_table[name] = LocationData(
        LOCATION_OFFSET + len(location_table), faction, number, "objective", module,
        str(entry["objective_file"]), int(entry["objective_index"]), str(entry["id"]),
    )
    for alias in entry["aliases"]:
        event_key = "|".join((module.casefold(), str(alias["objective_file"]).casefold(), str(alias["objective_index"])))
        objective_location_by_event[event_key] = name

build_catalog = load_build_eligibility()
build_location_by_event: dict[str, str] = {}
build_identities: dict[tuple[str, str], str] = {}
build_eligible_modules: dict[tuple[str, str], set[str]] = {}
for entry in build_catalog["entries"]:
    identity = (str(entry["faction"]), str(entry["odf"]))
    build_eligible_modules.setdefault(identity, set()).add(str(entry["mission_module"]))

for entry in sorted(build_catalog["entries"], key=lambda item: (item["faction"], item["odf"], item["mission_module"])):
    faction = str(entry["faction"])
    odf = str(entry["odf"])
    identity = (faction, odf)
    name = build_identities.get(identity)
    if name is None:
        display_name = str(entry["display_name"])
        name = f"{faction} First Build: {display_name} ({odf})"
        build_identities[identity] = name
        location_table[name] = LocationData(
            LOCATION_OFFSET + len(location_table), faction, int(entry["mission_number"]), "build",
            str(entry["mission_module"]), build_odf=odf,
            eligible_mission_modules=tuple(sorted(build_eligible_modules[identity])),
        )
    event_key = "|".join((str(entry["mission_module"]).casefold(), odf.casefold()))
    build_location_by_event[event_key] = name

location_name_to_id = {name: data.code for name, data in location_table.items()}
mission_location_by_module = {
    data.mission_module.casefold(): name for name, data in location_table.items() if data.kind == "mission"
}
victory_location_name = "Borg Mission 10 Complete"

if len(build_identities) != 147 or len(location_table) != 302:
    raise ValueError(f"Armada II location table expected 155 campaign + 147 build locations, found {len(location_table)}")
