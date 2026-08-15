// Armada II observer. It installs version-pinned observation detours only and
// journals native campaign objective-complete and mission-success events.

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {
constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\archipelago_armada2_observer_v1";
constexpr wchar_t kControlPipeName[] = L"\\\\.\\pipe\\archipelago_armada2_control_v1";
constexpr char kExpectedSha256[] = "c01ff40248bc4c711ea2cde60deda2b9862a8274b18b5537e618fa7b61957ae0";
constexpr DWORD kSucceedMissionRva = 0x4e930;
constexpr DWORD kFailMissionRva = 0x4e900;
constexpr DWORD kObjectiveCompleteRva = 0x57cc0;
// Script-facing bridge: filename at stack[1], boolean clear flag at stack[2].
// This executes only when a campaign script changes the displayed objective file.
constexpr DWORD kObjectivesTextFromFileRva = 0x57ca0;
constexpr DWORD kSetMissionFilenameRva = 0x89a70;
constexpr DWORD kSelectedMissionRva = 0x323cac;
constexpr DWORD kShellMissionOrdinalRva = 0x3a89ac;
constexpr DWORD kShellCampaignIndexRva = 0x3a89a4;
constexpr DWORD kShellFilenamesRva = 0x3a8afc;
constexpr DWORD kSetupMissionRva = 0x1dcc00;
constexpr DWORD kSetObjectivesFileRva = 0x10c9f0;
constexpr DWORD kParseObjectivesTextRva = 0x10d4d0;
constexpr DWORD kLoadRulesRva = 0x6480;
constexpr DWORD kLoadActionScriptsRva = 0x7b70;
constexpr DWORD kTechnologyParseRva = 0x99360;
// These are observation-only build seams for the pinned GOG executable.  A
// completion is never a check until its local-player discriminator is proven.
constexpr DWORD kProducerPushBuildQueueItemRva = 0xb7930;
constexpr DWORD kProducerFinishBuildRva = 0xb8f50;
constexpr DWORD kConstructionRigFinishBuildRva = 0xaff90;
constexpr DWORD kResearchStationFinishBuildRva = 0xba2f0;
constexpr DWORD kEntityGetUserTeamRva = 0xd0060;
constexpr DWORD kTeamAddUnitRva = 0x97ea0;
// Native script/object APIs used only by the bounded Nebula Anomaly test
// adapter. Each is separately signature-pinned in the local catalog.
constexpr DWORD kBuildObjectRva = 0x51990;
constexpr DWORD kEntityGetRva = 0xcfff0;
constexpr DWORD kEntityGetTransformRva = 0xcfd50;
constexpr DWORD kEntitySetTransformRva = 0xcfdf0;
constexpr DWORD kScriptRemoveObjectRva = 0x55770;
constexpr DWORD kCraftListRva = 0x340b80;
constexpr DWORD kCraftDisableEnginesRva = 0xc9ef0;
constexpr DWORD kCraftDisableWeaponsRva = 0xca090;
constexpr DWORD kCraftDisableSensorsRva = 0xca110;
// Permanent-modifier adapter seams.  SetUpgradeModifier is the stock team
// multiplier table; UpdateCraft* refreshes already-existing local objects.
constexpr DWORD kTeamGetTeamRva = 0x96340;
constexpr DWORD kTeamSetUpgradeModifierRva = 0x987d0;
constexpr DWORD kTeamUpdateCraftEnginesRva = 0x98230;
constexpr DWORD kTeamUpdateCraftShieldsRva = 0x98310;
constexpr DWORD kTeamAddDilithiumRva = 0x96e30;
constexpr DWORD kTeamAddMetalRva = 0x97010;
constexpr DWORD kCraftRepairShipRva = 0xc94a0;
constexpr DWORD kCraftRegenerateShieldsRva = 0xc75a0;
// This is the exact final `fstp` after Producer::StartBuild has initialized
// its three active-build timers (+25c, +260, +264).
constexpr DWORD kProducerStartBuildTimerRva = 0xb8457;
constexpr std::array<unsigned char, 16> kSucceedMissionSignature = {
    0x55, 0x8b, 0xec, 0xa1, 0xe4, 0x5b, 0x73, 0x00, 0x8b, 0x4d, 0x08, 0xc6, 0x80, 0xd4, 0x00, 0x00};
constexpr DWORD kPipeAccessOutbound = 0x00000002;
constexpr DWORD kErrorPipeConnected = 535;
constexpr UINT kUiProbeMessage = WM_APP + 0x2A;
constexpr UINT kNativeLaunchMessage = WM_APP + 0x2B;
constexpr UINT kNebulaAnomalyMessage = WM_APP + 0x2C;
constexpr UINT kStatusTrapMessage = WM_APP + 0x2D;
constexpr UINT kPermanentUpgradesMessage = WM_APP + 0x2E;
constexpr UINT kHelpfulEffectMessage = WM_APP + 0x2F;
constexpr UINT_PTR kNativeLaunchRetryTimer = 0xA22;
constexpr UINT_PTR kNebulaAnomalyTimer = 0xA23;
constexpr UINT_PTR kWarpBurstTimer = 0xA24;
constexpr ULONGLONG kNebulaAnomalyDurationMs = 15'000;
constexpr ULONGLONG kHazardousNebulaDurationMs = 3'000;
constexpr float kStatusTrapDurationSeconds = 20.0f;
constexpr float kResourceCacheAmount = 150.0f;
constexpr float kEmergencyRepairSeconds = 8.0f;
constexpr float kWarpBurstMultiplier = 1.5f;
constexpr float kWarpFieldCollapseMultiplier = 0.5f;
constexpr ULONGLONG kWarpBurstDurationMs = 20'000;
constexpr SIZE_T kHookBytes = 8;
constexpr SIZE_T kObjectiveHookBytes = 9;
void* g_succeed_trampoline{};
volatile LONG g_succeed_pending{};
volatile ULONG_PTR g_succeed_return_address{};
std::array<ULONG_PTR, 48> g_succeed_stack{};
void* g_fail_trampoline{};
volatile LONG g_fail_pending{};
volatile ULONG_PTR g_fail_return_address{};
std::array<ULONG_PTR, 48> g_fail_stack{};
void* g_objective_trampoline{};
volatile LONG g_objective_pending{};
volatile ULONG_PTR g_objective_return_address{}, g_objective_arg1{}, g_objective_arg2{};
void* g_file_trampoline{}; volatile LONG g_file_pending{}; char g_objective_file[MAX_PATH]{};
void* g_parse_trampoline{}; volatile LONG g_parse_pending{}; char g_parsed_objective_file[MAX_PATH]{};
void* g_mission_file_trampoline{}; volatile LONG g_mission_file_pending{}; char g_mission_file[MAX_PATH]{};
void* g_rules_trampoline{}, *g_scripts_trampoline{}, *g_tech_trampoline{};
volatile LONG g_rules_pending{}, g_scripts_pending{}, g_tech_pending{};
char g_rules_file[MAX_PATH]{}, g_scripts_file[MAX_PATH]{}, g_tech_file[MAX_PATH]{};
void* g_build_queue_trampoline{}, *g_producer_finish_trampoline{}, *g_rig_finish_trampoline{}, *g_research_finish_trampoline{};
void* g_team_add_unit_trampoline{};
void* g_producer_start_build_timer_return{};
volatile LONG g_build_telemetry_armed{};
volatile LONG g_production_timing_hook_armed{};
// A single global queue/completion pair loses legitimate player builds when
// construction and shipyard production overlap.  Retain a bounded route for
// each producer and a separate completed-event ring; no AI route is admitted.
struct BuildRoute {
    ULONG_PTR producer{}, queued_class{}, completed_class{}, team_list{};
    LONG team{}, queued_count{};
    LONG state{}; // 0 empty, 1 locally queued, 2 awaiting Team::AddUnit
    char odf[MAX_PATH]{};
};
struct BuildEvent {
    ULONG_PTR producer{}, queued_class{}, completed_class{};
    LONG team{};
    char odf[MAX_PATH]{};
};
std::array<BuildRoute, 16> g_build_routes{};
std::array<BuildEvent, 32> g_completed_builds{};
size_t g_completed_build_head{}, g_completed_build_tail{}, g_completed_build_count{};
CRITICAL_SECTION g_build_lock{};
volatile LONG g_build_lock_ready{};
// The event pipe is a durable in-process handoff: hooks append immediately,
// while one worker holds/reconnects the outbound pipe.  A short client pipe
// reconnect must never discard a verified objective/build/result event.
std::deque<std::string> g_event_queue{};
CRITICAL_SECTION g_event_lock{};
volatile LONG g_event_lock_ready{};
HMODULE g_observer_module{};
HHOOK g_ui_dispatch_hook{};
volatile DWORD g_ui_thread{};
char g_pending_launch_map[MAX_PATH]{};
volatile LONG g_launch_in_progress{};
volatile LONG g_launch_retry_count{};
volatile LONG g_nebula_anomaly_in_progress{};
int g_nebula_anomaly_object_id{};
ULONGLONG g_nebula_anomaly_deadline{};
ULONGLONG g_nebula_anomaly_duration_ms{kNebulaAnomalyDurationMs};
char g_nebula_anomaly_odf[32]{"mnebula9"};
struct PermanentUpgradeCounts {
    int construction{}, shipyard{}, weapon{}, impulse{}, shield{};
};
PermanentUpgradeCounts g_permanent_upgrade_counts{};
volatile LONG g_warp_burst_active{};
ULONGLONG g_warp_burst_deadline{};
volatile LONG g_warp_collapse_active{};
ULONGLONG g_warp_collapse_deadline{};

using SetupMissionFn = bool(__cdecl*)(HWND, int*);
struct Matrix34 { float values[12]; };
using EntityGetFn = void*(__cdecl*)(int);
using EntityGetTransformFn = const Matrix34*(__thiscall*)(void*);
using EntitySetTransformFn = void(__thiscall*)(void*, const Matrix34*);
using BuildObjectFn = void*(__cdecl*)(const char*, int, const Matrix34*);
using ScriptRemoveObjectFn = void(__thiscall*)(void*, int);
using CraftTimedDisableFn = void(__thiscall*)(void*, float);
using TeamGetTeamFn = void*(__cdecl*)(int);
using TeamSetUpgradeModifierFn = void(__thiscall*)(void*, int, int, float);
using TeamUpdateCraftsFn = void(__cdecl*)(int);
using TeamAddResourceFn = void(__thiscall*)(void*, float);
using CraftTimedRepairFn = void(__thiscall*)(void*, float);

void capture_filename(char* destination, volatile LONG* pending, uintptr_t* stack);
void capture_msvc_string(char* destination, volatile LONG* pending, uintptr_t* stack);
bool install_string_hook(DWORD rva, const unsigned char* signature, SIZE_T length, void* hook, void** trampoline_out);
bool install_build_telemetry_hooks();
std::string json_escape(const std::string& text);
std::string active_mission_module();
using EntityGetUserTeamFn = int(__cdecl*)();
void start_nebula_anomaly();
void update_nebula_anomaly();
void start_status_trap(WPARAM trap_type);
void apply_permanent_upgrades();
bool install_production_timing_hook();
void apply_helpful_effect(WPARAM effect_type);
void update_warp_burst();
void observer_event_worker();

std::string sha256_file(const std::wstring& path) {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_HASH_HANDLE hash{};
    DWORD object_size{}, bytes{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &bytes, 0) < 0) return {};
    std::vector<unsigned char> object(object_size), digest(32), buffer(64 * 1024);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) < 0) return {};
    std::ifstream input(path, std::ios::binary);
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), buffer.size());
        const auto count = input.gcount();
        if (count > 0 && BCryptHashData(hash, buffer.data(), static_cast<ULONG>(count), 0) < 0) return {};
    }
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return {};
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    std::ostringstream text;
    for (const auto byte : digest) text << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
    return text.str();
}

