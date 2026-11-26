// Wave 3 in-game PC settings menu (spec §1.1-§1.4, §4.1, §4.2 item 3).
//
// Zero rex:: APIs. Guest memory is only ever touched through
// mcla::kernel::GuestMemoryHeap::Instance() checked endian-explicit helpers.
//
// FEATURE OFF IS A PASSTHROUGH. Every menu mutation is gated by MclaMenuEnabled():
// MCLA_NO_WAVE3_MENU wins, and mcla_settings_menu must be registered and true.
// The row tables below contain only settings with real registered readers;
// missing readers stay filtered out. The pause controller/list/row path below
// is measured from the raw guest image and generated TUs (see the constants).

#include "hooks/hooks.h"
#include "hooks/wave3_midhooks.h"
#include "ppc_context.h"
#include "kernel/memory.h"
#include "common/cvar.h"
#include "app.h"
#include "logging.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Original recompiled guest bodies (C linkage, defined in generated TUs).
PPC_FUNC_IMPL(__imp__sub_82220308);
PPC_FUNC_IMPL(__imp__sub_82631FD0);
PPC_FUNC_IMPL(__imp__sub_82630E40);
PPC_FUNC_IMPL(__imp__sub_826309E0);
PPC_FUNC_IMPL(__imp__sub_8260EBB8);
PPC_FUNC_IMPL(__imp__sub_8260E9A0);
PPC_FUNC_IMPL(__imp__sub_8260FAA0);

// Additional measured, map-only entry points. They stay strong C++-linkage
// overrides like the Wave 1/2 hooks; addr_owners.py reports one map row each.
PPC_FUNC_IMPL(__imp__sub_82660310);
PPC_FUNC_IMPL(__imp__sub_826334D0);
PPC_FUNC(sub_82660310);
PPC_FUNC(sub_826334D0);

// Guest helper VAs referenced only via CallGuest below. All were measured with
// `python tools/addr_owners.py --check <va>`: rc=0, 1 site each (a map row in
// generated/ppc_xenon/ppc_func_mapping.cpp), and all appear in spec §0.2's
// unclaimed-helper audit:
//   0x8263B860 kSetRowLabelFn   :32777   0x82722398 kGetFlashIntFn :38832
//   0x82661508 kIsButtonClickedFn:33659  0x827227B8 kSetFlashStrFn :38840
//   0x8263AD60 kUIObjectCtorFn  :32737   0x826349F0 kOptionRowCtorFn:32539
//   0x82633D40 kLabelRowCtorFn  :32517   0x8268D578 kAllocStateFn  :34913

