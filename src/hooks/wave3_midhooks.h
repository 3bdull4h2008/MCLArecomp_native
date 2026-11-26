#pragma once

// Wave 3 settings-menu midhook helpers (spec §4.1 / §1.3).
//
// These functions are called from C++ injected by tools/emit_dropped_ops.py into
// generated/ppc_xenon/ppc_recomp.<TU>.cpp. They have C++ linkage and their
// definitions live in src/hooks/wave3_menu.cpp. Every helper is gated so that,
// with the feature off, it returns a value that leaves the guest's original
// instruction sequence byte-for-byte equivalent.
//
// No rex:: APIs. All guest memory access is through GuestMemoryHeap::Instance()
// checked, endian-explicit helpers.
//
// Injector signatures (exact call forms expected by §4.1):
//   mcla_menu_capture_continue(ctx.r3);
//   if (!mcla_menu_enable_save(ctx, base)) ctx.r4.s64 = 0;
//   if (mcla_menu_click(ctx, base, r31)) { ctx.r3.s64 = 1; ctx.r1.s64 += 768; return; }
//   if (mcla_menu_cancel(ctx, base)) goto loc_826698AC;
//   if (mcla_menu_null_table_guard(r30)) { ctx.r3.s64 = 0; ctx.r1.s64 += 112; return; }
//   if (mcla_menu_list_vtable_guard(r31)) goto loc_8265F108;

#include "ppc_context.h"

#include <cstdint>

// --- Midhook entry points (defined in src/hooks/wave3_menu.cpp) -------------

// 0x82660644 (TU 131): remember the Pause-tab node (reads [r3+32]).
void mcla_menu_capture_continue(PPCRegister tabNode);

// 0x82660C2C (TU 131): true keeps the Save row enabled; false leaves guest's
// original `li r4,0` in force.
bool mcla_menu_enable_save(PPCContext& ctx, uint8_t* base);

// 0x826686B8 (TU 132): true means this menu consumed the accept/click.
bool mcla_menu_click(PPCContext& ctx, uint8_t* base, PPCRegister node);

// 0x82669864 (TU 132): true means this menu consumed the cancel/back.
bool mcla_menu_cancel(PPCContext& ctx, uint8_t* base);

// 0x8266126C (TU 131): true when the table pointer is unusable (guest skips
// its dereference and returns 0, matching the 0x82661290 exit).
bool mcla_menu_null_table_guard(PPCRegister table);

// 0x8265F0F0 (TU 131): true when the indirect call target at [list+0]/+648 is
// not a plausible guest code pointer (guest takes the loc_8265F108 path).
bool mcla_menu_list_vtable_guard(PPCRegister list);

// --- Host-side state shared with the PPC_FUNC wrappers ---------------------
// All values are guest VAs (0 = unknown). Kept here so the entry wrappers in
// wave3_menu.cpp and any injector preamble agree on one owner.

bool MclaMenuActive();                 // false when no in-game submenu is open
uint32_t MclaMenuContinueTab();        // captured at 0x82660644
uint32_t MclaMenuSettingsState();      // captured at 0x82660C2C
uint32_t MclaMenuPauseList();          // captured from sub_82631FD0
uint32_t MclaMenuLastList();           // last list seen by sub_82631FD0
uint32_t MclaMenuLastTabStrip();       // last strip seen by sub_82630E40
uint32_t MclaMenuStateFor(int index);  // replacement r3 for sub_82220308 (0 = no swap)
void MclaMenuSetPauseList(uint32_t list);
void MclaMenuSetLastList(uint32_t list);
void MclaMenuSetLastTabStrip(uint32_t strip);
void MclaMenuSetActive(int index);
void MclaMenuTick(uint32_t list);      // PollRows/InstallRows/WatchLayout hook

// Live motion-blur companion state. Worker 3 (src/gpu_device.cpp, inside its
// existing PPC_FUNC(sub_8260C8E0)) writes these; wave3_menu.cpp's
// PPC_FUNC(sub_8260E9A0) reads them to apply the live On/Off toggle. Zero means
// "not captured yet", in which case the companion wrapper is a pure passthrough.
extern uint32_t g_wave3_blurObj;
extern uint32_t g_wave3_savedTintFade;
extern uint32_t g_wave3_savedDesatScale;