void report_status(const std::string& json) {
    wchar_t program_data[MAX_PATH]{};
    const DWORD size = GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH);
    std::ofstream log(std::wstring(program_data, size) + L"\\Archipelago\\logs\\StarTrekArmadaIIObserver.jsonl", std::ios::app | std::ios::binary);
    log << json << '\n';
    if (!InterlockedCompareExchange(&g_event_lock_ready, 0, 0)) return;
    EnterCriticalSection(&g_event_lock);
    g_event_queue.push_back(json + "\n");
    LeaveCriticalSection(&g_event_lock);
}

void observer_event_worker() {
    while (true) {
        const HANDLE pipe = CreateNamedPipeW(kPipeName, kPipeAccessOutbound, PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return;
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != kErrorPipeConnected) {
            CloseHandle(pipe);
            Sleep(50);
            continue;
        }
        bool connected = true;
        while (connected) {
            std::string line;
            EnterCriticalSection(&g_event_lock);
            if (!g_event_queue.empty()) line = g_event_queue.front();
            LeaveCriticalSection(&g_event_lock);
            if (line.empty()) {
                Sleep(10);
                continue;
            }
            DWORD written{};
            if (!WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) || written != line.size()) {
                connected = false;
                continue;
            }
            EnterCriticalSection(&g_event_lock);
            if (!g_event_queue.empty() && g_event_queue.front() == line) g_event_queue.pop_front();
            LeaveCriticalSection(&g_event_lock);
        }
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
}

struct WindowSearch { DWORD pid; HWND window; };
BOOL CALLBACK find_game_window(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    if (pid != search->pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr) return TRUE;
    wchar_t class_name[32]{};
    GetClassNameW(window, class_name, MAX_PATH);
    if (_wcsicmp(class_name, L"Armada2") == 0) { search->window = window; return FALSE; }
    return TRUE;
}

HWND find_game_window() {
    WindowSearch search{GetCurrentProcessId(), nullptr};
    EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&search));
    return search.window;
}

struct ModalDialogSearch { HWND dialog{}; };
BOOL CALLBACK find_enabled_modal_dialog(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<ModalDialogSearch*>(parameter);
    wchar_t class_name[16]{};
    if (IsWindowEnabled(window) &&
        GetClassNameW(window, class_name, ARRAYSIZE(class_name)) > 0 &&
        wcscmp(class_name, L"#32770") == 0) {
        search->dialog = window;
        return FALSE;
    }
    return TRUE;
}

HWND find_enabled_modal_dialog() {
    ModalDialogSearch search{};
    EnumThreadWindows(GetCurrentThreadId(), find_enabled_modal_dialog, reinterpret_cast<LPARAM>(&search));
    return search.dialog;
}

bool is_mission_selector_dialog(HWND dialog) {
    const HWND owner = GetWindow(dialog, GW_OWNER);
    wchar_t owner_class[16]{};
    return owner && GetClassNameW(owner, owner_class, ARRAYSIZE(owner_class)) > 0 &&
           wcscmp(owner_class, L"#32770") == 0;
}

void schedule_native_launch_retry() {
    const HWND window = find_game_window();
    if (!window || InterlockedIncrement(&g_launch_retry_count) > 120) {
        if (window) KillTimer(window, kNativeLaunchRetryTimer);
        InterlockedExchange(&g_launch_in_progress, 0);
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"native_launch_rejected_shell_timeout\",\"pinned\":true}");
        return;
    }
    SetTimer(window, kNativeLaunchRetryTimer, 250, nullptr);
}

void remove_nebula_anomaly(const char* reason) {
    const auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    const int object_id = g_nebula_anomaly_object_id;
    if (image && object_id) {
        // ScriptInterfaceImp::RemoveObject has no instance-field access in the
        // pinned implementation.  It resolves this entity ID and executes the
        // stock object-expiry/remove sequence; a null unused `this` is safe.
        reinterpret_cast<ScriptRemoveObjectFn>(image + kScriptRemoveObjectRva)(nullptr, object_id);
    }
    g_nebula_anomaly_object_id = 0;
    g_nebula_anomaly_deadline = 0;
    g_nebula_anomaly_duration_ms = kNebulaAnomalyDurationMs;
    strcpy_s(g_nebula_anomaly_odf, "mnebula9");
    InterlockedExchange(&g_nebula_anomaly_in_progress, 0);
    const HWND window = find_game_window();
    if (window) KillTimer(window, kNebulaAnomalyTimer);
    report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"nebula_anomaly_removed\",\"reason\":\"" +
                  json_escape(reason) + "\",\"pinned\":true}");
}

void* find_local_named_hero(const unsigned char* image, int local_team) {
    const auto list = *reinterpret_cast<uintptr_t const*>(image + kCraftListRva);
    if (!list) return nullptr;
    const auto begin = *reinterpret_cast<uintptr_t const*>(list + 4);
    const auto end = *reinterpret_cast<uintptr_t const*>(list + 8);
    if (!begin || end < begin || end - begin > 4096) return nullptr;
    for (auto entry = begin; entry < end; entry += sizeof(uintptr_t)) {
        const auto craft = *reinterpret_cast<void* const*>(entry);
        if (!craft || *reinterpret_cast<const LONG*>(reinterpret_cast<const unsigned char*>(craft) + 0xec) != local_team ||
            *reinterpret_cast<const unsigned char*>(reinterpret_cast<const unsigned char*>(craft) + 0x113)) continue;
        const char* name = *reinterpret_cast<const char* const*>(reinterpret_cast<const unsigned char*>(craft) + 0x21c);
        if (!name) continue;
        if (strcmp(name, "Martok's Negh'Var") == 0 || strcmp(name, "U.S.S. Enterprise") == 0 ||
            strcmp(name, "Borg Queen's Diamond") == 0) return craft;
    }
    return nullptr;
}

void start_nebula_anomaly() {
    const auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return;
    const int local_team = reinterpret_cast<EntityGetUserTeamFn>(const_cast<unsigned char*>(image) + kEntityGetUserTeamRva)();
    void* hero = find_local_named_hero(image, local_team);
    if (!hero) {
        InterlockedExchange(&g_nebula_anomaly_in_progress, 0);
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"nebula_anomaly_rejected_no_named_local_hero\",\"pinned\":true}");
        return;
    }
    const auto transform = reinterpret_cast<EntityGetTransformFn>(const_cast<unsigned char*>(image) + kEntityGetTransformRva)(hero);
    if (!transform) {
        InterlockedExchange(&g_nebula_anomaly_in_progress, 0);
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"nebula_anomaly_rejected_no_transform\",\"pinned\":true}");
        return;
    }
    const Matrix34 initial = *transform;
    void* nebula = reinterpret_cast<BuildObjectFn>(const_cast<unsigned char*>(image) + kBuildObjectRva)(g_nebula_anomaly_odf, local_team, &initial);
    if (!nebula) {
        InterlockedExchange(&g_nebula_anomaly_in_progress, 0);
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"nebula_anomaly_rejected_build_failed\",\"pinned\":true}");
        return;
    }
    g_nebula_anomaly_object_id = *reinterpret_cast<const int*>(reinterpret_cast<const unsigned char*>(nebula) + 0x28);
    if (!g_nebula_anomaly_object_id) {
        InterlockedExchange(&g_nebula_anomaly_in_progress, 0);
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"nebula_anomaly_rejected_no_object_id\",\"pinned\":true}");
        return;
    }
    g_nebula_anomaly_deadline = GetTickCount64() + g_nebula_anomaly_duration_ms;
    const HWND window = find_game_window();
    if (!window || !SetTimer(window, kNebulaAnomalyTimer, 250, nullptr)) {
        remove_nebula_anomaly("timer_unavailable");
        return;
    }
    report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"nebula_anomaly_started\",\"odf\":\"" +
                  json_escape(g_nebula_anomaly_odf) + "\",\"object_id\":" + std::to_string(g_nebula_anomaly_object_id) +
                  ",\"duration_ms\":" + std::to_string(g_nebula_anomaly_duration_ms) + ",\"pinned\":true}");
}

void update_nebula_anomaly() {
    if (!InterlockedCompareExchange(&g_nebula_anomaly_in_progress, 0, 0)) return;
    if (GetTickCount64() >= g_nebula_anomaly_deadline) { remove_nebula_anomaly("expired"); return; }
    const auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) { remove_nebula_anomaly("image_unavailable"); return; }
    const int local_team = reinterpret_cast<EntityGetUserTeamFn>(const_cast<unsigned char*>(image) + kEntityGetUserTeamRva)();
    void* hero = find_local_named_hero(image, local_team);
    void* nebula = reinterpret_cast<EntityGetFn>(const_cast<unsigned char*>(image) + kEntityGetRva)(g_nebula_anomaly_object_id);
    if (!hero || !nebula) { remove_nebula_anomaly("hero_or_nebula_unavailable"); return; }
    const auto transform = reinterpret_cast<EntityGetTransformFn>(const_cast<unsigned char*>(image) + kEntityGetTransformRva)(hero);
    if (!transform) { remove_nebula_anomaly("hero_transform_unavailable"); return; }
    const Matrix34 current = *transform;
    reinterpret_cast<EntitySetTransformFn>(const_cast<unsigned char*>(image) + kEntitySetTransformRva)(nebula, &current);
}