namespace {

// ---------------------------------------------------------------------------
// Enable gate (single-variable bisect friendly, project convention MCLA_NO_*).
// ---------------------------------------------------------------------------
bool EnvOptedOut() {
    static const bool optedOut = []() {
        const char* e = std::getenv("MCLA_NO_WAVE3_MENU");
        return e != nullptr && std::strcmp(e, "0") != 0;
    }();
    return optedOut;
}

bool MclaMenuEnabled() {
    if (EnvOptedOut()) return false;
    const auto& cv = mcla::cvar::CVarSystem::Instance();
    // Has() keeps the helper safe before CVar registration and when the clean
    // mcla_settings_menu=false off-switch is selected.
    return cv.Has("mcla_settings_menu") && cv.GetBool("mcla_settings_menu");
}

// ---------------------------------------------------------------------------
// Safe guest call helper (spec §1.2). A 256-byte child frame with a valid
// PowerPC back-chain; callerCtx is untouched except for the written word at
// [sp]. Works because cr*/ctr/xer/r11..r31 live as C++ locals in the caller TU.
// ---------------------------------------------------------------------------
template <typename... Args>
uint32_t CallGuest(PPCContext& callerCtx, uint8_t* base, uint32_t guestFnVa, Args... args) {
    auto* app = mcla::GetApp();
    auto fn = (app && app->GetFunctionDispatcher())
                  ? app->GetFunctionDispatcher()->GetFunction(guestFnVa)
                  : nullptr;
    if (!fn) fn = mcla::kernel::g_memory.FindFunction(guestFnVa);
    if (!fn) {
        MCLA_LOG_WARN("SETTINGS-MENU: missing guest fn 0x{:08X}", guestFnVa);
        return 0;
    }

    PPCContext child{};
    const uint32_t sp = (callerCtx.r1.u32 - 256u) & ~15u;
    (void)mcla::kernel::GuestMemoryHeap::Instance().WriteU32BE(sp, callerCtx.r1.u32);
    child.r1.u64 = sp;
    child.fpscr = callerCtx.fpscr;
    child.msr = callerCtx.msr;
    child.lr = callerCtx.lr;

    const uint32_t argVals[8] = {static_cast<uint32_t>(args)...};
    PPCRegister* regs[] = {&child.r3, &child.r4, &child.r5, &child.r6,
                           &child.r7, &child.r8, &child.r9, &child.r10};
    for (size_t i = 0; i < sizeof...(Args) && i < 8; ++i) regs[i]->u64 = argVals[i];

    fn(child, base);
    return child.r3.u32;
}

// ---------------------------------------------------------------------------
// Host-side menu state. All values are guest VAs; 0 == unknown.
// ---------------------------------------------------------------------------
std::atomic<uint32_t> s_continueTab{0};
std::atomic<uint32_t> s_settingsState{0};
std::atomic<uint32_t> s_pauseList{0};
std::atomic<uint32_t> s_lastList{0};
std::atomic<uint32_t> s_lastTabStrip{0};
std::atomic<int> s_activeMenu{-1};
std::atomic<uint32_t> s_menuState[4]{};  // replacement r3 for sub_82220308
std::atomic<uint64_t> s_tickLog{0};
std::atomic<uint32_t> s_pauseController{0};
std::atomic<uint32_t> s_installedList{0};

// Measured guest layout (raw words + generated bodies):
//   controller vtable 0x820A1FB0: +0 sub_82660310, +16 sub_82668660.
//   fixed-list vtables 0x8209F034 / 0x820A19D4: +32 sub_826334D0,
//     +176 sub_82631FD0. Their ctor sub_82634738 stores array/count/capacity
//     at +176/+180/+182, selection at +184, adapter at +192, VHSM at +196.
//   label-row vtable 0x8208DACC; ctor sub_82633D40, setter sub_8263B860.
//   sub_82661210 resolves the controller's current list through manager
//     0x8200D804 -> +48 -> +0x13F80 -> children[72], then either
//     controller+1152 or [ [controller+352] + [controller+356]*4 ] -> +68 ->
//     [that pointer].
constexpr uint32_t kPauseControllerVtable = 0x820A1FB0u;
constexpr uint32_t kFixedListVtable = 0x8209F034u;
constexpr uint32_t kEmbeddedListVtable = 0x820A19D4u;
constexpr uint32_t kLabelRowVtable = 0x8208DACCu;
constexpr uint32_t kUiManagerGlobal = 0x8200D804u;
constexpr size_t kGuestRowBytes = 208u;  // caller allocates 208 before 82633D40
constexpr size_t kMaxGuestRows = 32u;
constexpr size_t kGuestLabelBytes = 96u;

uint32_t s_guestRowObjects = 0;
uint32_t s_guestRowArray = 0;
uint32_t s_guestLabels = 0;
bool s_guestRowsReady = false;
std::array<uint32_t, kMaxGuestRows> s_guestRows{};

struct SavedListFields {
    uint32_t list = 0;
    uint32_t array = 0;
    uint32_t countAndCapacity = 0;
    uint32_t selected = 0;
    uint32_t adapter = 0;
    uint32_t vhsm = 0;
};
SavedListFields s_savedList;

// ---------------------------------------------------------------------------
// Declarative row tables. A row is emitted only if every CVar it names is
// actually registered (CVarSystem::Has), so the UI never shows a dead row.
// ---------------------------------------------------------------------------
enum MenuIndex { kMenuPause = 0, kMenuDisplay = 1, kMenuPerformance = 2, kMenuControls = 3, kMenuCount = 4 };
enum RowAction { kActionNone = 0, kActionSave, kActionRestart, kActionQuit,
                 kActionDisplay, kActionPerformance, kActionControls, kActionContinue };

struct MenuRow {
    const char* label;
    const char* cvar;          // nullptr => action row
    const char* gateCvar;      // must be registered for the row to exist
    const char* const* choices;
    size_t choiceCount;
    int action;
    float minValue = 0.0f;     // numeric rows use step/min/max instead of choices
    float maxValue = 0.0f;
    float step = 0.0f;
};

const char* const kBoolChoices[] = {"false", "true"};
const char* const kGammaChoices[] = {"tv", "pc"};
const char* const kAspectChoices[] = {"auto", "16:9", "16:10", "43:18"};
const char* const kPromptsChoices[] = {"auto", "xbox", "ps"};
const char* const kIntroChoices[] = {"normal", "skip"};
const char* const kFpsChoices[] = {"30", "60", "120", "0", "-1"};
const char* const kRendererChoices[] = {"native", "legacy", "capture"};
const char* const kPatchGroupChoices[] = {"all", "none", "sdk,guest,native"};
const char* const kDisplayModeChoices[] = {"windowed", "borderless", "fullscreen"};
const char* const kScaleChoices[] = {"1", "2", "3"};

const MenuRow kPauseRows[] = {
    {"CONTINUE", nullptr, "mcla_settings_menu", nullptr, 0, kActionContinue},
    {"SAVE GAME", nullptr, "mcla_settings_menu", nullptr, 0, kActionSave},
    {"RESTART GAME", nullptr, "mcla_settings_menu", nullptr, 0, kActionRestart},
    {"QUIT GAME", nullptr, "mcla_settings_menu", nullptr, 0, kActionQuit},
    {"DISPLAY", nullptr, "mcla_settings_menu", nullptr, 0, kActionDisplay},
    {"PERFORMANCE", nullptr, "mcla_settings_menu", nullptr, 0, kActionPerformance},
    {"CONTROLS", nullptr, "mcla_settings_menu", nullptr, 0, kActionControls},
};

// Rows are filtered by CVarSystem::Has. UI interpolation has no reader in this
// tree, so it stays absent instead of pretending to control the game.
const MenuRow kDisplayRows[] = {
    {"DISPLAY MODE", "display_mode", "display_mode", kDisplayModeChoices, 3, kActionNone},
    {"MOTION BLUR", "mcla_motion_blur", "mcla_motion_blur", kBoolChoices, 2, kActionNone},
    {"DEPTH OF FIELD", "mcla_depth_of_field", "mcla_depth_of_field", kBoolChoices, 2, kActionNone},
    {"AMBIENT OCCLUSION (RESTART)", "mcla_ambient_occlusion", "mcla_ambient_occlusion", kBoolChoices, 2, kActionNone},
    {"SHADOWS (RESTART)", "mcla_shadows", "mcla_shadows", kBoolChoices, 2, kActionNone},
    {"DISPLAY GAMMA (RESTART)", "mcla_display_gamma", "mcla_display_gamma", kGammaChoices, 2, kActionNone},
    {"ASPECT RATIO", "mcla_aspect_ratio", "mcla_aspect_ratio", kAspectChoices, 4, kActionNone},
    {"FRAME RATE", "mcla_fps", "mcla_fps", kFpsChoices, 5, kActionNone},
    {"VSYNC", "vsync", "vsync", kBoolChoices, 2, kActionNone},
    {"FULLSCREEN", "fullscreen", "fullscreen", kBoolChoices, 2, kActionNone},
    {"RENDER SCALE (RESTART)", "resolution_scale", "resolution_scale", kScaleChoices, 3, kActionNone},
    {"RENDERER (RESTART)", "renderer_mode", "renderer_mode", kRendererChoices, 3, kActionNone},
    {"UI INTERPOLATION", "mcla_ui_interpolate", "mcla_ui_interpolate", kBoolChoices, 2, kActionNone},
};

const MenuRow kPerformanceRows[] = {
    {"FULLSCREEN BLUR (RESTART)", "mcla_fullscreen_blur", "mcla_fullscreen_blur", kBoolChoices, 2, kActionNone},
    {"RACE SHADOWS (RESTART)", "mcla_race_shadows", "mcla_race_shadows", kBoolChoices, 2, kActionNone},
    {"FAST CAR SHADOWS (RESTART)", "mcla_fast_car_shadows", "mcla_fast_car_shadows", kBoolChoices, 2, kActionNone},
    {"FOLIAGE IMPOSTORS (RESTART)", "mcla_foliage_impostors", "mcla_foliage_impostors", kBoolChoices, 2, kActionNone},
    {"LOD SCALE (RESTART)", "mcla_lod_scale", "mcla_lod_scale", nullptr, 0, kActionNone, 0.25f, 2.0f, 0.05f},
    {"FRAME DELTA MAX", "mcla_dt_max", "mcla_dt_max", nullptr, 0, kActionNone, 0.01f, 0.5f, 0.01f},
    {"FLOAT CLOCK", "mcla_float_clock", "mcla_float_clock", kBoolChoices, 2, kActionNone},
    {"SWAP UNLOCK", "mcla_swap_unlock", "mcla_swap_unlock", kBoolChoices, 2, kActionNone},
    {"PACE AT SWAP", "mcla_pace_at_swap", "mcla_pace_at_swap", kBoolChoices, 2, kActionNone},
    {"CONTINUOUS PHYSICS", "mcla_physics_continuous", "mcla_physics_continuous", kBoolChoices, 2, kActionNone},
    {"CHECKPOINT SWEEP", "mcla_checkpoint_sweep", "mcla_checkpoint_sweep", kBoolChoices, 2, kActionNone},
    {"POLL WAIT", "mcla_poll_wait", "mcla_poll_wait", kBoolChoices, 2, kActionNone},
    {"POLL SPIN US", "mcla_poll_spin_us", "mcla_poll_spin_us", nullptr, 0, kActionNone, 0.0f, 2000.0f, 10.0f},
    {"POLL TAIL US", "mcla_poll_tail_us", "mcla_poll_tail_us", nullptr, 0, kActionNone, 0.0f, 1000.0f, 10.0f},
    {"PUMP WAKE ON DEMAND", "mcla_pump_wake_on_demand", "mcla_pump_wake_on_demand", kBoolChoices, 2, kActionNone},
    {"PUMP WAKE PAUSE S", "mcla_pump_wake_pause_s", "mcla_pump_wake_pause_s", nullptr, 0, kActionNone, 0.0f, 3600.0f, 10.0f},
    {"PUMP SLEEP MAX MS", "mcla_pump_sleep_max_ms", "mcla_pump_sleep_max_ms", nullptr, 0, kActionNone, -1.0f, 500.0f, 1.0f},
    {"PATCH GROUPS (RESTART)", "mcla_patch_groups", "mcla_patch_groups", kPatchGroupChoices, 3, kActionNone},
    {"TWO-STAGE RENDER (RESTART)", "mcla_render_2stage", "mcla_render_2stage", kBoolChoices, 2, kActionNone},
    {"LEAN CENSUS (RESTART)", "mcla_lean_census", "mcla_lean_census", kBoolChoices, 2, kActionNone},
    {"LEAN COPY (RESTART)", "mcla_lean_copy", "mcla_lean_copy", kBoolChoices, 2, kActionNone},
};

const MenuRow kControlsRows[] = {
    {"BUTTON PROMPTS", "mcla_button_prompts", "mcla_button_prompts", kPromptsChoices, 3, kActionNone},
    {"VIBRATION", "mcla_vibration", "mcla_vibration", kBoolChoices, 2, kActionNone},
    {"LEFT STICK DEADZONE", "mcla_left_stick_deadzone", "mcla_left_stick_deadzone", nullptr, 0, kActionNone, 0.0f, 30.0f, 1.0f},
    {"RIGHT STICK DEADZONE", "mcla_right_stick_deadzone", "mcla_right_stick_deadzone", nullptr, 0, kActionNone, 0.0f, 30.0f, 1.0f},
    {"PAD LATCH MS", "mcla_script_pad_latch_ms", "mcla_script_pad_latch_ms", nullptr, 0, kActionNone, 0.0f, 500.0f, 10.0f},
    {"INTRO", "mcla_intro", "mcla_intro", kIntroChoices, 2, kActionNone},
    {"HIDE CURSOR", "mcla_hide_cursor", "mcla_hide_cursor", kBoolChoices, 2, kActionNone},
};

const MenuRow* const kTables[] = {kPauseRows, kDisplayRows, kPerformanceRows, kControlsRows};
const size_t kCounts[] = {sizeof(kPauseRows) / sizeof(kPauseRows[0]),
                          sizeof(kDisplayRows) / sizeof(kDisplayRows[0]),
                          sizeof(kPerformanceRows) / sizeof(kPerformanceRows[0]),
                          sizeof(kControlsRows) / sizeof(kControlsRows[0])};

std::mutex s_rowsMutex;
std::vector<MenuRow> s_builtRows;
std::vector<std::string> s_rowValues;

void BuildRows(int menu, std::vector<MenuRow>& out) {
    if (menu < 0 || menu >= kMenuCount) return;
    const auto& cv = mcla::cvar::CVarSystem::Instance();
    for (size_t i = 0; i < kCounts[menu]; ++i) {
        const MenuRow& row = kTables[menu][i];
        if (row.gateCvar && !cv.Has(row.gateCvar)) continue;
        if (row.cvar && !cv.Has(row.cvar)) continue;
        out.push_back(row);
    }
}

void PollRows() {
    const auto& cv = mcla::cvar::CVarSystem::Instance();
    std::lock_guard<std::mutex> lock(s_rowsMutex);
    s_builtRows.clear();
    s_rowValues.clear();
    BuildRows(s_activeMenu.load(), s_builtRows);
    s_rowValues.reserve(s_builtRows.size());
    for (const auto& row : s_builtRows)
        s_rowValues.push_back(row.cvar ? std::string(cv.GetString(row.cvar)) : std::string());
}

bool ReadGuest32(uint32_t addr, uint32_t& out) {
    return mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(addr, &out);
}

bool CurrentPauseList(uint32_t controller, uint32_t& list) {
    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
    uint32_t vt = 0, manager = 0, managerBase = 0, uiBase = 0, children = 0, screen = 0;
    uint8_t embedded = 0;
    if (controller == 0 || !ReadGuest32(controller, vt) || vt != kPauseControllerVtable) return false;
    if (!ReadGuest32(kUiManagerGlobal, manager) || manager == 0 ||
        !ReadGuest32(manager + 48u, managerBase) || managerBase == 0 ||
        !ReadGuest32(managerBase + 0x13F80u + 68u, children) || children == 0 ||
        !ReadGuest32(children + 72u * 4u, screen) || screen == 0 ||
        !mem.ReadU8(screen + 246u, &embedded)) return false;

    if (embedded != 0) {
        list = controller + 1152u;
    } else {
        uint32_t pages = 0, pageIndex = 0, page = 0, widget = 0;
        if (!ReadGuest32(controller + 352u, pages) || pages == 0 ||
            !ReadGuest32(controller + 356u, pageIndex) || pageIndex == 0xFFFFFFFFu ||
            !ReadGuest32(pages + pageIndex * 4u, page) || page == 0 ||
            !ReadGuest32(page + 68u, widget) || widget == 0 ||
            !ReadGuest32(widget, list)) return false;
    }

    uint32_t listVt = 0;
    return ReadGuest32(list, listVt) &&
           (listVt == kFixedListVtable || listVt == kEmbeddedListVtable);
}

void RestoreGuestRows() {
    const uint32_t list = s_installedList.exchange(0);
    if (list == 0 || s_savedList.list != list) return;
    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
    bool ok = mem.WriteU32BE(list + 176u, s_savedList.array) &&
              mem.WriteU32BE(list + 180u, s_savedList.countAndCapacity) &&
              mem.WriteU32BE(list + 184u, s_savedList.selected) &&
              mem.WriteU32BE(list + 192u, s_savedList.adapter) &&
              mem.WriteU32BE(list + 196u, s_savedList.vhsm);
    if (!ok) MCLA_LOG_WARN("SETTINGS-MENU: failed to restore guest list {:08X}", list);
    s_savedList = {};
}

bool EnsureGuestRowStorage(PPCContext& ctx, uint8_t* base) {
    if (s_guestRowsReady) return true;
    if (s_guestRowObjects != 0) return false;  // a prior init failed; stay passthrough
    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
    const size_t objectBytes = kGuestRowBytes * kMaxGuestRows;
    const size_t arrayBytes = sizeof(uint32_t) * kMaxGuestRows;
    const size_t labelBytes = kGuestLabelBytes * kMaxGuestRows;
    const uint32_t objects = mem.Alloc(objectBytes, 16u);
    const uint32_t array = mem.Alloc(arrayBytes, 16u);
    const uint32_t labels = mem.Alloc(labelBytes, 16u);
    if (objects == 0 || array == 0 || labels == 0) {
        if (objects) mem.Free(objects, objectBytes);
        if (array) mem.Free(array, arrayBytes);
        if (labels) mem.Free(labels, labelBytes);
        MCLA_LOG_WARN("SETTINGS-MENU: guest row allocation failed");
        return false;
    }
    s_guestRowObjects = objects;
    s_guestRowArray = array;
    s_guestLabels = labels;
    const std::array<uint8_t, objectBytes> zeroObjects{};
    if (!mem.WriteBytes(objects, zeroObjects.data(), objectBytes)) return false;

    for (size_t i = 0; i < kMaxGuestRows; ++i) {
        const uint32_t row = objects + static_cast<uint32_t>(i * kGuestRowBytes);
        s_guestRows[i] = row;
        const uint32_t made = CallGuest(ctx, base, 0x82633D40u, row, labels + static_cast<uint32_t>(i * kGuestLabelBytes), 0u);
        uint32_t vt = 0;
        if (made != row || !ReadGuest32(row, vt) || vt != kLabelRowVtable) {
            MCLA_LOG_WARN("SETTINGS-MENU: label-row ctor failed at row {}", i);
            return false;
        }
    }
    s_guestRowsReady = true;
    MCLA_LOG_INFO("SETTINGS-MENU: guest rows ready objects={:08X} array={:08X} labels={:08X}",
                  objects, array, labels);
    return true;
}

void CopyBuiltRows(std::vector<MenuRow>& rows, std::vector<std::string>& values) {
    std::lock_guard<std::mutex> lock(s_rowsMutex);
    rows = s_builtRows;
    values = s_rowValues;
}

bool UpdateGuestRowLabels(PPCContext& ctx, uint8_t* base) {
    if (!s_guestRowsReady) return false;
    std::vector<MenuRow> rows;
    std::vector<std::string> values;
    CopyBuiltRows(rows, values);
    if (rows.empty() || rows.size() > kMaxGuestRows) return false;
    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
    for (size_t i = 0; i < rows.size(); ++i) {
        std::string label(rows[i].label);
        if (rows[i].cvar && i < values.size()) label += ": " + values[i];
        if (label.size() >= kGuestLabelBytes) label.resize(kGuestLabelBytes - 1u);
        const uint32_t labelVa = s_guestLabels + static_cast<uint32_t>(i * kGuestLabelBytes);
        if (!mem.WriteBytes(labelVa, label.c_str(), static_cast<uint32_t>(label.size() + 1u))) {
            MCLA_LOG_WARN("SETTINGS-MENU: row label write failed at row {}", i);
            return false;
        }
        (void)CallGuest(ctx, base, 0x8263B860u, s_guestRows[i], labelVa);
        (void)mem.WriteU32BE(s_guestRowArray + static_cast<uint32_t>(i * 4u), s_guestRows[i]);
    }
    return true;
}

bool InstallGuestRows(PPCContext& ctx, uint8_t* base, uint32_t list) {
    if (!EnsureGuestRowStorage(ctx, base)) return false;
    std::vector<MenuRow> rows;
    std::vector<std::string> values;
    CopyBuiltRows(rows, values);
    if (rows.empty() || rows.size() > kMaxGuestRows) return false;

    if (s_installedList.load() != list) {
        RestoreGuestRows();
        uint32_t vt = 0, array = 0, countAndCapacity = 0, selected = 0;
        uint32_t adapter = 0, vhsm = 0;
        if (!ReadGuest32(list, vt) || (vt != kFixedListVtable && vt != kEmbeddedListVtable) ||
            !ReadGuest32(list + 176u, array) || !ReadGuest32(list + 180u, countAndCapacity) ||
            !ReadGuest32(list + 184u, selected) || !ReadGuest32(list + 192u, adapter) ||
            !ReadGuest32(list + 196u, vhsm)) return false;
        s_savedList = {list, array, countAndCapacity, selected, adapter, vhsm};
    }

    if (!UpdateGuestRowLabels(ctx, base)) {
        if (s_installedList.load() != list) s_savedList = {};
        return false;
    }
    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
    const uint32_t count = static_cast<uint32_t>(rows.size());
    uint32_t selected = 0;
    if (s_installedList.load() == list) (void)ReadGuest32(list + 184u, selected);
    if (selected >= count) selected = 0;
    uint32_t listVt = 0;
    if (!ReadGuest32(list, listVt)) return false;
    const uint16_t capacity = static_cast<uint16_t>(kMaxGuestRows);
    const bool ok = mem.WriteU32BE(list + 176u, s_guestRowArray) &&
                    mem.WriteU16BE(list + 180u, static_cast<uint16_t>(count)) &&
                    mem.WriteU16BE(list + 182u, capacity) &&
                    mem.WriteU32BE(list + 184u, selected) &&
                    mem.WriteU32BE(list + 192u, 0u) &&
                    mem.WriteU32BE(list + 196u, 0u);
    if (!ok) {
        RestoreGuestRows();
        return false;
    }
    s_installedList.store(list);
    s_pauseList.store(list);
    static std::atomic<bool> s_loggedInstall{false};
    if (!s_loggedInstall.exchange(true))
        MCLA_LOG_INFO("SETTINGS-MENU: installed list={:08X} menu={} rows={}", list,
                      s_activeMenu.load(), count);
    return true;
}

int SelectedHostRow(uint32_t list) {
    if (list == 0 || s_installedList.load() != list || !s_guestRowsReady) return -1;
    std::lock_guard<std::mutex> lock(s_rowsMutex);
    if (s_builtRows.empty() || s_builtRows.size() > kMaxGuestRows) return -1;
    uint32_t selected = 0, node = 0;
    if (!ReadGuest32(list + 184u, selected) || selected >= s_builtRows.size() ||
        !ReadGuest32(s_guestRowArray + selected * 4u, node)) return -1;
    return node == s_guestRows[selected] ? static_cast<int>(selected) : -1;
}

// Cycle a CVar row through its declared choices. SetString fires registered
// change callbacks AFTER releasing the CVarSystem mutex (cvar.h:112-129).
void CycleRow(size_t index) {
    std::lock_guard<std::mutex> lock(s_rowsMutex);
    if (index >= s_builtRows.size()) return;
    const MenuRow& row = s_builtRows[index];
    if (!row.cvar) return;
    auto& cv = mcla::cvar::CVarSystem::Instance();

    if (row.step > 0.0f && row.maxValue > row.minValue) {
        float value = cv.GetFloat(row.cvar);
        if (!std::isfinite(value) || value < row.minValue || value > row.maxValue) {
            value = row.minValue;
        } else {
            const float steps = std::round((value - row.minValue) / row.step);
            value = row.minValue + (steps + 1.0f) * row.step;
            if (value > row.maxValue + 1e-4f) value = row.minValue;
        }
        (void)cv.SetFloat(row.cvar, value);
        MCLA_LOG_INFO("SETTINGS-MENU: {} -> {}", row.cvar, cv.GetString(row.cvar));
        return;
    }

    if (!row.choices || row.choiceCount == 0) return;
    const std::string cur(cv.GetString(row.cvar));
    size_t at = 0;
    while (at < row.choiceCount) {
        const std::string_view choice(row.choices[at]);
        const bool match = choice == "true" ? cv.GetBool(row.cvar)
                          : choice == "false" ? !cv.GetBool(row.cvar)
                          : choice == cur;
        if (match) break;
        ++at;
    }
    at = (at + 1u) % row.choiceCount;
    (void)cv.SetString(row.cvar, row.choices[at]);
    if (std::strcmp(row.cvar, "display_mode") == 0) {
        const std::string_view chosen(row.choices[at]);
        const bool on = chosen == "fullscreen" || chosen == "borderless" || chosen == "borderless_fullscreen";
        if (cv.GetBool("fullscreen") != on) {
            (void)cv.SetBool("fullscreen", on);
        }
    }
    MCLA_LOG_INFO("SETTINGS-MENU: {} -> {}", row.cvar, cv.GetString(row.cvar));
}

bool ApplyRowAction(int action, PPCContext& ctx, uint8_t* base) {
    switch (action) {
        case kActionDisplay:
        case kActionPerformance:
        case kActionControls: {
            const int previous = s_activeMenu.load();
            const int next = action == kActionDisplay ? kMenuDisplay
                             : action == kActionPerformance ? kMenuPerformance
                                                            : kMenuControls;
            MclaMenuSetActive(next);
            PollRows();
            MCLA_LOG_INFO("SETTINGS-MENU: submenu {}", next);
            const uint32_t list = s_installedList.load();
            if (list == 0 || !InstallGuestRows(ctx, base, list)) {
                MclaMenuSetActive(previous);
                PollRows();
                return false;
            }
            return true;
        }
        case kActionContinue:
            RestoreGuestRows();
            s_activeMenu.store(-1);
            (void)CallGuest(ctx, base, 0x82668480u, s_pauseController.load());
            return true;
        case kActionSave:
        case kActionRestart:
        case kActionQuit:
            // Process relaunch / WM_CLOSE belong to the app layer (worker 3,
            // src/app.cpp). Log-only here; nothing is signalled by default.
            MCLA_LOG_INFO("SETTINGS-MENU: action {} requested (host side deferred)", action);
            return true;
        default: return false;
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Exported state accessors (declared in hooks/wave3_midhooks.h).
// ---------------------------------------------------------------------------
bool MclaMenuActive() { return s_activeMenu.load() >= 0; }
uint32_t MclaMenuContinueTab() { return s_continueTab.load(); }
uint32_t MclaMenuSettingsState() { return s_settingsState.load(); }
uint32_t MclaMenuPauseList() { return s_pauseList.load(); }
uint32_t MclaMenuLastList() { return s_lastList.load(); }
uint32_t MclaMenuLastTabStrip() { return s_lastTabStrip.load(); }
uint32_t MclaMenuStateFor(int index) {
    return (index >= 0 && index < kMenuCount) ? s_menuState[index].load() : 0u;
}
void MclaMenuSetPauseList(uint32_t list) { s_pauseList.store(list); }
void MclaMenuSetLastList(uint32_t list) { s_lastList.store(list); }
void MclaMenuSetLastTabStrip(uint32_t strip) { s_lastTabStrip.store(strip); }
void MclaMenuSetActive(int index) { s_activeMenu.store(index); }

void MclaMenuTick(uint32_t list) {
    s_lastList.store(list);
    if (!MclaMenuEnabled()) {
        RestoreGuestRows();
        s_activeMenu.store(-1);
        return;
    }
    if (!MclaMenuActive()) return;
    const uint32_t pauseList = s_pauseList.load();
    if (list != 0 && pauseList != 0 && list != pauseList) return;
    PollRows();
    if (s_tickLog.fetch_add(1) < 8)
        MCLA_LOG_INFO("SETTINGS-MENU: tick list=0x{:08X} menu={}", list, s_activeMenu.load());
}

// Live motion-blur companion state, written by worker 3 in its existing
// PPC_FUNC(sub_8260C8E0); zero means "not captured", forcing a passthrough.
uint32_t g_wave3_blurObj = 0;
uint32_t g_wave3_savedTintFade = 0;
uint32_t g_wave3_savedDesatScale = 0;

// ---------------------------------------------------------------------------
// §4.1 midhook helpers. Each returns the guest-original value when disabled.
// ---------------------------------------------------------------------------
void mcla_menu_capture_continue(PPCRegister tabNode) {
    if (!MclaMenuEnabled()) return;
    if (s_activeMenu.load() < 0) s_activeMenu.store(kMenuPause);
    const uint32_t node = tabNode.u32;
    uint32_t tab = 0;
    if (node != 0 && mcla::kernel::GuestMemoryHeap::Instance().ReadU32BE(node + 32u, &tab) && tab != 0)
        s_continueTab.store(tab);
}

bool mcla_menu_enable_save(PPCContext& ctx, uint8_t* base) {
    (void)base;
    if (!MclaMenuEnabled()) return false;
    const uint32_t node = ctx.r3.u32;
    if (node == 0) return false;
    if (s_settingsState.load() == 0) s_settingsState.store(node);
    // Capture the descriptor for diagnostics/state-swap compatibility. The
    // host-owned list below supplies SAVE GAME, so the guest's dev-only Save row
    // stays disabled and its original `li r4,0` remains in force.
    return false;
}

bool mcla_menu_click(PPCContext& ctx, uint8_t* base, PPCRegister controller) {
    if (!MclaMenuEnabled() || !MclaMenuActive()) return false;
    uint32_t controllerVt = 0;
    if (controller.u32 == 0 || !ReadGuest32(controller.u32, controllerVt) ||
        controllerVt != kPauseControllerVtable) return false;
    s_pauseController.store(controller.u32);

    uint32_t list = s_installedList.load();
    if (list == 0) {
        if (!CurrentPauseList(controller.u32, list)) return false;
        PollRows();
        if (!InstallGuestRows(ctx, base, list)) return false;
    } else {
        uint32_t current = 0;
        if (!CurrentPauseList(controller.u32, current) || current != list) return false;
    }
    const int index = SelectedHostRow(list);
    if (index < 0) return false;

    MenuRow row{};
    {
        std::lock_guard<std::mutex> lock(s_rowsMutex);
        if (static_cast<size_t>(index) >= s_builtRows.size()) return false;
        row = s_builtRows[static_cast<size_t>(index)];
    }
    if (row.cvar) {
        CycleRow(static_cast<size_t>(index));
        PollRows();
        UpdateGuestRowLabels(ctx, base);
        return true;
    }
    return ApplyRowAction(row.action, ctx, base);
}

bool mcla_menu_cancel(PPCContext& ctx, uint8_t* base) {
    if (!MclaMenuEnabled()) return false;
    const int menu = s_activeMenu.load();
    if (menu <= kMenuPause) {
        RestoreGuestRows();
        s_activeMenu.store(-1);
        return false;  // at Pause: let the guest close it
    }
    if (s_installedList.load() == 0) return false;
    const int previous = s_activeMenu.load();
    MclaMenuSetActive(kMenuPause);
    PollRows();
    if (!InstallGuestRows(ctx, base, s_installedList.load())) {
        MclaMenuSetActive(previous);
        PollRows();
        return false;
    }
    return true;
}

bool mcla_menu_null_table_guard(PPCRegister table) {
    if (!MclaMenuEnabled() || !MclaMenuActive() || s_installedList.load() == 0) return false;
    return table.u32 == 0; // exact 0x82661290 exit condition (TU 131 :12981-12985)
}

bool mcla_menu_list_vtable_guard(PPCRegister list) {
    if (!MclaMenuEnabled() || !MclaMenuActive() || list.u32 != s_installedList.load()) return false;
    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
    uint32_t vt = 0;
    if (list.u32 == 0 || !mem.ReadU32BE(list.u32 + 0u, &vt) || vt == 0) return true;
    // The indirect call at 0x8265F0F0 is [ [r31+0] + 648 ].
    uint32_t fn = 0;
    if (!mem.ReadU32BE(vt + 648u, &fn)) return true;
    return fn < 0x82000000u || fn >= 0x82A00000u;
}

// ---------------------------------------------------------------------------
// PPC_FUNC entry wrappers (spec §1.3, §1.6). All declared in hooks.h by worker 2.
// ---------------------------------------------------------------------------
PPC_FUNC(sub_82220308) {
    if (MclaMenuEnabled() && MclaMenuActive()) {
        const uint32_t settings = s_settingsState.load();
        if (settings != 0 && ctx.r3.u32 == settings) {
            const uint32_t replacement = MclaMenuStateFor(s_activeMenu.load());
            if (replacement != 0) ctx.r3.u64 = replacement;
        }
    }
    __imp__sub_82220308(ctx, base);
}

PPC_FUNC(sub_82631FD0) {
    const uint32_t list = ctx.r3.u32;
    s_lastList.store(list);
    MclaMenuTick(list);
    if (MclaMenuEnabled() && MclaMenuActive() && s_installedList.load() == list)
        UpdateGuestRowLabels(ctx, base);
    __imp__sub_82631FD0(ctx, base);
}

PPC_FUNC(sub_82630E40) {
    if (MclaMenuEnabled()) s_lastTabStrip.store(ctx.r3.u32);
    __imp__sub_82630E40(ctx, base);
}

// Pause-controller update. The vtable check is the measured identity guard:
// raw vtable 0x820A1FB0 starts with sub_82660310 and has sub_82668660 at +16.
PPC_FUNC(sub_82660310) {
    const uint32_t controller = ctx.r3.u32;
    uint32_t vt = 0;
    const bool validController = MclaMenuEnabled() && controller != 0 &&
                                 ReadGuest32(controller, vt) && vt == kPauseControllerVtable;
    if (!MclaMenuEnabled()) {
        RestoreGuestRows();
        s_activeMenu.store(-1);
        __imp__sub_82660310(ctx, base);
        return;
    }
    if (validController) {
        s_pauseController.store(controller);
        if (s_activeMenu.load() < 0) {
            s_activeMenu.store(kMenuPause);
            PollRows();
        }
        uint32_t list = 0;
        if (CurrentPauseList(controller, list)) (void)InstallGuestRows(ctx, base, list);
    }
    __imp__sub_82660310(ctx, base);
    if (validController) {
        uint32_t list = 0;
        if (CurrentPauseList(controller, list)) (void)InstallGuestRows(ctx, base, list);
    }
}

// Fixed-list key handler. Keys 51..54 are the guest's left/right family
// (0x82633544..0x82633550 all jump to 0x826336CC). For host-owned rows we cycle
// the CVar here instead of entering the guest row's value-list callback.
PPC_FUNC(sub_826334D0) {
    const uint32_t list = ctx.r3.u32;
    const uint32_t key = ctx.r4.u32 & 0xFFu;
    if (MclaMenuEnabled() && s_installedList.load() == list && key >= 51u && key <= 54u) {
        const int index = SelectedHostRow(list);
        if (index >= 0) {
            CycleRow(static_cast<size_t>(index));
            PollRows();
            UpdateGuestRowLabels(ctx, base);
            ctx.r3.u64 = 1;
            return;
        }
    }
    __imp__sub_826334D0(ctx, base);
}

// Key '3'..'6' interception needs NodeReachesList() plus the vtable+32 range
// check of spec §1.3. Neither has a measured guest offset here, so this wrapper
// is an exact passthrough rather than a guessed early return.
PPC_FUNC(sub_826309E0) {
    __imp__sub_826309E0(ctx, base);
}

// Depth of field: zero the 16-byte CoC block at [r3+0xF0] only when the CVar is
// registered and explicitly false. Absent CVar -> original behaviour.
PPC_FUNC(sub_8260EBB8) {
    const auto& cv = mcla::cvar::CVarSystem::Instance();
    if (MclaMenuEnabled() && cv.Has("mcla_depth_of_field") &&
        !cv.GetBool("mcla_depth_of_field") && ctx.r3.u32 != 0) {
        const uint8_t zeros[16] = {};
        (void)mcla::kernel::GuestMemoryHeap::Instance().WriteBytes(ctx.r3.u32 + 0xF0u, zeros, 16u);
    }
    __imp__sub_8260EBB8(ctx, base);
}

// Live motion blur. The launcher's MCLA_MOTION_BLUR is consumed into the CVar
// at startup by App::BridgeLauncherEnvToCvars; if the bridge is disabled the
// env remains an explicit override. Measured chain (IDA + generated TU 123):
// sub_8260E9A0 is the StreakMotionBlur pass; it ends in sub_8260D678(rec,
// [rec+56], [rec+92], tech [rec+3392]=StreakMotionBlur, var [rec+132]=FullResMap,
// 0). Its only consumer is sub_8260FAA0 mode 2, which binds [rec+56] as
// RenderMap. With blur off: skip the streak pass and point [rec+56] at the
// unblurred scene [rec+92] for the duration of the tonemap, then restore.
static bool MotionBlurDisabled() {
    if (const char* e = std::getenv("MCLA_MOTION_BLUR")) return e[0] == '0';
    return !mcla::cvar::CVarSystem::Instance().GetBool("mcla_motion_blur");
}

PPC_FUNC(sub_8260FAA0) {
    auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
    const uint32_t rec = ctx.r3.u32;
    uint32_t blurRt = 0, sceneRt = 0;
    const bool swap = MotionBlurDisabled() && rec != 0 && mem.ReadU32BE(rec + 56u, &blurRt) &&
                      mem.ReadU32BE(rec + 92u, &sceneRt) && sceneRt != 0 &&
                      mem.WriteU32BE(rec + 56u, sceneRt);
    __imp__sub_8260FAA0(ctx, base);
    if (swap) (void)mem.WriteU32BE(rec + 56u, blurRt);
}

PPC_FUNC(sub_8260E9A0) {
    if (MotionBlurDisabled() && ctx.r3.u32 != 0) {
        static std::atomic<uint32_t> s_n{0};
        if (s_n.fetch_add(1) == 0)
            MCLA_LOG_INFO("MOTION-BLUR: off -- sub_8260E9A0 streak pass skipped, "
                          "sub_8260FAA0 RenderMap [rec+56] -> scene [rec+92]");
        return;
    }
    const uint32_t obj = g_wave3_blurObj;
    if (MclaMenuEnabled() && obj != 0 && ctx.r3.u32 == obj) {
        auto& mem = mcla::kernel::GuestMemoryHeap::Instance();
        const bool blur = mcla::cvar::CVarSystem::Instance().GetBool("mcla_motion_blur");
        (void)mem.WriteU32BE(obj + 3400u, blur ? g_wave3_savedTintFade : 0u);
        (void)mem.WriteU32BE(obj + 3404u, blur ? g_wave3_savedDesatScale : 0u);
    }
    __imp__sub_8260E9A0(ctx, base);
}
