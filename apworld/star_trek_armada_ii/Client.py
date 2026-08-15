"""Normal Archipelago client shell for Star Trek: Armada II."""

from __future__ import annotations

import argparse
import asyncio
import ctypes
import hashlib
import json
import os
import subprocess
import threading
import time
from ctypes import wintypes
from pathlib import Path
from typing import Any

from CommonClient import CommonContext, get_base_parser, gui_enabled, handle_url_arg, logger, server_loop
from NetUtils import ClientStatus
import Utils

try:
    from worlds.tracker.TrackerClient import TrackerGameContext as ContextBase
except ModuleNotFoundError:
    ContextBase = CommonContext
    UNIVERSAL_TRACKER_AVAILABLE = False
else:
    UNIVERSAL_TRACKER_AVAILABLE = True

from . import GAME_NAME
from .client_ledger import ClientLedger
from .items import (FACTION_KEYS, FINAL_ACCESS_ITEM, HELPFUL_ITEM_COUNTS, PERMANENT_UPGRADES,
                    VICTORY_ITEM, item_name_to_id)
from .items import TRAP_ITEM_COUNTS
from .locations import location_name_to_id, mission_location_by_module
from .native_launcher import launch_request, route_for_module
from .observer_protocol import OBSERVER_PIPE, validate_event


GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value
ERROR_FILE_NOT_FOUND = 2
ERROR_PIPE_BUSY = 231
ERROR_PIPE_NOT_CONNECTED = 233
ERROR_BROKEN_PIPE = 109
CREATE_NO_WINDOW = 0x08000000
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
kernel32.CreateFileW.restype = wintypes.HANDLE
kernel32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                               ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
kernel32.ReadFile.restype = wintypes.BOOL
kernel32.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID, wintypes.DWORD,
                               ctypes.POINTER(wintypes.DWORD), wintypes.LPVOID]
kernel32.WriteFile.restype = wintypes.BOOL

CONTROL_PIPE = r"\\.\pipe\archipelago_armada2_control_v1"
CONTROL_REQUEST_LOCK = threading.Lock()
SETTINGS_PATH = (Path(os.environ.get("LOCALAPPDATA", str(Path.home()))) /
                 "Archipelago" / "StarTrekArmadaII" / "settings.json")
STARTUP_BINK_FILES = ("Activision.bik", "Intro.bik", "MadDocSoftware.bik", "Paramount.bik")


def startup_bink_directory(game_root: Path) -> Path:
    """Resolve the stock startup-movie directory for supported installs."""
    for candidate in (game_root, game_root / "animations"):
        if all((candidate / name).is_file() or
               (candidate / f"{name}.disabled").is_file()
               for name in STARTUP_BINK_FILES):
            return candidate
    raise FileNotFoundError(f"Could not find all Armada II startup movies under: {game_root}")


def suppress_startup_binks(game_root: Path) -> tuple[Path, ...]:
    """Temporarily hide only the four application-startup movies.

    The game falls through to the main menu when these stock files carry the
    ``.disabled`` suffix.  This is intentionally a reversible rename rather
    than a Bink/runtime code modification; briefing and mission movies are not
    touched.
    """
    movie_directory = startup_bink_directory(game_root)
    renamed: list[Path] = []
    try:
        for name in STARTUP_BINK_FILES:
            source = movie_directory / name
            disabled = source.with_name(source.name + ".disabled")
            if source.is_file():
                source.rename(disabled)
                # Retain only the files this client renamed, so a pre-existing
                # disabled state is never flipped during launch or cleanup.
                renamed.append(disabled)
            elif disabled.is_file():
                continue
            else:
                raise FileNotFoundError(f"Required startup movie is missing: {source}")
    except OSError:
        for disabled in reversed(renamed):
            if disabled.is_file() and not disabled.with_name(disabled.name.removesuffix(".disabled")).exists():
                disabled.rename(disabled.with_name(disabled.name.removesuffix(".disabled")))
        raise
    return tuple(renamed)


def restore_startup_binks(renamed: tuple[Path, ...] | list[Path]) -> None:
    """Restore only startup movies renamed by this managed launch."""
    for disabled in renamed:
        source = disabled.with_name(disabled.name.removesuffix(".disabled"))
        if disabled.is_file() and not source.exists():
            disabled.rename(source)