void start_status_trap(WPARAM trap_type) {
    const auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return;
    const int local_team = reinterpret_cast<EntityGetUserTeamFn>(const_cast<unsigned char*>(image) + kEntityGetUserTeamRva)();
    DWORD function_rva{};
    const char* name{};
    switch (trap_type) {
    case 1: function_rva = kCraftDisableEnginesRva; name = "engine_disruption"; break;
    case 2: function_rva = kCraftDisableWeaponsRva; name = "weapons_malfunction"; break;
    case 3: function_rva = kCraftDisableSensorsRva; name = "sensor_blackout"; break;
    default:
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"status_trap_rejected_unknown_type\",\"pinned\":true}");
        return;
    }
    // Craft::craftList contains both mobile craft and stations. The pinned
    // CraftClass::isStation field (+0x20d) is the engine's own discriminator:
    // target every non-station craft on the current player's team, never a
    // hero-only subset and never AI, neutral, or player-owned stations.
    const auto list = *reinterpret_cast<uintptr_t const*>(image + kCraftListRva);
    if (!list) {
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"status_trap_rejected_no_craft_list\",\"pinned\":true}");
        return;
    }
    const auto begin = *reinterpret_cast<uintptr_t const*>(list + 4);
    const auto end = *reinterpret_cast<uintptr_t const*>(list + 8);
    if (!begin || end < begin || end - begin > 4096) {
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"status_trap_rejected_invalid_craft_list\",\"pinned\":true}");
        return;
    }
    const auto disable = reinterpret_cast<CraftTimedDisableFn>(const_cast<unsigned char*>(image) + function_rva);
    size_t affected{};
    for (auto entry = begin; entry < end; entry += sizeof(uintptr_t)) {
        void* craft = *reinterpret_cast<void* const*>(entry);
        if (!craft || *reinterpret_cast<const LONG*>(reinterpret_cast<const unsigned char*>(craft) + 0xec) != local_team ||
            *reinterpret_cast<const unsigned char*>(reinterpret_cast<const unsigned char*>(craft) + 0x113)) continue;
        const auto craft_class = *reinterpret_cast<void* const*>(reinterpret_cast<const unsigned char*>(craft) + 0x40);
        if (!craft_class || *reinterpret_cast<const unsigned char*>(reinterpret_cast<const unsigned char*>(craft_class) + 0x20d)) continue;
        disable(craft, kStatusTrapDurationSeconds);
        ++affected;
    }
    if (!affected) {
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"status_trap_rejected_no_local_ships\",\"pinned\":true}");
        return;
    }
    report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"status_trap_applied\",\"trap\":\"" +
                  std::string(name) + "\",\"affected_ship_count\":" + std::to_string(affected) +
                  ",\"duration_seconds\":20,\"pinned\":true}");
}

int clamped_upgrade_count(int value) {
    return value < 0 ? 0 : (value > 20 ? 20 : value);
}

float upgrade_multiplier(int count) {
    return 1.0f + 0.05f * static_cast<float>(clamped_upgrade_count(count));
}

bool producer_odf_has_prefix(const void* producer, const char* prefix) {
    if (!producer) return false;
    const auto producer_class = *reinterpret_cast<void* const*>(reinterpret_cast<const unsigned char*>(producer) + 0x40);
    if (!producer_class) return false;
    const auto odf = *reinterpret_cast<const char* const*>(reinterpret_cast<const unsigned char*>(producer_class) + 0x7c);
    return odf && _strnicmp(odf, prefix, strlen(prefix)) == 0;
}

extern "C" int scale_producer_start_build_timers(void* producer) {
    // Called after the stock start routine has populated all three timers.
    // The owner check is mandatory: campaign AI, neutral, and story producers
    // remain entirely stock.  Source ODF classification excludes research
    // queue timing: only construction rigs and shipyards are adjusted.
    __try {
        const auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
        if (!image || !producer) return 0;
        const int local_team = reinterpret_cast<EntityGetUserTeamFn>(image + kEntityGetUserTeamRva)();
        if (*reinterpret_cast<const LONG*>(reinterpret_cast<const unsigned char*>(producer) + 0xec) != local_team) return 0;
        const bool construction = producer_odf_has_prefix(producer, "fconst") ||
                                  producer_odf_has_prefix(producer, "kconst") ||
                                  producer_odf_has_prefix(producer, "bconst") ||
                                  producer_odf_has_prefix(producer, "cconst") ||
                                  producer_odf_has_prefix(producer, "rconst") ||
                                  producer_odf_has_prefix(producer, "mconst") ||
                                  producer_odf_has_prefix(producer, "sconst") ||
                                  producer_odf_has_prefix(producer, "const");
        const bool shipyard = producer_odf_has_prefix(producer, "fyard") ||
                              producer_odf_has_prefix(producer, "kyard") ||
                              producer_odf_has_prefix(producer, "byard") ||
                              producer_odf_has_prefix(producer, "cyard") ||
                              producer_odf_has_prefix(producer, "ryard");
        const int count = construction ? g_permanent_upgrade_counts.construction :
                          (shipyard ? g_permanent_upgrade_counts.shipyard : 0);
        if (!count) return 0;
        const float factor = 1.0f / upgrade_multiplier(count);
        auto* bytes = reinterpret_cast<unsigned char*>(producer);
        for (const size_t offset : {SIZE_T{0x25c}, SIZE_T{0x260}, SIZE_T{0x264}}) {
            auto& value = *reinterpret_cast<float*>(bytes + offset);
            if (value > 0.0f && value < 36000.0f) value *= factor;
        }
        return construction ? 1 : 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

extern "C" void on_producer_start_build_timer_finalized(void* producer) {
    const int result = scale_producer_start_build_timers(producer);
    if (result < 0) {
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"production_time_scale_rejected\",\"pinned\":true}");
    } else if (result) {
        const int count = result == 1 ? g_permanent_upgrade_counts.construction : g_permanent_upgrade_counts.shipyard;
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"production_time_scaled\",\"line\":\"" +
                      std::string(result == 1 ? "construction" : "shipyard") + "\",\"multiplier\":" +
                      std::to_string(upgrade_multiplier(count)) + ",\"pinned\":true}");
    }
}

extern "C" __declspec(naked) void producer_start_build_timer_hook() {
    __asm {
        // Reproduce the six-byte overwritten stock instruction first, then
        // scale the initialized timers while all game registers are preserved.
        fstp dword ptr [esi+264h]
        pushfd
        pushad
        push esi
        call on_producer_start_build_timer_finalized
        add esp, 4
        popad
        popfd
        jmp dword ptr [g_producer_start_build_timer_return]
    }
}

bool install_production_timing_hook() {
    if (InterlockedCompareExchange(&g_production_timing_hook_armed, 0, 0)) return true;
    auto* target = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)) + kProducerStartBuildTimerRva;
    const unsigned char signature[] = {0xd9, 0x9e, 0x64, 0x02, 0x00, 0x00};
    if (memcmp(target, signature, sizeof(signature)) != 0) return false;
    DWORD old{};
    if (!VirtualProtect(target, sizeof(signature), PAGE_EXECUTE_READWRITE, &old)) return false;
    target[0] = 0xe9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(reinterpret_cast<unsigned char*>(&producer_start_build_timer_hook) - (target + 5));
    target[5] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, sizeof(signature));
    VirtualProtect(target, sizeof(signature), old, &old);
    g_producer_start_build_timer_return = target + sizeof(signature);
    InterlockedExchange(&g_production_timing_hook_armed, 1);
    return true;
}

void apply_permanent_upgrades() {
    const auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return;
    const int local_team = reinterpret_cast<EntityGetUserTeamFn>(const_cast<unsigned char*>(image) + kEntityGetUserTeamRva)();
    void* team = reinterpret_cast<TeamGetTeamFn>(const_cast<unsigned char*>(image) + kTeamGetTeamRva)(local_team);
    if (!team || !install_production_timing_hook()) {
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"permanent_upgrades_rejected\",\"pinned\":true}");
        return;
    }
    const auto set_modifier = reinterpret_cast<TeamSetUpgradeModifierFn>(const_cast<unsigned char*>(image) + kTeamSetUpgradeModifierRva);
    // category 2 is the local faction upgrade line. Modifier types map to the
    // engine fields used by combat/movement: shield=0, impulse=1, weapon=2.
    set_modifier(team, 2, 0, upgrade_multiplier(g_permanent_upgrade_counts.shield));
    set_modifier(team, 2, 1, upgrade_multiplier(g_permanent_upgrade_counts.impulse));
    set_modifier(team, 2, 2, upgrade_multiplier(g_permanent_upgrade_counts.weapon));
    reinterpret_cast<TeamUpdateCraftsFn>(const_cast<unsigned char*>(image) + kTeamUpdateCraftShieldsRva)(local_team);
    reinterpret_cast<TeamUpdateCraftsFn>(const_cast<unsigned char*>(image) + kTeamUpdateCraftEnginesRva)(local_team);
    report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"permanent_upgrades_applied\",\"construction\":" +
                  std::to_string(clamped_upgrade_count(g_permanent_upgrade_counts.construction)) + ",\"shipyard\":" +
                  std::to_string(clamped_upgrade_count(g_permanent_upgrade_counts.shipyard)) + ",\"weapon\":" +
                  std::to_string(clamped_upgrade_count(g_permanent_upgrade_counts.weapon)) + ",\"impulse\":" +
                  std::to_string(clamped_upgrade_count(g_permanent_upgrade_counts.impulse)) + ",\"shield\":" +
                  std::to_string(clamped_upgrade_count(g_permanent_upgrade_counts.shield)) + ",\"pinned\":true}");
}

void set_local_impulse_multiplier(const unsigned char* image, float multiplier) {
    const int local_team = reinterpret_cast<EntityGetUserTeamFn>(const_cast<unsigned char*>(image) + kEntityGetUserTeamRva)();
    void* team = reinterpret_cast<TeamGetTeamFn>(const_cast<unsigned char*>(image) + kTeamGetTeamRva)(local_team);
    if (!team) return;
    reinterpret_cast<TeamSetUpgradeModifierFn>(const_cast<unsigned char*>(image) + kTeamSetUpgradeModifierRva)(team, 2, 1, multiplier);
    reinterpret_cast<TeamUpdateCraftsFn>(const_cast<unsigned char*>(image) + kTeamUpdateCraftEnginesRva)(local_team);
}

