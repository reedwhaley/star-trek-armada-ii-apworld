"""Armada II progression and planned expansion item metadata."""

from __future__ import annotations

from dataclasses import dataclass


ITEM_OFFSET = 7_862_000
FACTIONS = ("Federation", "Klingon", "Borg")
FACTION_KEYS = {
    "Federation": "Temporal Research Facility",
    "Klingon": "Shockwave Station",
    "Borg": "Transwarp Gate",
}
FINAL_ACCESS_ITEM = "Fluidic Space"
VICTORY_ITEM = "Victory"
FILLER_ITEM = "Campaign Intelligence"

PERMANENT_UPGRADES = (
    "Construction Efficiency",
    "Shipyard Throughput",
    "Weapon Damage",
    "Impulse Speed",
    "Shield Capacity",
)
# Five faction-local permanent lines, twenty copies at five percent each.
PERMANENT_UPGRADE_COUNTS = {name: 20 for name in PERMANENT_UPGRADES}

# The remaining 168 non-progression slots. The generator shuffles this exact
# weighted composition; it never depends on dictionary iteration order.
HELPFUL_ITEM_COUNTS = {
    # Keep the 268-slot expansion pool stable while giving both principal
    # economy resources equal cache representation.
    "Dilithium Cache": 18,
    "Metal Cache": 18,
    "Emergency Repairs": 32,
    "Slipstream Drive": 28,
}
TRAP_ITEM_COUNTS = {
    # Eight equal trap lines retain the fixed 72-slot trap allocation.
    "Nebula Anomaly": 9,
    "Engine Disruption": 9,
    "Weapons Malfunction": 9,
    "Sensor Blackout": 9,
    "E.P.S. Conduit Rupture": 9,
    "Warp Field Collapse": 9,
    "Dilithium Loss": 9,
    "Metal Loss": 9,
}
EXPANSION_ITEM_COUNTS = {**PERMANENT_UPGRADE_COUNTS, **HELPFUL_ITEM_COUNTS, **TRAP_ITEM_COUNTS}
EXPANSION_ITEM_TOTAL = sum(EXPANSION_ITEM_COUNTS.values())
TRAP_ITEM_TOTAL = sum(TRAP_ITEM_COUNTS.values())


@dataclass(frozen=True)
class ItemData:
    code: int
    classification: str


item_table: dict[str, ItemData] = {}
for faction in FACTIONS:
    for number in range(1, 11):
        item_table[f"{faction} Mission {number} Access"] = ItemData(
            ITEM_OFFSET + len(item_table), "progression"
        )
for key in (*FACTION_KEYS.values(), FINAL_ACCESS_ITEM):
    item_table[key] = ItemData(ITEM_OFFSET + len(item_table), "progression")
item_table[FILLER_ITEM] = ItemData(ITEM_OFFSET + len(item_table), "filler")
for name in PERMANENT_UPGRADES:
    item_table[name] = ItemData(ITEM_OFFSET + len(item_table), "useful")
for name in HELPFUL_ITEM_COUNTS:
    item_table[name] = ItemData(ITEM_OFFSET + len(item_table), "useful")
for name in TRAP_ITEM_COUNTS:
    item_table[name] = ItemData(ITEM_OFFSET + len(item_table), "trap")
item_table[VICTORY_ITEM] = ItemData(ITEM_OFFSET + len(item_table), "progression")

item_name_to_id = {name: data.code for name, data in item_table.items()}


def expansion_item_pool(trap_count: int = TRAP_ITEM_TOTAL, rng=None) -> list[str]:
    """Return the fixed-size expansion pool with a configurable trap share.

    Omitted traps are replaced one-for-one by weighted positive filler.  The
    caller supplies the world's seeded RNG, so YAML ``random`` remains
    reproducible for a generated seed rather than using process-global state.
    """
    if not 0 <= trap_count <= TRAP_ITEM_TOTAL:
        raise ValueError(f"trap_count must be in [0, {TRAP_ITEM_TOTAL}], got {trap_count}")
    permanent = [name for name, count in PERMANENT_UPGRADE_COUNTS.items() for _ in range(count)]
    helpful = [name for name, count in HELPFUL_ITEM_COUNTS.items() for _ in range(count)]
    traps = [name for name, count in TRAP_ITEM_COUNTS.items() for _ in range(count)]
    if trap_count == TRAP_ITEM_TOTAL:
        selected_traps = traps
    elif rng is None:
        # Deterministic dependency-free behavior for metadata/unit tests.
        selected_traps = traps[:trap_count]
    else:
        selected_traps = rng.sample(traps, trap_count)
    replacements = TRAP_ITEM_TOTAL - trap_count
    if rng is None:
        selected_helpful = [helpful[index % len(helpful)] for index in range(replacements)]
    else:
        selected_helpful = [rng.choice(helpful) for _ in range(replacements)]
    return [*permanent, *helpful, *selected_traps, *selected_helpful]
