// SPDX-License-Identifier: Apache-2.0
#include "../fps60.h"

namespace {
using status_draw_fn = void (__cdecl*)(int window, int x, int y);
using scan_draw_fn = void (__cdecl*)(int x, int y);

uint8_t* status_entry = nullptr;
uint8_t* scan_entry = nullptr;
uint8_t* ally_step = nullptr;
uint8_t* ally_draw = nullptr;
void* original_ally_step = nullptr;
volatile uint16_t* status_ticks = nullptr;
volatile uint16_t* scan_ticks = nullptr;
status_draw_fn original_status = nullptr;
scan_draw_fn original_scan = nullptr;

template<typename Draw, typename... Args>
void draw_with_stock_timer(volatile uint16_t* ticks, Draw draw, Args... args) {
    if (g_disabled || pacing_kind() != INV_BUILD) {
        draw(args...);
        return;
    }

    // The native draw advances the list only if the countdown starts at zero.
    // Keep drawing the current entry between logic ticks, including at zero.
    const uint16_t saved = *ticks;
    *ticks = 1;
    draw(args...);
    *ticks = saved;
}

void __cdecl draw_status(int window, int x, int y) {
    draw_with_stock_timer(status_ticks, original_status, window, x, y);
}

void __cdecl draw_scan(int x, int y) {
    draw_with_stock_timer(scan_ticks, original_scan, x, y);
}

bool __cdecl advance_ally_status() {
    return g_disabled || pacing_kind() != INV_BUILD;
}

__declspec(naked) void ally_status_step() {
    __asm {
        pushfd
        pushad
        call advance_ally_status
        test al, al
        jz draw_current
        popad
        popfd
        jmp dword ptr [original_ally_step]
    draw_current:
        popad
        popfd
        jmp dword ptr [ally_draw]
    }
}

volatile uint16_t* countdown(uint8_t* step) {
    // MOV AX,[countdown]; MOV CX,AX; DEC AX; MOV [countdown],AX;
    // TEST CX,CX; JNZ; MOV EAX,20; MOV [countdown],AX.
    if (!sig_match(step, "66 A1 ?? ?? ?? ?? 66 8B C8 66 48 66 A3 ?? ?? ?? ?? "
                         "66 85 C9 75 ?? B8 14 00 00 00 66 A3 ?? ?? ?? ??")) {
        return nullptr;
    }
    auto* timer = read_abs(step + 2);
    if (!timer || read_abs(step + 13) != timer || read_abs(step + 29) != timer) {
        return nullptr;
    }
    return reinterpret_cast<volatile uint16_t*>(timer);
}

bool install() {
    status_entry = sig_find_unique("UI-ENEMYINFO enemy status draw",
        "55 8B EC 51 66 A1 ?? ?? ?? ?? 66 8B C8 66 48 66 A3 ?? ?? ?? ?? "
        "66 85 C9 75 2C B8 14 00 00 00 66 A3 ?? ?? ?? ??", 0x895240);
    if (status_entry) status_ticks = countdown(status_entry + 4);

    scan_entry = sig_find_unique("UI-ENEMYINFO Scan panel draw",
        "55 8B EC 83 EC 18 6A 00 6A 00 8D 45 FC 50 8D 45 F8 50 "
        "0F BF 05 ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 83 C4 04 50 6A 00", 0x89BF70);
    if (scan_entry) scan_ticks = countdown(scan_entry + 0x775);

    // The ally list holds its own countdown and index per party slot. Verify
    // the full step, including index wrap and the start of the drawing path.
    ally_step = sig_find_unique("UI-ENEMYINFO ally status step",
        "0F B7 4E 23 8D 41 FF 66 89 46 23 66 85 C9 7F 19 "
        "66 FF 46 25 B8 14 00 00 00 66 89 46 23 66 39 56 25 "
        "7C 06 33 C0 66 89 46 25 D9 E8 83 EC 08", 0x89711A);
    if (ally_step) ally_draw = ally_step + 0x29;

    if (!status_ticks) log_line("UI-ENEMYINFO: enemy status countdown was not found");
    if (!scan_ticks) log_line("UI-ENEMYINFO: Scan immunities countdown was not found");
    return status_ticks || scan_ticks || ally_step;
}

void pre() {
    if (original_status || original_scan || original_ally_step) return;
    bool status_ok = status_ticks && hook_entry("UI-ENEMYINFO enemy status draw",
        status_entry, (void*)&draw_status, (void**)&original_status);
    bool scan_ok = scan_ticks && hook_entry("UI-ENEMYINFO Scan immunities draw",
        scan_entry, (void*)&draw_scan, (void**)&original_scan);
    bool ally_ok = ally_step && hook_entry("UI-ENEMYINFO ally status step",
        ally_step, (void*)&ally_status_step, &original_ally_step);
    row_UIENEMYINFO.on = status_ok || scan_ok || ally_ok;
}
}  // namespace

Row row_UIENEMYINFO = {
    "UI-ENEMYINFO",
    "enemy, ally, and Scan immunity status text rotate at stock speed",
    1, install, pre, nullptr, 0
};