void update_warp_burst() {
    const ULONGLONG now = GetTickCount64();
    bool changed{};
    if (InterlockedCompareExchange(&g_warp_burst_active, 0, 0) && now >= g_warp_burst_deadline) {
        InterlockedExchange(&g_warp_burst_active, 0); changed = true;
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"slipstream_drive_expired\",\"pinned\":true}");
    }
    if (InterlockedCompareExchange(&g_warp_collapse_active, 0, 0) && now >= g_warp_collapse_deadline) {
        InterlockedExchange(&g_warp_collapse_active, 0); changed = true;
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"warp_field_collapse_expired\",\"pinned\":true}");
    }
    if (!changed && (InterlockedCompareExchange(&g_warp_burst_active, 0, 0) ||
                     InterlockedCompareExchange(&g_warp_collapse_active, 0, 0))) return;
    const auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    float multiplier = upgrade_multiplier(g_permanent_upgrade_counts.impulse);
    if (InterlockedCompareExchange(&g_warp_burst_active, 0, 0)) multiplier *= kWarpBurstMultiplier;
    if (InterlockedCompareExchange(&g_warp_collapse_active, 0, 0)) multiplier *= kWarpFieldCollapseMultiplier;
    if (image) set_local_impulse_multiplier(image, multiplier);
    const HWND window = find_game_window();
    if (window && !InterlockedCompareExchange(&g_warp_burst_active, 0, 0) &&
        !InterlockedCompareExchange(&g_warp_collapse_active, 0, 0)) KillTimer(window, kWarpBurstTimer);
}

void apply_helpful_effect(WPARAM effect_type) {
    const auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!image) return;
    const int local_team = reinterpret_cast<EntityGetUserTeamFn>(const_cast<unsigned char*>(image) + kEntityGetUserTeamRva)();
    void* team = reinterpret_cast<TeamGetTeamFn>(const_cast<unsigned char*>(image) + kTeamGetTeamRva)(local_team);
    if (!team) return;
    if (effect_type == 1 || effect_type == 2 || effect_type == 7 || effect_type == 8) {
        const bool dilithium = effect_type == 1 || effect_type == 7;
        const bool loss = effect_type >= 7;
        const DWORD function_rva = dilithium ? kTeamAddDilithiumRva : kTeamAddMetalRva;
        // Stock typed resource mutators clamp their result to the legal
        // [0, configured maximum] range, so an empty stockpile never becomes
        // negative and resource UI/state stay synchronized.
        reinterpret_cast<TeamAddResourceFn>(const_cast<unsigned char*>(image) + function_rva)(team,
                                                                                                  loss ? -kResourceCacheAmount : kResourceCacheAmount);
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"resource_" +
                      std::string(loss ? "loss" : "cache") + "_applied\",\"resource\":\"" +
                      std::string(dilithium ? "dilithium" : "metal") + "\",\"amount\":" +
                      std::to_string(loss ? -150 : 150) + ",\"team\":" +
                      std::to_string(local_team) + ",\"pinned\":true}");
        return;
    }
    if (effect_type == 3 || effect_type == 6) {
        // Prefer the campaign hero; fall back to the first living local ship.
        void* target = find_local_named_hero(image, local_team);
        if (!target) {
            const auto list = *reinterpret_cast<uintptr_t const*>(image + kCraftListRva);
            const auto begin = list ? *reinterpret_cast<uintptr_t const*>(list + 4) : 0;
            const auto end = list ? *reinterpret_cast<uintptr_t const*>(list + 8) : 0;
            for (auto entry = begin; entry && entry < end; entry += sizeof(uintptr_t)) {
                void* craft = *reinterpret_cast<void* const*>(entry);
                if (craft && *reinterpret_cast<const LONG*>(reinterpret_cast<const unsigned char*>(craft) + 0xec) == local_team &&
                    !*reinterpret_cast<const unsigned char*>(reinterpret_cast<const unsigned char*>(craft) + 0x113)) { target = craft; break; }
            }
        }
        if (!target) {
            report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"" +
                          std::string(effect_type == 3 ? "emergency_repairs" : "eps_conduit_rupture") +
                          "_rejected_no_local_target\",\"pinned\":true}");
            return;
        }
        if (effect_type == 6) {
            // Version-pinned GameObject/Craft layout: current/max health at
            // +15c/+160, and current/max shields at +1c8/+1cc.  Clamp hull
            // at one percent of max so a received trap cannot instantly fail
            // a campaign before the player can react.
            auto* bytes = reinterpret_cast<unsigned char*>(target);
            auto& current_health = *reinterpret_cast<float*>(bytes + 0x15c);
            const float maximum_health = *reinterpret_cast<const float*>(bytes + 0x160);
            auto& health_ratio = *reinterpret_cast<float*>(bytes + 0x158);
            auto& current_shields = *reinterpret_cast<float*>(bytes + 0x1c8);
            const float maximum_shields = *reinterpret_cast<const float*>(bytes + 0x1cc);
            if (maximum_health > 0.0f) {
                current_health = (std::max)(maximum_health * 0.01f, current_health - maximum_health * 0.15f);
                health_ratio = current_health / maximum_health;
            }
            if (maximum_shields > 0.0f) current_shields = (std::max)(0.0f, current_shields - maximum_shields * 0.30f);
            report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"eps_conduit_rupture_applied\",\"team\":" +
                          std::to_string(local_team) + ",\"shield_damage_percent\":30,\"hull_damage_percent\":15,\"pinned\":true}");
            return;
        }
        reinterpret_cast<CraftTimedRepairFn>(const_cast<unsigned char*>(image) + kCraftRepairShipRva)(target, kEmergencyRepairSeconds);
        reinterpret_cast<CraftTimedRepairFn>(const_cast<unsigned char*>(image) + kCraftRegenerateShieldsRva)(target, kEmergencyRepairSeconds);
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"emergency_repairs_applied\",\"team\":" +
                      std::to_string(local_team) + ",\"pinned\":true}");
        return;
    }
    if (effect_type == 4 || effect_type == 5) {
        if (effect_type == 4) {
            g_warp_burst_deadline = GetTickCount64() + kWarpBurstDurationMs;
            InterlockedExchange(&g_warp_burst_active, 1);
        } else {
            g_warp_collapse_deadline = GetTickCount64() + kWarpBurstDurationMs;
            InterlockedExchange(&g_warp_collapse_active, 1);
        }
        float multiplier = upgrade_multiplier(g_permanent_upgrade_counts.impulse);
        if (InterlockedCompareExchange(&g_warp_burst_active, 0, 0)) multiplier *= kWarpBurstMultiplier;
        if (InterlockedCompareExchange(&g_warp_collapse_active, 0, 0)) multiplier *= kWarpFieldCollapseMultiplier;
        set_local_impulse_multiplier(image, multiplier);
        const HWND window = find_game_window();
        if (!window || !SetTimer(window, kWarpBurstTimer, 250, nullptr)) {
            if (effect_type == 4) InterlockedExchange(&g_warp_burst_active, 0);
            else InterlockedExchange(&g_warp_collapse_active, 0);
            set_local_impulse_multiplier(image, upgrade_multiplier(g_permanent_upgrade_counts.impulse));
            report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"speed_effect_rejected_timer\",\"pinned\":true}");
            return;
        }
        report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"" +
                      std::string(effect_type == 4 ? "slipstream_drive_applied" : "warp_field_collapse_applied") + "\",\"duration_seconds\":20,\"team\":" +
                      std::to_string(local_team) + ",\"pinned\":true}");
    }
}

LRESULT CALLBACK ui_dispatch_hook(int code, WPARAM, LPARAM parameter) {
    if (code >= 0) {
        auto* message = reinterpret_cast<MSG*>(parameter);
        if (message && message->message == kUiProbeMessage) {
            message->message = WM_NULL;
            report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"ui_thread_probe_hit\",\"ui_thread\":" + std::to_string(GetCurrentThreadId()) + ",\"pinned\":true}");
        } else if (message && (message->message == kNebulaAnomalyMessage ||
                               (message->message == WM_TIMER && message->wParam == kNebulaAnomalyTimer))) {
            const bool start_requested = message->message == kNebulaAnomalyMessage;
            message->message = WM_NULL;
            if (start_requested) start_nebula_anomaly();
            else update_nebula_anomaly();
        } else if (message && message->message == kStatusTrapMessage) {
            const WPARAM trap_type = message->wParam;
            message->message = WM_NULL;
            start_status_trap(trap_type);
        } else if (message && message->message == kPermanentUpgradesMessage) {
            message->message = WM_NULL;
            apply_permanent_upgrades();
        } else if (message && (message->message == kHelpfulEffectMessage ||
                               (message->message == WM_TIMER && message->wParam == kWarpBurstTimer))) {
            const WPARAM effect_type = message->wParam;
            message->message = WM_NULL;
            if (effect_type == kWarpBurstTimer) update_warp_burst();
            else apply_helpful_effect(effect_type);
        } else if (message && (message->message == kNativeLaunchMessage ||
                               (message->message == WM_TIMER && message->wParam == kNativeLaunchRetryTimer))) {
            message->message = WM_NULL;
            auto* image = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
            HWND dialog = find_enabled_modal_dialog();
            if (!image || !dialog) {
                schedule_native_launch_retry();
                return CallNextHookEx(g_ui_dispatch_hook, code, 0, parameter);
            }
            if (!is_mission_selector_dialog(dialog)) {
                // This is the stock Single Player shell. Re-enter its own
                // SetupMission path; the queued private message is consumed
                // by the nested mission selector and confirms the exact row.
                int selector_result{};
                if (!PostThreadMessageW(GetCurrentThreadId(), kNativeLaunchMessage, 0, 0)) {
                    InterlockedExchange(&g_launch_in_progress, 0);
                    report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"native_launch_rejected_selector_post_failed\",\"pinned\":true}");
                    return CallNextHookEx(g_ui_dispatch_hook, code, 0, parameter);
                }
                reinterpret_cast<SetupMissionFn>(image + kSetupMissionRva)(dialog, &selector_result);
                report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"native_selector_opened\",\"selector_result\":" + std::to_string(selector_result) + ",\"pinned\":true}");
                return CallNextHookEx(g_ui_dispatch_hook, code, 0, parameter);
            }
            // Resolve the request through the stock ShellSettings filename
            // table: ten rows per campaign. This avoids inventing a faction
            // index and mirrors MissionSelectDlgProc's selected-row state.
            int table_index = -1;
            for (int candidate = 0; candidate < 50; ++candidate) {
                const char* filename = *reinterpret_cast<const char* const*>(image + kShellFilenamesRva + candidate * sizeof(void*));
                if (filename && strcmp(filename, g_pending_launch_map) == 0) { table_index = candidate; break; }
            }
            if (table_index < 0) {
                report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"native_launch_rejected_map_not_in_selector\",\"mission_file\":\"" + json_escape(g_pending_launch_map) + "\",\"pinned\":true}");
                return CallNextHookEx(g_ui_dispatch_hook, code, 0, parameter);
            }
            const int campaign_index = table_index / 10;
            const unsigned char mission_ordinal = static_cast<unsigned char>(table_index % 10);
            *reinterpret_cast<unsigned char*>(image + kSelectedMissionRva) = mission_ordinal;
            *reinterpret_cast<unsigned char*>(image + kShellMissionOrdinalRva) = mission_ordinal;
            *reinterpret_cast<int32_t*>(image + kShellCampaignIndexRva) = campaign_index;
            EndDialog(dialog, 1);
            const HWND window = find_game_window();
            if (window) KillTimer(window, kNativeLaunchRetryTimer);
            report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"native_selector_confirmed\",\"mission_file\":\"" + json_escape(g_pending_launch_map) + "\",\"campaign_index\":" + std::to_string(campaign_index) + ",\"mission_ordinal\":" + std::to_string(mission_ordinal) + ",\"ui_thread\":" + std::to_string(GetCurrentThreadId()) + ",\"pinned\":true}");
        }
    }
    return CallNextHookEx(g_ui_dispatch_hook, code, 0, parameter);
}

