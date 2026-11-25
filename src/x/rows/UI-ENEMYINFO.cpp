// SPDX-License-Identifier: Apache-2.0
#include "../fps60.h"

namespace {
using draw_fn = void (__cdecl*)(int window, int x, int y);

uint8_t* entry = nullptr;
volatile uint16_t* ticks = nullptr;
draw_fn original_draw = nullptr;

void __cdecl draw_status(int window, int x, int y) {
    if (g_disabled || pacing_kind() != INV_BUILD) {
        original_draw(window, x, y);
        return;
    }

    // The draw advances the status index only if the countdown starts at zero.
    // Draw the existing status on render-only passes, even when a logic tick
    // has just brought the countdown to zero.
    const uint16_t saved = *ticks;
    *ticks = 1;
    original_draw(window, x, y);
    *ticks = saved;
}

bool install() {
    entry = sig_find_unique("UI-ENEMYINFO enemy status draw",
        "55 8B EC 51 66 A1 ?? ?? ?? ?? 66 8B C8 66 48 66 A3 ?? ?? ?? ?? "
        "66 85 C9 75 2C B8 14 00 00 00 66 A3 ?? ?? ?? ??",
        0x895240);
    if (!entry) return false;

    auto* timer = read_abs(entry + 6);
    if (!timer || read_abs(entry + 17) != timer || read_abs(entry + 33) != timer) {
        log_line("UI-ENEMYINFO: inconsistent countdown operands - refused");
        return false;
    }
    ticks = reinterpret_cast<volatile uint16_t*>(timer);
    return true;
}

void pre() {
    if (original_draw) return;
    if (!hook_entry("UI-ENEMYINFO enemy status draw", entry, (void*)&draw_status, (void**)&original_draw)) {
        row_UIENEMYINFO.on = 0;
        log_line("UI-ENEMYINFO: draw hook failed - disabled");
    }
}
}  // namespace

Row row_UIENEMYINFO = {
    "UI-ENEMYINFO",
    "enemy Info window status text rotates at stock speed",
    1, install, pre, nullptr, 0
};
