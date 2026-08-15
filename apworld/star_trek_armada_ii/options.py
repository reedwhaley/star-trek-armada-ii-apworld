"""Archipelago options, with dependency-free defaults for local metadata tests."""

from __future__ import annotations

from dataclasses import dataclass

try:
    from Options import Choice, PerGameCommonOptions, Range
except ModuleNotFoundError:
    @dataclass(frozen=True)
    class BaseOptions:
        starting_faction: str = "random"
        other_mission_completion_requirement: int = 0
        trap_count: int = 72
else:
    class StartingFaction(Choice):
        display_name = "Starting faction"
        option_federation = 0
        option_klingon = 1
        option_borg = 2
        option_random = 3
        default = 3

    class OtherMissionCompletionRequirement(Range):
        display_name = "Other mission completion requirement"
        range_start = 0
        range_end = 29
        default = 0

    class TrapCount(Range):
        """Number of traps placed for this player; YAML also accepts random."""
        display_name = "Trap count"
        range_start = 0
        range_end = 72
        default = 72

    @dataclass
    class ArmadaIIOptions(PerGameCommonOptions):
        starting_faction: StartingFaction
        other_mission_completion_requirement: OtherMissionCompletionRequirement
        trap_count: TrapCount