bool ensure_ui_dispatch_hook(DWORD* thread_out) {
    const HWND window = find_game_window();
    if (!window) return false;
    const DWORD thread_id = GetWindowThreadProcessId(window, nullptr);
    if (!g_ui_dispatch_hook) {
        g_ui_dispatch_hook = SetWindowsHookExW(WH_GETMESSAGE, ui_dispatch_hook, g_observer_module, thread_id);
        if (!g_ui_dispatch_hook) return false;
    }
    g_ui_thread = thread_id;
    if (thread_out) *thread_out = thread_id;
    return true;
}

void control_worker() {
    while (true) {
        const HANDLE pipe = CreateNamedPipeW(kControlPipeName, PIPE_ACCESS_DUPLEX,
                                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                             1, 512, 512, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) return;
        if (!ConnectNamedPipe(pipe, nullptr) && GetLastError() != kErrorPipeConnected) { CloseHandle(pipe); continue; }
        char command[128]{}; DWORD read{}; const BOOL received = ReadFile(pipe, command, sizeof(command) - 1, &read, nullptr);
        command[received ? read : 0] = 0;
        const char* reply = "rejected unknown command\n";
        if (received && strcmp(command, "probe_ui_thread\n") == 0) {
            DWORD thread_id{};
            if (ensure_ui_dispatch_hook(&thread_id) && PostThreadMessageW(thread_id, kUiProbeMessage, 0, 0)) {
                reply = "queued ui-thread probe\n";
            } else {
                reply = "rejected ui-thread unavailable\n";
            }
        } else if (received && strncmp(command, "launch_map ", 11) == 0) {
            char* requested_map = command + 11;
            const size_t requested_length = strcspn(requested_map, "\r\n");
            requested_map[requested_length] = 0;
            DWORD thread_id{};
            if (InterlockedCompareExchange(&g_launch_in_progress, 1, 0) != 0) {
                reply = "rejected mission launch already pending\n";
            } else if (requested_length > 0 && requested_length < MAX_PATH && ensure_ui_dispatch_hook(&thread_id)) {
                strncpy_s(g_pending_launch_map, requested_map, _TRUNCATE);
                InterlockedExchange(&g_launch_retry_count, 0);
                if (PostThreadMessageW(thread_id, kNativeLaunchMessage, 0, 0)) reply = "queued native campaign controller route\n";
                else { InterlockedExchange(&g_launch_in_progress, 0); reply = "rejected native launch post failed\n"; }
            } else {
                InterlockedExchange(&g_launch_in_progress, 0);
                reply = "rejected ui-thread unavailable\n";
            }
        } else if (received && strcmp(command, "enable_build_telemetry\n") == 0) {
            if (InterlockedCompareExchange(&g_build_telemetry_armed, 1, 0) == 0) {
                if (install_build_telemetry_hooks()) {
                    reply = "build telemetry armed after mission route\n";
                } else {
                    InterlockedExchange(&g_build_telemetry_armed, 0);
                    reply = "rejected build telemetry signature mismatch\n";
                }
            } else {
                reply = "build telemetry already armed\n";
            }
        } else if (received && strncmp(command, "apply_upgrade_counts ", 21) == 0) {
            PermanentUpgradeCounts parsed{};
            if (sscanf_s(command + 21, "%d %d %d %d %d", &parsed.construction, &parsed.shipyard,
                         &parsed.weapon, &parsed.impulse, &parsed.shield) == 5) {
                g_permanent_upgrade_counts = parsed;
                DWORD thread_id{};
                if (ensure_ui_dispatch_hook(&thread_id) && PostThreadMessageW(thread_id, kPermanentUpgradesMessage, 0, 0)) {
                    reply = "queued native permanent upgrades\n";
                } else {
                    reply = "rejected permanent upgrades ui thread unavailable\n";
                }
            } else {
                reply = "rejected permanent upgrade counts\n";
            }
        } else if (received && strcmp(command, "test_full_permanent_upgrades\n") == 0) {
            // Diagnostic only: mirrors a player who has received all twenty
            // copies of every permanent upgrade line.
            g_permanent_upgrade_counts = {20, 20, 20, 20, 20};
            DWORD thread_id{};
            if (ensure_ui_dispatch_hook(&thread_id) && PostThreadMessageW(thread_id, kPermanentUpgradesMessage, 0, 0)) {
                reply = "queued native permanent upgrade full-stack test\n";
            } else {
                reply = "rejected permanent upgrades ui thread unavailable\n";
            }
        } else if (received && (strcmp(command, "test_dilithium_cache\n") == 0 ||
                                strcmp(command, "test_metal_cache\n") == 0 ||
                                strcmp(command, "test_emergency_repairs\n") == 0 ||
                                strcmp(command, "test_eps_conduit_rupture\n") == 0 ||
                                strcmp(command, "test_slipstream_drive\n") == 0 ||
                                strcmp(command, "test_warp_field_collapse\n") == 0 ||
                                strcmp(command, "test_dilithium_loss\n") == 0 ||
                                strcmp(command, "test_metal_loss\n") == 0 ||
                                strcmp(command, "apply_dilithium_cache\n") == 0 ||
                                strcmp(command, "apply_metal_cache\n") == 0 ||
                                strcmp(command, "apply_emergency_repairs\n") == 0 ||
                                strcmp(command, "apply_eps_conduit_rupture\n") == 0 ||
                                strcmp(command, "apply_slipstream_drive\n") == 0 ||
                                strcmp(command, "apply_warp_field_collapse\n") == 0 ||
                                strcmp(command, "apply_dilithium_loss\n") == 0 ||
                                strcmp(command, "apply_metal_loss\n") == 0)) {
            const WPARAM effect_type = strstr(command, "dilithium") ? 1 :
                                       (strstr(command, "metal") ? 2 :
                                       (strstr(command, "repairs") ? 3 : 4));
            const WPARAM selected_effect = strstr(command, "collapse") ? 5 :
                                           (strstr(command, "rupture") ? 6 :
                                           (strstr(command, "loss") ? (strstr(command, "dilithium") ? 7 : 8) : effect_type));
            DWORD thread_id{};
            if (ensure_ui_dispatch_hook(&thread_id) && PostThreadMessageW(thread_id, kHelpfulEffectMessage, selected_effect, 0)) {
                reply = selected_effect >= 5 ? "queued native trap effect\n" : "queued native helpful effect\n";
            } else {
                reply = "rejected helpful effect ui thread unavailable\n";
            }
        } else if (received && (strcmp(command, "test_nebula_anomaly\n") == 0 ||
                                strcmp(command, "test_nebula_radioactive\n") == 0 ||
                                strcmp(command, "test_nebula_metreon\n") == 0 ||
                                strcmp(command, "test_nebula_mutara\n") == 0 ||
                                strcmp(command, "test_nebula_cerulean\n") == 0 ||
                                strcmp(command, "apply_nebula_radioactive\n") == 0 ||
                                strcmp(command, "apply_nebula_metreon\n") == 0 ||
                                strcmp(command, "apply_nebula_mutara\n") == 0 ||
                                strcmp(command, "apply_nebula_cerulean\n") == 0)) {
            DWORD thread_id{};
            if (InterlockedCompareExchange(&g_nebula_anomaly_in_progress, 1, 0) != 0) {
                reply = "rejected nebula anomaly already active\n";
            } else {
                if (strstr(command, "radioactive")) strcpy_s(g_nebula_anomaly_odf, "mnebula6");
                else if (strstr(command, "metreon")) strcpy_s(g_nebula_anomaly_odf, "mnebula7");
                else if (strstr(command, "mutara")) strcpy_s(g_nebula_anomaly_odf, "mnebula8");
                else if (strstr(command, "cerulean")) strcpy_s(g_nebula_anomaly_odf, "mnebula10");
                else strcpy_s(g_nebula_anomaly_odf, "mnebula9");
                const bool production_trap = strncmp(command, "apply_", 6) == 0;
                if (!production_trap) {
                    g_nebula_anomaly_duration_ms = strcmp(g_nebula_anomaly_odf, "mnebula9") == 0 ?
                        kNebulaAnomalyDurationMs : kHazardousNebulaDurationMs;
                } else if (strcmp(g_nebula_anomaly_odf, "mnebula8") == 0 ||
                           strcmp(g_nebula_anomaly_odf, "mnebula10") == 0) {
                    g_nebula_anomaly_duration_ms = 20'000; // Mutara/Cerulean
                } else {
                    g_nebula_anomaly_duration_ms = 10'000; // Metreon/Radioactive
                }
                if (ensure_ui_dispatch_hook(&thread_id) && PostThreadMessageW(thread_id, kNebulaAnomalyMessage, 0, 0)) {
                reply = "queued native trap nebula anomaly\n";
                } else {
                    InterlockedExchange(&g_nebula_anomaly_in_progress, 0);
                    reply = "rejected nebula anomaly ui thread unavailable\n";
                }
            }
        } else if (received && (strcmp(command, "test_engine_disruption\n") == 0 ||
                                strcmp(command, "test_weapons_malfunction\n") == 0 ||
                                strcmp(command, "test_sensor_blackout\n") == 0 ||
                                strcmp(command, "apply_engine_disruption\n") == 0 ||
                                strcmp(command, "apply_weapons_malfunction\n") == 0 ||
                                strcmp(command, "apply_sensor_blackout\n") == 0)) {
            const WPARAM trap_type = strstr(command, "engine") ? 1 : (strstr(command, "weapons") ? 2 : 3);
            DWORD thread_id{};
            if (ensure_ui_dispatch_hook(&thread_id) && PostThreadMessageW(thread_id, kStatusTrapMessage, trap_type, 0)) {
                reply = "queued native trap status effect\n";
            } else {
                reply = "rejected status-trap ui thread unavailable\n";
            }
        }
        DWORD written{}; WriteFile(pipe, reply, static_cast<DWORD>(strlen(reply)), &written, nullptr);
        DisconnectNamedPipe(pipe); CloseHandle(pipe);
    }
}