def _load_game_root() -> Path | None:
    """Return the install folder selected by the player, if it is still valid."""
    try:
        configured = json.loads(SETTINGS_PATH.read_text(encoding="utf-8")).get("game_root")
    except (OSError, ValueError, TypeError):
        return None
    root = Path(configured) if isinstance(configured, str) else None
    return root if root and (root / "Armada2.exe").is_file() else None


def _save_game_root(root: Path) -> None:
    SETTINGS_PATH.parent.mkdir(parents=True, exist_ok=True)
    SETTINGS_PATH.write_text(json.dumps({"game_root": str(root)}, indent=2) + "\n", encoding="utf-8")


def _prompt_for_game_root() -> Path | None:
    """Ask once for the stock install folder; never alter the installation."""
    try:
        import tkinter as tk
        from tkinter import filedialog
        dialog = tk.Tk()
        dialog.withdraw()
        dialog.attributes("-topmost", True)
        selected = filedialog.askdirectory(
            parent=dialog,
            title="Select the folder containing Armada2.exe",
            mustexist=True,
        )
        dialog.destroy()
    except Exception as error:
        logger.warning("Could not open the Armada II folder picker: %s", error)
        return None
    root = Path(selected) if selected else None
    if root and (root / "Armada2.exe").is_file():
        _save_game_root(root)
        return root
    return None


def run_hidden(command: list[str]) -> subprocess.CompletedProcess[str]:
    """Run a native helper without stealing focus from the game/client."""
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    return subprocess.run(command, capture_output=True, text=True, check=False,
                          startupinfo=startup, creationflags=CREATE_NO_WINDOW)
kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
kernel32.CloseHandle.restype = wintypes.BOOL


