// SPDX-License-Identifier: Apache-2.0
#include "../fps60.h"

static bool install() {
    // The native field-menu pointer draw tests bit 1 of an 8-bit menu counter
    // only for callers that request a flashing pointer. At 120 FPS the counter
    // ticks at 60 Hz, so the equipment-slot pointer blinks 15 times per second.
    auto* draw = sig_find_unique("UI-MENU-001 field-menu pointer draw",
        "55 8B EC 83 EC 08 E8 D5 96 FE FF A8 ?? 74 0A 83 7D 10 00 0F 85 C8 00 00 00",
        0x8C0580);
    if (!draw) return false;

    auto* mask = draw + 12;
    constexpr uint8_t slow_mask = 0x10;  // 32 counter ticks per full blink
    if (*mask == slow_mask) return true;  // an existing standalone fix may already have applied it
    if (*mask != 0x02) {
        log_line("  UI-MENU-001: unexpected blink mask 0x%02X - refused", (unsigned)*mask);
        return false;
    }
    return patch_write(mask, &slow_mask, sizeof(slow_mask));
}

Row row_UIMENU001 = {
    "UI-MENU-001",
    "slow the blink of flashing field-menu pointers",
    1, install, nullptr, nullptr, 0
};