std::string json_escape(const std::string& text) {
    std::ostringstream escaped;
    for (const unsigned char value : text) {
        switch (value) {
        case '\\': escaped << "\\\\"; break;
        case '"': escaped << "\\\""; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (value < 0x20) escaped << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(value);
            else escaped << static_cast<char>(value);
        }
    }
    return escaped.str();
}

std::string active_mission_module() {
    const std::string filename = std::filesystem::path(g_mission_file).filename().string();
    const auto dot = filename.rfind('.');
    if (dot == std::string::npos || _stricmp(filename.c_str() + dot, ".bzn") != 0) return {};
    // Stock campaign map a2_kling01.bzn is governed by a2_kling01s.dsl.
    // This mapping is derived from the native route already verified at load.
    return filename.substr(0, dot) + "s.dsl";
}

extern "C" void on_succeed_from_stack(uintptr_t* original_stack) {
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_succeed_return_address),
                               reinterpret_cast<PVOID>(*original_stack));
    for (SIZE_T i = 0; i < g_succeed_stack.size(); ++i) g_succeed_stack[i] = original_stack[i];
    InterlockedExchange(&g_succeed_pending, 1);
}

extern "C" __declspec(naked) void succeed_hook() {
    __asm {
        pushfd
        pushad
        lea eax, [esp+36]
        push eax
        call on_succeed_from_stack
        add esp, 4
        popad
        popfd
        jmp dword ptr [g_succeed_trampoline]
    }
}

extern "C" void on_fail_from_stack(uintptr_t* original_stack) {
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_fail_return_address),
                               reinterpret_cast<PVOID>(*original_stack));
    for (SIZE_T i = 0; i < g_fail_stack.size(); ++i) g_fail_stack[i] = original_stack[i];
    InterlockedExchange(&g_fail_pending, 1);
}

extern "C" __declspec(naked) void fail_hook() {
    __asm {
        pushfd
        pushad
        lea eax, [esp+36]
        push eax
        call on_fail_from_stack
        add esp, 4
        popad
        popfd
        jmp dword ptr [g_fail_trampoline]
    }
}

extern "C" void on_objective_from_stack(uintptr_t* stack) {
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_objective_return_address), reinterpret_cast<PVOID>(*stack));
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_objective_arg1), reinterpret_cast<PVOID>(stack[1]));
    InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_objective_arg2), reinterpret_cast<PVOID>(stack[2]));
    InterlockedExchange(&g_objective_pending, 1);
}
extern "C" __declspec(naked) void objective_hook() {
    __asm {
        pushfd
        pushad
        lea eax, [esp+36]
        push eax
        call on_objective_from_stack
        add esp, 4
        popad
        popfd
        jmp dword ptr [g_objective_trampoline]
    }
}
extern "C" void on_file_from_stack(uintptr_t* stack) {
    __try {
        strncpy_s(g_objective_file, reinterpret_cast<const char*>(stack[1]), _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_objective_file[0] = 0;
    }
    InterlockedExchange(&g_file_pending, 1);
}

extern "C" void on_parse_from_stack(uintptr_t* stack) {
    capture_filename(g_parsed_objective_file, &g_parse_pending, stack);
}

extern "C" void on_mission_file_from_stack(uintptr_t* stack) {
    capture_msvc_string(g_mission_file, &g_mission_file_pending, stack);
}

extern "C" __declspec(naked) void file_hook() {
    __asm {
        pushfd
        pushad
        lea eax, [esp+36]
        push eax
        call on_file_from_stack
        add esp, 4
        popad
        popfd
        jmp dword ptr [g_file_trampoline]
    }
}

extern "C" __declspec(naked) void parse_hook() {
    __asm {
        pushfd
        pushad
        lea eax, [esp+36]
        push eax
        call on_parse_from_stack
        add esp, 4
        popad
        popfd
        jmp dword ptr [g_parse_trampoline]
    }
}

extern "C" __declspec(naked) void mission_file_hook() {
    __asm {
        pushfd
        pushad
        lea eax, [esp+36]
        push eax
        call on_mission_file_from_stack
        add esp, 4
        popad
        popfd
        jmp dword ptr [g_mission_file_trampoline]
    }
}

// Producer::PushBuildQueueItem has `this` in ECX and the requested class as
// its first stack argument.  FinishBuild variants also retain `this` in ECX.
// We intentionally retain only a same-producer queue/completion pair; a
// global finish hook would confuse simultaneous AI production with the player.
extern "C" void on_build_queue(uintptr_t producer, uintptr_t* stack) {
    __try {
        const auto local_team = reinterpret_cast<EntityGetUserTeamFn>(
            reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)) + kEntityGetUserTeamRva)();
        if (*reinterpret_cast<const LONG*>(producer + 0xec) != local_team) return;
        if (!InterlockedCompareExchange(&g_build_lock_ready, 0, 0)) return;
        EnterCriticalSection(&g_build_lock);
        BuildRoute* route{};
        for (auto& candidate : g_build_routes) {
            if (candidate.state && candidate.producer == producer) { route = &candidate; break; }
            if (!route && !candidate.state) route = &candidate;
        }
        if (route) {
            if (!route->state) {
                *route = {};
                route->producer = producer;
                route->state = 1;
            }
            // A shipyard may accept several orders before the first finishes.
            // The engine's current build class supplies the completed identity;
            // this count preserves the normal-queue proof for every order.
            route->queued_class = stack[1];
            if (route->queued_count < 0x7fffffff) ++route->queued_count;
        }
        LeaveCriticalSection(&g_build_lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // The producer may be destroyed during a queue transition.  It cannot
        // yield a verified completion without a subsequent owned AddUnit.
    }
}

extern "C" void on_build_finish(uintptr_t producer) {
    if (!InterlockedCompareExchange(&g_build_lock_ready, 0, 0)) return;
    __try {
        EnterCriticalSection(&g_build_lock);
        BuildRoute* route{};
        for (auto& candidate : g_build_routes) {
            if (candidate.state == 1 && candidate.producer == producer && candidate.queued_count > 0) { route = &candidate; break; }
        }
        if (!route) { LeaveCriticalSection(&g_build_lock); return; }
        const auto build_class = *reinterpret_cast<const uintptr_t*>(producer + 0x254);
        // Current class is authoritative at the beginning of completion; the
        // queue argument is retained solely to establish its normal origin.
        if (!build_class || !route->queued_class) { LeaveCriticalSection(&g_build_lock); return; }
        // GameObjectClass::unitString is inline character storage at +0xd4,
        // not a char pointer.  Treating it as a pointer corrupted the first
        // telemetry sample even though producer correlation was correct.
        // The stable seed identity is the class SOD/ODF filename, not the
        // localised display string held inline at +0xd4.
        const auto odf = *reinterpret_cast<const char* const*>(build_class + 0x7c);
        if (!odf || strnlen(odf, MAX_PATH) >= MAX_PATH) { LeaveCriticalSection(&g_build_lock); return; }
        for (size_t index = 0; index < MAX_PATH && odf[index]; ++index) {
            const unsigned char value = static_cast<unsigned char>(odf[index]);
            if (!(value == '_' || value == '-' || (value >= '0' && value <= '9') ||
                  (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z'))) { LeaveCriticalSection(&g_build_lock); return; }
        }
        strncpy_s(route->odf, odf, _TRUNCATE);
        route->completed_class = build_class;
        route->team = *reinterpret_cast<const LONG*>(producer + 0xec);
        route->team_list = *reinterpret_cast<const uintptr_t*>(producer + 0xf0);
        --route->queued_count;
        // GameObjectClass::Construct calls SetTeam -> Team::AddUnit before
        // FinishBuild returns.  Wait for that exact ownership boundary rather
        // than reporting merely because a production timer expired.
        route->state = 2;
        LeaveCriticalSection(&g_build_lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedCompareExchange(&g_build_lock_ready, 0, 0)) LeaveCriticalSection(&g_build_lock);
        // A stale producer may be destroyed by the engine between queue and
        // completion; ignore it rather than dereferencing it again.
    }
}

extern "C" void on_team_add_unit(uintptr_t team, uintptr_t* stack) {
    if (!InterlockedCompareExchange(&g_build_lock_ready, 0, 0)) return;
    __try {
        const auto object = stack[1];
        if (!object) return;
        EnterCriticalSection(&g_build_lock);
        for (auto& route : g_build_routes) {
            if (route.state != 2 || team != route.team_list ||
                *reinterpret_cast<const uintptr_t*>(object + 0x40) != route.completed_class ||
                *reinterpret_cast<const LONG*>(object + 0xec) != route.team) continue;
            if (g_completed_build_count < g_completed_builds.size()) {
                auto& completed = g_completed_builds[g_completed_build_tail];
                completed = {route.producer, route.queued_class, route.completed_class, route.team, {}};
                strncpy_s(completed.odf, route.odf, _TRUNCATE);
                g_completed_build_tail = (g_completed_build_tail + 1) % g_completed_builds.size();
                ++g_completed_build_count;
            }
            route.completed_class = 0;
            route.team_list = 0;
            route.team = 0;
            route.odf[0] = '\0';
            if (route.queued_count > 0) route.state = 1;
            else route = {};
            break;
        }
        LeaveCriticalSection(&g_build_lock);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (InterlockedCompareExchange(&g_build_lock_ready, 0, 0)) LeaveCriticalSection(&g_build_lock);
    }
}

extern "C" __declspec(naked) void build_queue_hook() {
    __asm {
        pushfd
        pushad
        mov eax, [esp+24]
        lea edx, [esp+36]
        push edx
        push eax
        call on_build_queue
        add esp, 8
        popad
        popfd
        jmp dword ptr [g_build_queue_trampoline]
    }
}

extern "C" __declspec(naked) void producer_finish_hook() {
    __asm { pushfd
            pushad
            mov eax, [esp+24]
            push eax
            call on_build_finish
            add esp, 4
            popad
            popfd
            jmp dword ptr [g_producer_finish_trampoline] }
}
extern "C" __declspec(naked) void rig_finish_hook() {
    __asm { pushfd
            pushad
            mov eax, [esp+24]
            push eax
            call on_build_finish
            add esp, 4
            popad
            popfd
            jmp dword ptr [g_rig_finish_trampoline] }
}
extern "C" __declspec(naked) void research_finish_hook() {
    __asm { pushfd
            pushad
            mov eax, [esp+24]
            push eax
            call on_build_finish
            add esp, 4
            popad
            popfd
            jmp dword ptr [g_research_finish_trampoline] }
}
extern "C" __declspec(naked) void team_add_unit_hook() {
    __asm { pushfd
            pushad
            mov eax, [esp+24]
            lea edx, [esp+36]
            push edx
            push eax
            call on_team_add_unit
            add esp, 8
            popad
            popfd
            jmp dword ptr [g_team_add_unit_trampoline] }
}

bool install_file_hook() {
    auto* target = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)) + kSetObjectivesFileRva;
    const unsigned char signature[] = {0x55, 0x8b, 0xec, 0x56, 0x57, 0x8b, 0x7d, 0x08};
    if (memcmp(target, signature, sizeof(signature)) != 0) return false;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, 13, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, target, 8);
    trampoline[8] = 0xe9;
    *reinterpret_cast<int32_t*>(trampoline + 9) = static_cast<int32_t>((target + 8) - (trampoline + 13));
    DWORD old{};
    if (!VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    target[0] = 0xe9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(reinterpret_cast<unsigned char*>(&file_hook) - (target + 5));
    target[5] = target[6] = target[7] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, 8);
    VirtualProtect(target, 8, old, &old);
    g_file_trampoline = trampoline;
    return true;
}