class ArmadaIIContext(ContextBase):
    game = GAME_NAME
    items_handling = 0b111
    tags = {"AP"}

    def __init__(self, server_address: str | None, password: str | None, ledger_directory: Path,
                 game_root: Path | None) -> None:
        super().__init__(server_address, password)
        self.ledger_directory = ledger_directory
        # No game/network event is accepted before Connected selects a
        # seed/team/slot-specific durable ledger.
        self.ledger = ClientLedger(ledger_directory / "unconnected.json")
        self.ledger_identity = ""
        self.game_root = game_root
        self.mission_locations: dict[str, str] = {}
        self.objective_locations: dict[str, str] = {}
        self.build_locations: dict[str, str] = {}
        self.missions: dict[str, dict] = {}
        self.goal_location = ""
        self.other_mission_completion_requirement = 0
        self.final_mission_requirements = [FINAL_ACCESS_ITEM, *FACTION_KEYS.values()]
        self.active_game: subprocess.Popen[bytes] | None = None
        self._build_telemetry_requested = False
        self.active_mission_module = ""
        self._trap_dispatch_lock = threading.Lock()
        self._helpful_dispatch_lock = threading.Lock()
        self._upgrade_apply_lock = threading.Lock()
        self._permanent_upgrade_session: tuple[int, str, tuple[int, int, int, int, int]] | None = None
        self._suppressed_startup_binks: tuple[Path, ...] = ()
        self._goal_report_in_progress = False
        self._goal_reported_for_connection = False
        self.launch_status = ("Select an unlocked mission to launch Armada II."
                              if game_root else "Select the Armada II install folder to enable mission launch.")
        self._launch_lock = threading.Lock()

    def run_gui(self) -> None:
        """Add the launcher tab while preserving the normal AP and Tracker UI."""
        from kivy.clock import Clock
        from kivy.metrics import dp
        from kivy.uix.boxlayout import BoxLayout
        from kivy.uix.button import Button
        from kivy.uix.gridlayout import GridLayout
        from kivy.uix.label import Label
        from kivy.uix.scrollview import ScrollView
        ctx = self

        class MissionPanel(ScrollView):
            def __init__(self) -> None:
                super().__init__()
                self.layout = GridLayout(cols=1, spacing=dp(6), padding=dp(10), size_hint_y=None)
                self.layout.bind(minimum_height=self.layout.setter("height"))
                self.add_widget(self.layout)
                Clock.schedule_interval(self.refresh, 0.5)

            def refresh(self, _dt: float) -> None:
                self.layout.clear_widgets()
                snapshot = ctx.mission_snapshot()
                self.layout.add_widget(Label(text=snapshot["status"], size_hint_y=None, height=dp(32)))
                for mission in snapshot["missions"]:
                    text = f"{mission['label']} — {mission['status']} — {mission['requires']}"
                    button = Button(text=text, disabled=not mission["available"],
                                    size_hint_y=None, height=dp(42))
                    if mission["available"]:
                        button.bind(on_release=lambda _button, selected=mission["module"]:
                                    ctx.start_managed_mission(selected))
                    self.layout.add_widget(button)

        manager_base = super().make_gui()

        class ArmadaIIManager(manager_base):
            base_title = "Archipelago Star Trek: Armada II Client"

            def build(self):
                container = super().build()
                self.add_client_tab("Mission Launcher", MissionPanel())
                return container

        self.ui = ArmadaIIManager(self)
        self.ui_task = asyncio.create_task(self.ui.async_run(), name="UI")

    def on_package(self, cmd: str, args: dict) -> None:
        super().on_package(cmd, args)
        if cmd == "Connected":
            self._select_room_ledger(args)
            slot_data = args.get("slot_data", {})
            self.mission_locations = {str(key).casefold(): str(value) for key, value in slot_data.get("mission_locations", {}).items()}
            self.objective_locations = {str(key).casefold(): str(value) for key, value in slot_data.get("objective_locations", {}).items()}
            self.build_locations = {str(key).casefold(): str(value) for key, value in slot_data.get("build_locations", {}).items()}
            self.missions = {str(key).casefold(): value for key, value in slot_data.get("missions", {}).items()}
            self.goal_location = str(slot_data.get("goal_location", ""))
            self.other_mission_completion_requirement = int(slot_data.get("other_mission_completion_requirement", 0))
            self.final_mission_requirements = list(slot_data.get("final_mission_requirements", self.final_mission_requirements))
            self._goal_reported_for_connection = False
            asyncio.create_task(self.flush_pending_checks(), name="Armada II pending checks")
            asyncio.create_task(self.report_goal_if_victorious(), name="Armada II goal status")
        elif cmd == "ReceivedItems":
            item_names = {code: name for name, code in item_name_to_id.items()}
            self.ledger.record_received_items(int(args.get("index", 0)), list(args.get("items", [])), item_names)
            threading.Thread(target=self.dispatch_pending_traps, daemon=True,
                             name="Armada II trap dispatcher").start()
            threading.Thread(target=self.apply_permanent_upgrades, daemon=True,
                             name="Armada II permanent upgrade adapter").start()
            threading.Thread(target=self.dispatch_pending_helpful, daemon=True,
                             name="Armada II helpful-item dispatcher").start()
            asyncio.create_task(self.report_goal_if_victorious(), name="Armada II goal status")

    async def report_goal_if_victorious(self) -> None:
        """Report completion only after the server routes the locked Victory item.

        The Rifts location grants Victory through normal Archipelago item
        delivery.  Submitting that location alone does not mark this player's
        slot complete, so retry after every connection and ReceivedItems packet
        until this connection accepts CLIENT_GOAL.
        """
        if (self._goal_report_in_progress or self._goal_reported_for_connection or
                not self.server or not self.ledger.item_count(VICTORY_ITEM)):
            return
        self._goal_report_in_progress = True
        try:
            await self.send_msgs([{"cmd": "StatusUpdate", "status": ClientStatus.CLIENT_GOAL}])
        except Exception:
            logger.exception("Could not report Armada II goal status; it will retry after reconnecting.")
        else:
            self._goal_reported_for_connection = True
            logger.debug("Reported Archipelago goal after receiving Armada II Victory.")
        finally:
            self._goal_report_in_progress = False

    def dispatch_pending_traps(self) -> None:
        """Queue each durable trap once, only after a routed campaign map is live."""
        with self._trap_dispatch_lock:
            module = self.active_mission_module
            if not module or self.active_game is None or self.active_game.poll() is not None:
                return
            commands = {
                "Engine Disruption": "apply_engine_disruption",
                "Weapons Malfunction": "apply_weapons_malfunction",
                "Sensor Blackout": "apply_sensor_blackout",
                "Warp Field Collapse": "apply_warp_field_collapse",
                "E.P.S. Conduit Rupture": "apply_eps_conduit_rupture",
                "Dilithium Loss": "apply_dilithium_loss",
                "Metal Loss": "apply_metal_loss",
            }
            for index, name in self.ledger.pending_traps(set(TRAP_ITEM_COUNTS)):
                if name == "Nebula Anomaly":
                    # Armada I's survivability weighting: Mutara 50%,
                    # Cerulean 30%, Metreon 12%, Radioactive 8%.
                    roll = index % 100
                    command = ("apply_nebula_mutara" if roll < 50 else
                               "apply_nebula_cerulean" if roll < 80 else
                               "apply_nebula_metreon" if roll < 92 else
                               "apply_nebula_radioactive")
                else:
                    command = commands.get(name)
                if not command:
                    continue
                try:
                    reply = send_control_request(command, timeout=3.0)
                except OSError as error:
                    logger.warning("Armada II trap %s remains queued locally: %s", name, error)
                    return
                if not reply.startswith("queued native trap"):
                    logger.warning("Armada II rejected trap %s: %s", name, reply)
                    return
                self.ledger.mark_trap_dispatched(index, module, command)

    def apply_permanent_upgrades(self) -> None:
        """Recalculate durable faction-local upgrade lines for this map.

        The ledger is the source of truth. This sends no item-consumption
        acknowledgement: a reconnect or a new campaign map simply reapplies
        the same counts to the newly created local team.
        """
        with self._upgrade_apply_lock:
            if (not self.active_mission_module or self.active_game is None or
                    self.active_game.poll() is not None):
                return
            counts = {name: min(20, self.ledger.item_count(name)) for name in PERMANENT_UPGRADES}
            session = (self.active_game.pid, self.active_mission_module,
                       tuple(counts[name] for name in PERMANENT_UPGRADES))
            if session == self._permanent_upgrade_session:
                return
            command = "apply_upgrade_counts {} {} {} {} {}".format(
                counts["Construction Efficiency"], counts["Shipyard Throughput"],
                counts["Weapon Damage"], counts["Impulse Speed"], counts["Shield Capacity"],
            )
            try:
                reply = send_control_request(command, timeout=3.0)
            except OSError as error:
                logger.warning("Armada II permanent upgrades await a live observer: %s", error)
                return
            if not reply.startswith("queued native permanent upgrades"):
                logger.warning("Armada II rejected permanent upgrades: %s", reply)
            else:
                self._permanent_upgrade_session = session
                logger.debug("Armada II queued permanent upgrades: %s", counts)

    def dispatch_pending_helpful(self) -> None:
        """Apply each received helpful item once after its campaign map is live."""
        with self._helpful_dispatch_lock:
            module = self.active_mission_module
            if not module or self.active_game is None or self.active_game.poll() is not None:
                return
            commands = {
                "Dilithium Cache": "apply_dilithium_cache",
                "Metal Cache": "apply_metal_cache",
                "Emergency Repairs": "apply_emergency_repairs",
                "Slipstream Drive": "apply_slipstream_drive",
            }
            for index, name in self.ledger.pending_helpful(set(HELPFUL_ITEM_COUNTS)):
                command = commands.get(name)
                if not command:
                    continue
                try:
                    reply = send_control_request(command, timeout=3.0)
                except OSError as error:
                    logger.warning("Armada II helpful item %s remains queued locally: %s", name, error)
                    return
                if not reply.startswith("queued native helpful effect"):
                    logger.warning("Armada II rejected helpful item %s: %s", name, reply)
                    return
                self.ledger.mark_helpful_dispatched(index, module, command)

    def _select_room_ledger(self, args: dict) -> None:
        """Isolate durable checks/items to the connected Archipelago slot.

        Received-item indices and location names are only unique inside a
        concrete room. Seed/team/slot alone is insufficient for local test
        rooms: the generator can intentionally reuse a seed number while the
        server address or slot data differs. Include both stable values before
        choosing the durable ledger, preventing cross-room item/check leaks.
        """
        seed_name = str(getattr(self, "seed_name", "") or "unknown-seed")
        team = int(args.get("team", 0))
        slot = int(args.get("slot", 0))
        slot_data = args.get("slot_data", {})
        try:
            slot_data_identity = json.dumps(slot_data, sort_keys=True, separators=(",", ":"), default=str)
        except (TypeError, ValueError):
            slot_data_identity = repr(slot_data)
        identity = f"{self.server}|{seed_name}|{team}|{slot}|{slot_data_identity}"
        if identity == self.ledger_identity:
            return
        digest = hashlib.sha256(identity.encode("utf-8")).hexdigest()[:16]
        self.ledger = ClientLedger(self.ledger_directory / f"ledger-{digest}.json")
        self.ledger_identity = identity
        logger.debug("Armada II selected isolated ledger %s for the connected room.", digest)

    async def server_auth(self, password_requested: bool = False) -> None:
        if password_requested and not self.password:
            await super().server_auth(password_requested)
        await self.get_username()
        await self.send_connect()

    def mission_lock_reasons(self, module: str) -> list[str]:
        module = module.casefold()
        mission = self.missions.get(module)
        if mission is None:
            return ["unknown campaign mission"]
        if module == "a2_borg10s.dsl":
            required = self.final_mission_requirements
        else:
            faction = str(mission["faction"])
            required = [f"{faction} Mission {int(mission['number'])} Access", FACTION_KEYS[faction]]
        reasons = [name for name in required if self.ledger.item_count(name) < 1]
        completed = self.ledger.submitted_mission_count(set(self.mission_locations.values()) or set(mission_location_by_module.values()))
        if module == "a2_borg10s.dsl" and completed < self.other_mission_completion_requirement:
            reasons.append(f"{self.other_mission_completion_requirement - completed} other mission completion(s)")
        return reasons

    def mission_snapshot(self) -> dict[str, Any]:
        if not self.server or not self.slot:
            return {"status": "Not connected.", "missions": []}
        rows = []
        order = {"Federation": 0, "Klingon": 1, "Borg": 2}
        for module, data in sorted(self.missions.items(), key=lambda pair: (order.get(str(pair[1].get("faction")), 9), pair[1].get("number", 0))):
            missing = self.mission_lock_reasons(module)
            rows.append({
                "label": f"{data.get('faction')} {data.get('number')}: {data.get('title', module)}",
                "status": "Available" if not missing else "Locked",
                "requires": "Ready" if not missing else ", ".join(missing),
                "available": not missing,
                "module": module,
            })
        return {"status": f"Connected as {self.auth or self.username or 'slot'}. {self.launch_status}", "missions": rows}

    def start_managed_mission(self, module: str) -> None:
        """Launch one AP-owned Armada II process without blocking the GUI."""
        logger.debug("Armada II mission launch requested for %s.", module)
        threading.Thread(target=self._start_managed_mission_worker, args=(module,), daemon=True,
                         name="Armada II mission launcher").start()

    def _start_managed_mission_worker(self, module: str) -> None:
        try:
            self._start_managed_mission_worker_inner(module)
        except Exception:
            logger.exception("Armada II mission launcher failed before a process could be routed.")
            self.launch_status = "Mission launch failed; see the client log for the exact stage."

    def _start_managed_mission_worker_inner(self, module: str) -> None:
        with self._launch_lock:
            logger.debug("Armada II launch worker entered for %s.", module)
            if self.active_game and self.active_game.poll() is None:
                logger.debug("Armada II launch rejected: managed PID %s is still active.", self.active_game.pid)
                self.launch_status = "Armada II is already running for the selected mission."
                return
            if self.mission_lock_reasons(module):
                logger.debug("Armada II launch rejected: %s is now locked: %s", module, self.mission_lock_reasons(module))
                self.launch_status = "Mission is locked."
                return
            route = route_for_module(module)
            if route is None:
                logger.debug("Armada II launch rejected: no native route for %s.", module)
                self.launch_status = "No pinned native route is available for that mission."
                return
            if self.game_root is None:
                logger.debug("Armada II launch rejected: game folder is not configured.")
                self.launch_status = "Armada II folder is not configured. Restart the client and select its install folder."
                return
            game = self.game_root / "Armada2.exe"
            injector = self.game_root / "armada2_injector.exe"
            observer = self.game_root / "armada2_observer.dll"
            missing = [path for path in (game, injector, observer) if not path.is_file()]
            if missing:
                logger.debug("Armada II launch rejected: helper(s) missing: %s", missing)
                self.launch_status = "Missing launcher helper: " + ", ".join(str(path) for path in missing)
                return
            try:
                logger.debug("Preparing Armada II startup-movie state.")
                self._suppressed_startup_binks = suppress_startup_binks(game.parent)
                logger.debug("Startup-movie state ready; %d file(s) renamed by this launch.", len(self._suppressed_startup_binks))
            except OSError as error:
                logger.exception("Armada II startup-movie preparation failed.")
                self.launch_status = f"Could not temporarily disable Armada II startup movies: {error}"
                return
            logger.debug("Starting Armada II executable: %s", game)
            process = subprocess.Popen([str(game)], cwd=game.parent)
            logger.debug("Started Armada II PID %s for %s.", process.pid, module)
            self.active_game = process
            self._build_telemetry_requested = False
            threading.Thread(target=self._restore_startup_binks_after_exit, args=(process,), daemon=True,
                             name="Armada II startup-movie restore").start()
            self.launch_status = "Starting Armada II and attaching the pinned observer..."
            time.sleep(0.75)
            result = run_hidden([str(injector), "--pid", str(process.pid), "--dll", str(observer)])
            if result.returncode:
                self._close_active_game(process.pid)
                detail = result.stderr.strip() or result.stdout.strip()
                self.launch_status = f"Observer injection failed: {detail or result.returncode}"
                return
            logger.debug("Pinned observer injected into Armada II PID %s.", process.pid)
            request = launch_request(route)
            deadline = time.monotonic() + 30.0
            while process.poll() is None and time.monotonic() < deadline:
                try:
                    reply = send_control_request(f"launch_map {request['bzn_filename']}", timeout=1.0)
                except OSError:
                    time.sleep(0.25)
                    continue
                if reply.startswith("queued native campaign controller route"):
                    self.launch_status = f"Launching {request['bzn_filename']} through the stock campaign selector."
                    return
                if "ui-thread unavailable" not in reply:
                    self._close_active_game(process.pid)
                    self.launch_status = f"Native mission launch was rejected: {reply}"
                    return
                time.sleep(0.25)
            self._close_active_game(process.pid)
            self.launch_status = "Armada II did not reach its campaign shell within 30 seconds."

    def dispatch_native_launch(self, module: str) -> str:
        """Request a pinned observer to confirm a stock selector row."""
        if self.mission_lock_reasons(module):
            return "Mission is locked."
        route = route_for_module(module)
        if route is None:
            return "Native controller mapping is not yet verified for this faction."
        request = launch_request(route)
        try:
            reply = send_control_request(f"launch_map {request['bzn_filename']}")
        except OSError as error:
            return f"Launcher is waiting for Armada II at the campaign mission selector: {error}"
        return f"{request['bzn_filename']}: {reply}"

    async def flush_pending_checks(self) -> None:
        """Send only disk-recorded checks, then acknowledge precisely those entries."""
        if not self.server:
            return
        names = self.ledger.pending_check_names()
        location_ids = [location_name_to_id[name] for name in names if name in location_name_to_id]
        submitted_names = [name for name in names if name in location_name_to_id]
        if location_ids:
            await self.send_msgs([{"cmd": "LocationChecks", "locations": location_ids}])
            self.ledger.mark_submitted(submitted_names)

    async def handle_observer_event(self, event: dict[str, Any]) -> None:
        """Translate a validated adapter event without ever accepting locked progress.

        The version-pinned named-pipe reader will call this method once the
        Armada II observer contract is proven.  Keeping the translation here
        lets the launcher and external starts share the same lock rules.
        """
        event_type = event.get("type")
        if event_type == "adapter_status":
            # Build detours are intentionally armed only after the native
            # campaign controller has accepted a map. This keeps launcher
            # startup independent of the still-experimental build boundary.
            if (event.get("mode") == "native_mission_filename_hit" and
                    self.active_game is not None and
                    self.active_game.poll() is None and
                    not self._build_telemetry_requested):
                self._build_telemetry_requested = True
                try:
                    reply = await asyncio.to_thread(send_control_request, "enable_build_telemetry", 3.0)
                    logger.debug("Armada II post-route build telemetry request: %s", reply)
                except OSError as error:
                    logger.warning("Armada II post-route build telemetry was not armed: %s", error)
            if event.get("mode") == "native_mission_filename_hit":
                self.active_mission_module = Path(str(event.get("mission_file", ""))).stem.casefold() + "s.dsl"
                threading.Thread(target=self.dispatch_pending_traps, daemon=True,
                                 name="Armada II trap dispatcher").start()
                threading.Thread(target=self.apply_permanent_upgrades, daemon=True,
                                 name="Armada II permanent upgrade adapter").start()
                threading.Thread(target=self.dispatch_pending_helpful, daemon=True,
                                 name="Armada II helpful-item dispatcher").start()
            return
        module = Path(str(event.get("mission_module") or event.get("caller_module", ""))).name.casefold()
        if self.mission_lock_reasons(module):
            logger.debug("Ignored Armada II event from locked mission %s.", module)
            return
        if event_type == "objective_complete":
            if event.get("initial") or not event.get("complete", False):
                return
            key = "|".join((module, Path(str(event.get("objective_file", ""))).name.casefold(), str(event.get("objective_index", ""))))
            location = self.objective_locations.get(key)
            if location:
                self.ledger.record_check(location, event)
        elif event_type == "mission_result":
            if event.get("result") != "success":
                logger.debug("Observed Armada II mission failure; no check sent.")
            else:
                # A verified success reconciles every configured objective for
                # this module before its completion location. Reconciliation is
                # intentionally never used to infer a future build check.
                for key, location in self.objective_locations.items():
                    if key.startswith(module + "|"):
                        reconciled = dict(event)
                        reconciled.update({"type": "mission_success_objective_reconcile", "objective_event_key": key})
                        self.ledger.record_check(location, reconciled)
                location = self.mission_locations.get(module)
                if location:
                    self.ledger.record_check(location, event)
        elif event_type == "build_complete":
            key = "|".join((module, str(event["odf"]).casefold()))
            location = self.build_locations.get(key)
            if location:
                self.ledger.record_check(location, event)
            else:
                logger.debug("Ignored build completion outside the mission-tech eligibility catalog: %s", key)
        else:
            return
        await self.flush_pending_checks()
        if event_type == "mission_result":
            await asyncio.to_thread(self._close_active_game, event.get("pid"))

    def _close_active_game(self, pid: object) -> None:
        """Close only the process that this client started for the mission."""
        try:
            expected_pid = int(pid)
        except (TypeError, ValueError):
            return
        process = self.active_game
        if process is None or process.pid != expected_pid:
            return
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
        self.active_game = None
        self._restore_startup_binks()
        self.launch_status = "Mission ended; return to Mission Launcher for the next selection."

    def _restore_startup_binks_after_exit(self, process: subprocess.Popen[bytes]) -> None:
        """Undo suppression if the managed game closes outside a result event."""
        process.wait()
        if self.active_game is process:
            self._restore_startup_binks()

    def _restore_startup_binks(self) -> None:
        try:
            restore_startup_binks(self._suppressed_startup_binks)
        except OSError as error:
            logger.warning("Could not restore Armada II startup movies: %s", error)
        self._suppressed_startup_binks = ()


