"""Star Trek: Armada II Archipelago world and Launcher registration."""

from __future__ import annotations

import os
import traceback

from .items import (FACTIONS, FACTION_KEYS, FILLER_ITEM, FINAL_ACCESS_ITEM,
                    ITEM_OFFSET, VICTORY_ITEM, expansion_item_pool, item_name_to_id, item_table)
from .locations import (LOCATION_OFFSET, LocationData, build_location_by_event, location_name_to_id,
                        location_table, mission_location_by_module, objective_location_by_event, victory_location_name)

GAME_NAME = "Star Trek: Armada II"

try:
    from BaseClasses import Item, ItemClassification, Location, Region
    from worlds.AutoWorld import World
    from worlds.LauncherComponents import Component, Type, components, icon_paths, launch_subprocess
    from worlds.generic.Rules import set_rule
    from .options import ArmadaIIOptions
except ModuleNotFoundError:
    # Enables catalog/ledger tests outside a full Archipelago installation.
    APWORLD_RUNTIME_AVAILABLE = False
else:
    APWORLD_RUNTIME_AVAILABLE = True

    class ArmadaIIItem(Item):
        game = GAME_NAME

    class ArmadaIILocation(Location):
        game = GAME_NAME

    class StarTrekArmadaIIWorld(World):
        game = GAME_NAME
        options_dataclass = ArmadaIIOptions
        item_name_to_id = item_name_to_id
        location_name_to_id = location_name_to_id
        origin_region_name = "Menu"
        victory_location_name = victory_location_name
        ut_can_gen_without_yaml = True

        @staticmethod
        def interpret_slot_data(slot_data: dict) -> dict:
            return slot_data

        def generate_early(self) -> None:
            tracker_data = getattr(self.multiworld, "re_gen_passthrough", {}).get(self.game, {})
            self.starting_faction = tracker_data.get("starting_faction")
            if self.starting_faction not in FACTIONS:
                selected = self.options.starting_faction.value
                self.starting_faction = self.random.choice(FACTIONS) if selected == 3 else FACTIONS[selected]
            self.generated_trap_amount = self.options.trap_count.value

        def create_regions(self) -> None:
            menu = Region("Menu", self.player, self.multiworld)
            campaign = Region("Campaign", self.player, self.multiworld)
            menu.connect(campaign)
            for name, data in location_table.items():
                campaign.locations.append(ArmadaIILocation(self.player, name, data.code, campaign))
            self.multiworld.regions.extend((menu, campaign))
            self.multiworld.get_location(victory_location_name, self.player).place_locked_item(self.create_item(VICTORY_ITEM))

        def create_items(self) -> None:
            starting = [f"{self.starting_faction} Mission 1 Access", FACTION_KEYS[self.starting_faction]]
            for name in starting:
                self.multiworld.push_precollected(self.create_item(name))
            capacity = len(location_table) - 1
            progression = [f"{faction} Mission {number} Access" for faction in FACTIONS for number in range(1, 11)]
            progression.extend((*FACTION_KEYS.values(), FINAL_ACCESS_ITEM))
            pool = [name for name in progression if name not in starting]
            pool.extend(expansion_item_pool(self.generated_trap_amount, self.random))
            self.random.shuffle(pool)
            pool.extend([FILLER_ITEM] * (capacity - len(pool)))
            self.multiworld.itempool.extend(self.create_item(name) for name in pool)

        def set_rules(self) -> None:
            for name, data in location_table.items():
                if data.kind == "build":
                    eligible_modules = data.eligible_mission_modules

                    def build_is_reachable(state, faction=data.faction, modules=eligible_modules):
                        if not state.has(FACTION_KEYS[faction], self.player):
                            return False
                        for module in modules:
                            if module.casefold() == "a2_borg10s.dsl":
                                if (state.has(FINAL_ACCESS_ITEM, self.player)
                                        and all(state.has(key, self.player) for key in FACTION_KEYS.values())):
                                    return True
                                continue
                            mission = location_table[mission_location_by_module[module.casefold()]]
                            if state.has(f"{mission.faction} Mission {mission.mission_number} Access", self.player):
                                return True
                        return False

                    set_rule(self.multiworld.get_location(name, self.player), build_is_reachable)
                    continue
                if data.mission_module.casefold() == "a2_borg10s.dsl":
                    requirements = (FINAL_ACCESS_ITEM, *FACTION_KEYS.values())
                else:
                    requirements = (f"{data.faction} Mission {data.mission_number} Access", FACTION_KEYS[data.faction])
                set_rule(self.multiworld.get_location(name, self.player),
                         lambda state, requirements=requirements: all(state.has(item, self.player) for item in requirements))
            self.multiworld.completion_condition[self.player] = lambda state: state.has(VICTORY_ITEM, self.player)

        def create_item(self, name: str) -> ArmadaIIItem:
            data = item_table[name]
            classification = getattr(ItemClassification, data.classification)
            return ArmadaIIItem(name, classification, data.code, self.player)

        def get_filler_item_name(self) -> str:
            return FILLER_ITEM

        def fill_slot_data(self) -> dict:
            mission_data = {
                data.mission_module.casefold(): {
                    "faction": data.faction, "number": data.mission_number,
                    "title": "Rifts" if data.mission_module.casefold() == "a2_borg10s.dsl" else data.mission_name,
                    "map": data.mission_module.removesuffix("S.dsl") + ".bzn",
                }
                for data in location_table.values() if data.kind == "mission"
            }
            return {
                "generation_options": self.options.as_dict("starting_faction", "other_mission_completion_requirement", "trap_count"),
                "starting_faction": self.starting_faction,
                "mission_locations": {data.mission_module.casefold(): name for name, data in location_table.items() if data.kind == "mission"},
                "objective_locations": objective_location_by_event,
                "build_locations": build_location_by_event,
                "missions": mission_data,
                "faction_key_names": FACTION_KEYS,
                "final_access_item": FINAL_ACCESS_ITEM,
                "final_mission_requirements": [FINAL_ACCESS_ITEM, *FACTION_KEYS.values()],
                "goal_location": victory_location_name,
                "other_mission_completion_requirement": self.options.other_mission_completion_requirement.value,
                "item_effect_manifest": {}, "trap_manifest": {}, "generated_trap_amount": self.generated_trap_amount,
            }

    def _run_client(*args: str) -> None:
        try:
            from .Client import launch
            launch(*args)
        except BaseException:
            log_dir = os.path.join(os.environ.get("ProgramData", r"C:\\ProgramData"), "Archipelago", "logs")
            os.makedirs(log_dir, exist_ok=True)
            with open(os.path.join(log_dir, "StarTrekArmadaIIClient-bootstrap-error.txt"), "w", encoding="utf-8") as stream:
                traceback.print_exc(file=stream)
            raise

    def launch_client(*args: str) -> None:
        launch_subprocess(_run_client, name="StarTrekArmadaIIClient", args=args)

    components.append(Component(
        "Star Trek: Armada II Client", func=launch_client, component_type=Type.CLIENT,
        game_name=GAME_NAME, supports_uri=True, icon="star_trek_armada_ii",
        description="Connect to Archipelago and launch unlocked Armada II campaign missions.",
    ))
    icon_paths["star_trek_armada_ii"] = f"ap:{__name__}/assets/armada2.ico"

__all__ = ("GAME_NAME", "item_name_to_id", "item_table", "location_name_to_id", "location_table")