bool install_objectives_text_from_file_hook() {
    const unsigned char signature[] = {0x55, 0x8b, 0xec, 0x8b, 0x45, 0x0c, 0x8b, 0x4d, 0x08};
    return install_string_hook(kObjectivesTextFromFileRva, signature, sizeof(signature),
                               reinterpret_cast<void*>(&file_hook), &g_file_trampoline);
}

bool install_mission_file_hook() {
    const unsigned char signature[] = {0x55,0x8b,0xec,0x6a,0xff,0x68,0x18,0xc6,0x69,0x00,0x64,0xa1,0x00,0x00,0x00,0x00};
    return install_string_hook(kSetMissionFilenameRva, signature, sizeof(signature),
                               reinterpret_cast<void*>(&mission_file_hook), &g_mission_file_trampoline);
}

bool install_parse_hook() {
    auto* target = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)) + kParseObjectivesTextRva;
    const unsigned char signature[] = {0x55,0x8b,0xec,0x6a,0xff,0x68,0x7f,0x02,0x6a,0x00,0x64,0xa1,0x00,0x00,0x00,0x00};
    return install_string_hook(kParseObjectivesTextRva, signature, sizeof(signature), reinterpret_cast<void*>(&parse_hook), &g_parse_trampoline);
}

void capture_filename(char* destination, volatile LONG* pending, uintptr_t* stack) {
    __try { strncpy_s(destination, MAX_PATH, reinterpret_cast<const char*>(stack[1]), _TRUNCATE); }
    __except (EXCEPTION_EXECUTE_HANDLER) { destination[0] = 0; }
    InterlockedExchange(pending, 1);
}

void capture_msvc_string(char* destination, volatile LONG* pending, uintptr_t* stack) {
    // At the SetMissionFilename entry, the by-value MSVC string begins at
    // stack[1]: byte flags, buffer pointer at stack[2], length at stack[3],
    // capacity at stack[4]. Do not use the generic C-string capture here.
    destination[0] = 0;
    __try {
        const auto source = reinterpret_cast<const char*>(stack[2]);
        const auto length = static_cast<SIZE_T>(stack[3]);
        if (source && length < MAX_PATH) {
            memcpy(destination, source, length);
            destination[length] = 0;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        destination[0] = 0;
    }
    InterlockedExchange(pending, 1);
}
extern "C" void on_rules_from_stack(uintptr_t* stack) { capture_filename(g_rules_file, &g_rules_pending, stack); }
extern "C" void on_scripts_from_stack(uintptr_t* stack) { capture_filename(g_scripts_file, &g_scripts_pending, stack); }
extern "C" void on_tech_from_stack(uintptr_t* stack) { capture_filename(g_tech_file, &g_tech_pending, stack); }
extern "C" __declspec(naked) void rules_hook() {
    __asm { pushfd
            pushad
            lea eax, [esp+36]
            push eax
            call on_rules_from_stack
            add esp, 4
            popad
            popfd
            jmp dword ptr [g_rules_trampoline] }
}
extern "C" __declspec(naked) void scripts_hook() {
    __asm { pushfd
            pushad
            lea eax, [esp+36]
            push eax
            call on_scripts_from_stack
            add esp, 4
            popad
            popfd
            jmp dword ptr [g_scripts_trampoline] }
}
extern "C" __declspec(naked) void tech_hook() {
    __asm { pushfd
            pushad
            lea eax, [esp+36]
            push eax
            call on_tech_from_stack
            add esp, 4
            popad
            popfd
            jmp dword ptr [g_tech_trampoline] }
}

bool install_string_hook(DWORD rva, const unsigned char* signature, SIZE_T length, void* hook, void** trampoline_out) {
    auto* target = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)) + rva;
    if (memcmp(target, signature, length) != 0) return false;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, length + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, target, length); trampoline[length] = 0xe9;
    *reinterpret_cast<int32_t*>(trampoline + length + 1) = static_cast<int32_t>((target + length) - (trampoline + length + 5));
    DWORD old{}; if (!VirtualProtect(target, length, PAGE_EXECUTE_READWRITE, &old)) return false;
    target[0] = 0xe9; *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(reinterpret_cast<unsigned char*>(hook) - (target + 5));
    for (SIZE_T i = 5; i < length; ++i) target[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, length); VirtualProtect(target, length, old, &old);
    *trampoline_out = trampoline; return true;
}

bool install_build_telemetry_hooks() {
    // Never split a multi-byte x86 instruction when copying the prologue into
    // a trampoline.  In particular, both 8b 86 <disp32> and 8b f9 must be
    // copied as complete instructions.
    const unsigned char queue[] = {0x55,0x8b,0xec,0x56,0x8b,0xf1,0x8b,0x86,0x74,0x02,0x00,0x00};
    const unsigned char producer_finish[] = {0x55,0x8b,0xec,0x83,0xec,0x34,0x53,0x56};
    const unsigned char rig_finish[] = {0x55,0x8b,0xec,0x51,0x53,0x56,0x57,0x8b,0xf9};
    const unsigned char research_finish[] = {0x55,0x8b,0xec,0x51,0x56,0x57,0x8b,0xf1};
    const unsigned char add_unit[] = {0x55,0x8b,0xec,0x51,0x53,0x8b,0xd9,0x56};
    return install_string_hook(kProducerPushBuildQueueItemRva, queue, sizeof(queue),
                               reinterpret_cast<void*>(&build_queue_hook), &g_build_queue_trampoline) &&
           install_string_hook(kProducerFinishBuildRva, producer_finish, sizeof(producer_finish),
                               reinterpret_cast<void*>(&producer_finish_hook), &g_producer_finish_trampoline) &&
           install_string_hook(kConstructionRigFinishBuildRva, rig_finish, sizeof(rig_finish),
                               reinterpret_cast<void*>(&rig_finish_hook), &g_rig_finish_trampoline) &&
           install_string_hook(kResearchStationFinishBuildRva, research_finish, sizeof(research_finish),
                               reinterpret_cast<void*>(&research_finish_hook), &g_research_finish_trampoline) &&
           install_string_hook(kTeamAddUnitRva, add_unit, sizeof(add_unit),
                               reinterpret_cast<void*>(&team_add_unit_hook), &g_team_add_unit_trampoline);
}

bool install_loader_hooks() {
    const unsigned char rules[] = {0x55,0x8b,0xec,0x81,0xec,0x30,0x01,0x00,0x00,0x53,0x8b,0x1d,0x08,0x7f,0x7b,0x00};
    const unsigned char scripts[] = {0x55,0x8b,0xec,0x81,0xec,0x30,0x02,0x00,0x00,0x53,0x8b,0x1d,0x08,0x7f,0x7b,0x00};
    const unsigned char tech[] = {0x55,0x8b,0xec,0x6a,0xff,0x68,0x3f,0xcb,0x69,0x00,0x64,0xa1,0x00,0x00,0x00,0x00};
    return install_string_hook(kLoadRulesRva, rules, sizeof(rules), reinterpret_cast<void*>(&rules_hook), &g_rules_trampoline) &&
           install_string_hook(kLoadActionScriptsRva, scripts, sizeof(scripts), reinterpret_cast<void*>(&scripts_hook), &g_scripts_trampoline) &&
           install_string_hook(kTechnologyParseRva, tech, sizeof(tech), reinterpret_cast<void*>(&tech_hook), &g_tech_trampoline);
}

bool install_objective_hook() {
    auto* target = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)) + kObjectiveCompleteRva;
    const unsigned char signature[] = {0x55,0x8b,0xec,0x8b,0x45,0x0c,0x8b,0x4d,0x08};
    if (memcmp(target, signature, sizeof(signature)) != 0) return false;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, kObjectiveHookBytes + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false; memcpy(trampoline, target, kObjectiveHookBytes); trampoline[kObjectiveHookBytes] = 0xe9;
    *reinterpret_cast<int32_t*>(trampoline + kObjectiveHookBytes + 1) = static_cast<int32_t>((target + kObjectiveHookBytes) - (trampoline + kObjectiveHookBytes + 5));
    DWORD old{}; if (!VirtualProtect(target, kObjectiveHookBytes, PAGE_EXECUTE_READWRITE, &old)) return false;
    target[0]=0xe9; *reinterpret_cast<int32_t*>(target+1)=static_cast<int32_t>(reinterpret_cast<unsigned char*>(&objective_hook)-(target+5));
    for (SIZE_T i=5;i<kObjectiveHookBytes;++i) target[i]=0x90; FlushInstructionCache(GetCurrentProcess(),target,kObjectiveHookBytes); VirtualProtect(target,kObjectiveHookBytes,old,&old); g_objective_trampoline=trampoline; return true;
}