def _observer_lines() -> Any:
    """Yield raw newline-delimited JSON messages from the future observer."""
    buffer = bytearray()
    while True:
        handle = kernel32.CreateFileW(OBSERVER_PIPE, GENERIC_READ, 0, None, OPEN_EXISTING, 0, None)
        if handle == INVALID_HANDLE_VALUE:
            if ctypes.get_last_error() in (ERROR_FILE_NOT_FOUND, ERROR_PIPE_BUSY):
                time.sleep(1)
                continue
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            while True:
                chunk = ctypes.create_string_buffer(4096)
                read = wintypes.DWORD()
                if not kernel32.ReadFile(handle, chunk, len(chunk), ctypes.byref(read), None):
                    if ctypes.get_last_error() == ERROR_BROKEN_PIPE:
                        break
                    raise ctypes.WinError(ctypes.get_last_error())
                buffer.extend(chunk.raw[:read.value])
                while b"\n" in buffer:
                    line, _, remainder = buffer.partition(b"\n")
                    buffer[:] = remainder
                    try:
                        yield json.loads(line.decode("utf-8"))
                    except (UnicodeDecodeError, json.JSONDecodeError):
                        # Native telemetry is untrusted at this boundary.  A
                        # malformed byte sequence must not terminate the
                        # long-lived observer reader or disable the launcher.
                        logger.warning("Ignored malformed Armada II observer JSON.")
        finally:
            kernel32.CloseHandle(handle)