bool install_succeed_hook() {
    auto* target = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)) + kSucceedMissionRva;
    if (memcmp(target, kSucceedMissionSignature.data(), kSucceedMissionSignature.size()) != 0) return false;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, kHookBytes + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, target, kHookBytes);
    trampoline[kHookBytes] = 0xe9;
    *reinterpret_cast<int32_t*>(trampoline + kHookBytes + 1) = static_cast<int32_t>((target + kHookBytes) - (trampoline + kHookBytes + 5));
    DWORD old_protection{};
    if (!VirtualProtect(target, kHookBytes, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
    target[0] = 0xe9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(reinterpret_cast<unsigned char*>(&succeed_hook) - (target + 5));
    for (SIZE_T offset = 5; offset < kHookBytes; ++offset) target[offset] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, kHookBytes);
    VirtualProtect(target, kHookBytes, old_protection, &old_protection);
    g_succeed_trampoline = trampoline;
    return true;
}

bool install_fail_hook() {
    auto* target = reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr)) + kFailMissionRva;
    const unsigned char signature[] = {0x55, 0x8b, 0xec, 0xa1, 0xe4, 0x5b, 0x73, 0x00};
    if (memcmp(target, signature, sizeof(signature)) != 0) return false;
    auto* trampoline = static_cast<unsigned char*>(VirtualAlloc(nullptr, kHookBytes + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, target, kHookBytes);
    trampoline[kHookBytes] = 0xe9;
    *reinterpret_cast<int32_t*>(trampoline + kHookBytes + 1) = static_cast<int32_t>((target + kHookBytes) - (trampoline + kHookBytes + 5));
    DWORD old_protection{};
    if (!VirtualProtect(target, kHookBytes, PAGE_EXECUTE_READWRITE, &old_protection)) return false;
    target[0] = 0xe9;
    *reinterpret_cast<int32_t*>(target + 1) = static_cast<int32_t>(reinterpret_cast<unsigned char*>(&fail_hook) - (target + 5));
    for (SIZE_T offset = 5; offset < kHookBytes; ++offset) target[offset] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, kHookBytes);
    VirtualProtect(target, kHookBytes, old_protection, &old_protection);
    g_fail_trampoline = trampoline;
    return true;
}

void bootstrap() {
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
    const std::string actual = sha256_file(std::wstring(executable, length));
    const bool pinned = actual == kExpectedSha256;
    InitializeCriticalSection(&g_event_lock);
    InterlockedExchange(&g_event_lock_ready, 1);
    std::thread(observer_event_worker).detach();
    // The loader/tech probes are retained for read-only debugger work. Their
    // early-startup ABI has not been proven safe for an injected detour: they
    // produced an invalid blank mission shell.  Do not arm them in live play.
    // The early loader probes remain debugger-only. This script bridge is later
    // and has a compact, verified stdcall ABI: (const char* filename, bool clear).
    // SetMissionFilename uses a dedicated bounded MSVC-string recorder; it is
    // armed only after the normal campaign shell is already active.
    const bool armed = pinned && install_succeed_hook() && install_fail_hook() && install_objective_hook() &&
                       install_objectives_text_from_file_hook() && install_mission_file_hook();
    report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"" + (armed ? std::string("succeed_hook_armed") : std::string("no_hook")) + "\",\"executable_sha256\":\"" + actual + "\",\"pinned\":" + (pinned ? "true" : "false") + "}");
    if (armed) {
        InitializeCriticalSection(&g_build_lock);
        InterlockedExchange(&g_build_lock_ready, 1);
        std::thread(control_worker).detach();
    }
    while (armed) {
        BuildEvent completed{};
        bool have_completed{};
        if (InterlockedCompareExchange(&g_build_lock_ready, 0, 0)) {
            EnterCriticalSection(&g_build_lock);
            if (g_completed_build_count) {
                completed = g_completed_builds[g_completed_build_head];
                g_completed_build_head = (g_completed_build_head + 1) % g_completed_builds.size();
                --g_completed_build_count;
                have_completed = true;
            }
            LeaveCriticalSection(&g_build_lock);
        }
        if (have_completed) {
            const std::string module = active_mission_module();
            if (module.empty()) continue;
            report_status("{\"type\":\"build_complete\",\"adapter\":\"armada2_observer\",\"mission_module\":\"" + json_escape(module) +
                          "\",\"producer\":\"0x" +
                          [&] { std::ostringstream value; value << std::hex << completed.producer; return value.str(); }() +
                          "\",\"build_class\":\"0x" +
                          [&] { std::ostringstream value; value << std::hex << completed.completed_class; return value.str(); }() +
                          "\",\"producer_team\":" + std::to_string(completed.team) +
                          ",\"odf\":\"" + json_escape(completed.odf) +
                          "\",\"normal_queue\":" + (completed.queued_class && completed.completed_class ? "true" : "false") +
                          ",\"local_player\":true,\"owner_verified\":true,\"executable_sha256\":\"" + actual +
                          "\",\"pinned\":true}");
        }
        if (InterlockedExchange(&g_rules_pending, 0)) report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"rules_load_hit\",\"file\":\"" + std::string(g_rules_file) + "\",\"executable_sha256\":\"" + actual + "\",\"pinned\":true}");
        if (InterlockedExchange(&g_scripts_pending, 0)) report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"script_load_hit\",\"file\":\"" + std::string(g_scripts_file) + "\",\"executable_sha256\":\"" + actual + "\",\"pinned\":true}");
        if (InterlockedExchange(&g_tech_pending, 0)) report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"tech_tree_hit\",\"file\":\"" + std::string(g_tech_file) + "\",\"executable_sha256\":\"" + actual + "\",\"pinned\":true}");
        if(InterlockedExchange(&g_file_pending,0)) report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"objective_file_hook_hit\",\"objective_file\":\""+std::string(g_objective_file)+"\",\"executable_sha256\":\""+actual+"\",\"pinned\":true}");
        if(InterlockedExchange(&g_parse_pending,0)) report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"objective_parse_hook_hit\",\"objective_file\":\""+std::string(g_parsed_objective_file)+"\",\"executable_sha256\":\""+actual+"\",\"pinned\":true}");
        if (InterlockedExchange(&g_mission_file_pending, 0)) {
            const auto* image = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
            const auto selected = *reinterpret_cast<const signed char*>(image + kSelectedMissionRva);
            const auto shell_ordinal = *reinterpret_cast<const signed char*>(image + kShellMissionOrdinalRva);
            const auto campaign_index = *reinterpret_cast<const int32_t*>(image + kShellCampaignIndexRva);
            report_status("{\"type\":\"adapter_status\",\"adapter\":\"armada2_observer\",\"mode\":\"native_mission_filename_hit\",\"mission_file\":\"" + json_escape(g_mission_file) + "\",\"selected_mission\":" + std::to_string(selected) + ",\"shell_mission_ordinal\":" + std::to_string(shell_ordinal) + ",\"campaign_index\":" + std::to_string(campaign_index) + ",\"executable_sha256\":\"" + actual + "\",\"pinned\":true}");
        }
        if (InterlockedExchange(&g_succeed_pending, 0)) {
            const auto address = reinterpret_cast<void*>(InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_succeed_return_address), nullptr));
            HMODULE caller{}; wchar_t path[MAX_PATH]{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &caller);
            GetModuleFileNameW(caller, path, MAX_PATH);
            std::string script_module;
            for (const auto candidate : g_succeed_stack) {
                HMODULE module{}; wchar_t candidate_path[MAX_PATH]{};
                if (candidate && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                     reinterpret_cast<LPCWSTR>(candidate), &module) &&
                    GetModuleFileNameW(module, candidate_path, MAX_PATH)) {
                    const auto name = std::filesystem::path(candidate_path).filename().string();
                    if (name.size() >= 4 && name.substr(name.size() - 4) == ".dsl") { script_module = name; break; }
                }
            }
            const std::string module = script_module.empty() ? std::filesystem::path(path).filename().string() : script_module;
            report_status("{\"type\":\"mission_result\",\"adapter\":\"armada2_observer\",\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"mission_module\":\"" + json_escape(module) + "\",\"result\":\"success\",\"executable_sha256\":\"" + actual + "\",\"pinned\":true}");
        }
        if (InterlockedExchange(&g_fail_pending, 0)) {
            const auto address = reinterpret_cast<void*>(InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_fail_return_address), nullptr));
            HMODULE caller{}; wchar_t path[MAX_PATH]{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(address), &caller);
            GetModuleFileNameW(caller, path, MAX_PATH);
            std::string script_module;
            for (const auto candidate : g_fail_stack) {
                HMODULE module{}; wchar_t candidate_path[MAX_PATH]{};
                if (candidate && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                                     reinterpret_cast<LPCWSTR>(candidate), &module) &&
                    GetModuleFileNameW(module, candidate_path, MAX_PATH)) {
                    const auto name = std::filesystem::path(candidate_path).filename().string();
                    if (name.size() >= 4 && name.substr(name.size() - 4) == ".dsl") { script_module = name; break; }
                }
            }
            const std::string module = script_module.empty() ? std::filesystem::path(path).filename().string() : script_module;
            report_status("{\"type\":\"mission_result\",\"adapter\":\"armada2_observer\",\"pid\":" + std::to_string(GetCurrentProcessId()) + ",\"mission_module\":\"" + json_escape(module) + "\",\"result\":\"failure\",\"executable_sha256\":\"" + actual + "\",\"pinned\":true}");
        }
        if (InterlockedExchange(&g_objective_pending, 0)) {
            const auto address=reinterpret_cast<void*>(InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_objective_return_address),nullptr)); HMODULE caller{}; wchar_t path[MAX_PATH]{}; GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(address),&caller); GetModuleFileNameW(caller,path,MAX_PATH);
            const auto a1=reinterpret_cast<uintptr_t>(InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_objective_arg1),nullptr)); const auto a2=reinterpret_cast<uintptr_t>(InterlockedExchangePointer(reinterpret_cast<PVOID volatile*>(&g_objective_arg2),nullptr));
            const std::string module = std::filesystem::path(path).filename().string();
            const std::string file = g_objective_file;
            // Initial displays have no SetObjectiveComplete calls in the proven
            // Fed1 flow. arg2 is the engine completion boolean; only completed
            // transitions can cross this observer boundary.
            if (a2 && !file.empty()) {
                report_status("{\"type\":\"objective_complete\",\"adapter\":\"armada2_observer\",\"mission_module\":\""+json_escape(module)+"\",\"objective_file\":\""+json_escape(file)+"\",\"objective_index\":"+std::to_string(a1)+",\"complete\":true,\"initial\":false,\"executable_sha256\":\""+actual+"\",\"pinned\":true}");
            }
        }
        Sleep(50);
    }
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_observer_module = module;
        std::thread(bootstrap).detach();
    }
    return TRUE;
}