def send_control_request(command: str, timeout: float = 15.0) -> str:
    """Send one bounded command to an already attached pinned observer.

    The observer exposes one control-pipe listener. Item receipt can wake the
    permanent, helpful, and trap dispatchers together, so serialize the full
    request/reply exchange and retry the listener-handoff window.
    """
    payload = (command.rstrip() + "\n").encode("ascii")
    deadline = time.monotonic() + timeout
    with CONTROL_REQUEST_LOCK:
        while True:
            handle = kernel32.CreateFileW(
                CONTROL_PIPE, GENERIC_READ | GENERIC_WRITE, 0, None, OPEN_EXISTING, 0, None
            )
            if handle == INVALID_HANDLE_VALUE:
                error = ctypes.get_last_error()
                if error in (ERROR_FILE_NOT_FOUND, ERROR_PIPE_BUSY, ERROR_PIPE_NOT_CONNECTED) and time.monotonic() < deadline:
                    time.sleep(0.1)
                    continue
                raise ctypes.WinError(error)
            try:
                written = wintypes.DWORD()
                if not kernel32.WriteFile(handle, payload, len(payload), ctypes.byref(written), None):
                    raise ctypes.WinError(ctypes.get_last_error())
                reply = ctypes.create_string_buffer(256)
                received = wintypes.DWORD()
                if not kernel32.ReadFile(handle, reply, len(reply), ctypes.byref(received), None):
                    raise ctypes.WinError(ctypes.get_last_error())
                return reply.raw[:received.value].decode("ascii", errors="replace").strip()
            except OSError as error:
                if error.winerror in (ERROR_PIPE_BUSY, ERROR_PIPE_NOT_CONNECTED, ERROR_BROKEN_PIPE) and time.monotonic() < deadline:
                    time.sleep(0.1)
                    continue
                raise
            finally:
                kernel32.CloseHandle(handle)


async def observe_adapter(ctx: ArmadaIIContext) -> None:
    """Consume only version-pinned observer events; the DLL remains deferred."""
    events: asyncio.Queue[dict[str, Any]] = asyncio.Queue()
    loop = asyncio.get_running_loop()

    def read_pipe() -> None:
        try:
            for raw in _observer_lines():
                event = validate_event(raw)
                if event is None:
                    logger.warning("Ignored unpinned or invalid Armada II observer event.")
                    continue
                loop.call_soon_threadsafe(events.put_nowait, event)
        except Exception:
            logger.exception("Armada II observer pipe reader stopped unexpectedly.")

    threading.Thread(target=read_pipe, name="Armada II observer pipe", daemon=True).start()
    while True:
        await ctx.handle_observer_event(await events.get())


async def run(connect: str | None, password: str | None, name: str | None, game_root: Path | None) -> None:
    ctx = ArmadaIIContext(connect, password, SETTINGS_PATH.parent / "ledgers", game_root)
    ctx.username = name
    ctx.server_task = asyncio.create_task(server_loop(ctx), name="server loop")
    if UNIVERSAL_TRACKER_AVAILABLE:
        ctx.run_generator()
    if gui_enabled:
        ctx.run_gui()
    ctx.run_cli()
    observer = asyncio.create_task(observe_adapter(ctx), name="Armada II observer")
    await ctx.exit_event.wait()
    observer.cancel()
    await asyncio.gather(observer, return_exceptions=True)


def launch(*args: str) -> None:
    Utils.init_logging("StarTrekArmadaIIClient")
    parser = get_base_parser()
    parser.add_argument("--name", default=None)
    parser.add_argument("--game-root", type=Path, default=None,
                        help="Armada II install folder; saved for later client launches.")
    parser.add_argument("url", nargs="?")
    parsed = handle_url_arg(parser.parse_args(args), parser=parser)
    game_root = parsed.game_root
    if game_root and (game_root / "Armada2.exe").is_file():
        _save_game_root(game_root)
    elif game_root:
        logger.warning("Ignoring --game-root because it does not contain Armada2.exe: %s", game_root)
        game_root = None
    game_root = game_root or _load_game_root()
    if game_root is None and gui_enabled:
        game_root = _prompt_for_game_root()
    asyncio.run(run(parsed.connect, parsed.password, parsed.name, game_root))
