// SPDX-License-Identifier: Apache-2.0
// Tests for X2-UI-018 (the Garment Grid UI's per-draw animation state is written back after render-only draws). The
// row's .cpp is included so its static helpers can be called.
#include "test.h"
#include "fakes.h"
#include "../../src/x2/rows/X2-UI-018.cpp"

using namespace x2;

static const size_t TABLE = 0x1D0;   // window table offset in the UI block (0xDF5F10 - 0xDF5D40)

TEST(ui018_restores_clocks_and_pulse_but_not_the_window_table) {
    static uint8_t ui[UI_BLOCK_LEN], pulse[PULSE_BLOCK_LEN];
    static int32_t win_a = 5;
    memset(ui, 0, sizeof ui); memset(pulse, 0, sizeof pulse);
    uint8_t** table = (uint8_t**)(ui + TABLE);
    table[3] = (uint8_t*)&win_a;
    *(int32_t*)(ui + 0x1C0) = 100;                        // clock 0xDF5F00
    pulse[0x1C] = 0x40;                                   // pulse 0xD47D0C
    static UiSnapshot s;
    memcpy(s.ui, ui, UI_BLOCK_LEN); memcpy(s.pulse, pulse, PULSE_BLOCK_LEN);
    for (int i = 0; i < WINDOW_SLOTS; i++) { s.window[i] = table[i]; s.window_counter[i] = table[i] ? *(int32_t*)table[i] : 0; }
    // the draw: clocks and counters step, the pulse moves, window slot 7 is opened and slot 3's window counts
    *(int32_t*)(ui + 0x1C0) = 101;
    pulse[0x1C] = 0x46;
    win_a = 6;
    static int32_t win_b = 0;
    table[7] = (uint8_t*)&win_b;
    CHECK_EQ(restore_ui(s, ui, TABLE, pulse), 1);
    CHECK_EQ(*(int32_t*)(ui + 0x1C0), 100);
    CHECK_EQ(pulse[0x1C], 0x40);
    CHECK_EQ(win_a, 5);
    CHECK(table[7] == (uint8_t*)&win_b);                  // the table is never written back
}

TEST(ui018_leaves_a_closed_or_replaced_window_alone) {
    static uint8_t ui[UI_BLOCK_LEN], pulse[PULSE_BLOCK_LEN];
    static int32_t win_a = 9, win_c = 1;
    memset(ui, 0, sizeof ui);
    uint8_t** table = (uint8_t**)(ui + TABLE);
    static UiSnapshot s;
    memset(&s, 0, sizeof s);
    s.window[0] = (uint8_t*)&win_a; s.window_counter[0] = 3;   // slot 0 closed by the draw (now null)
    s.window[1] = (uint8_t*)&win_a; s.window_counter[1] = 3;   // slot 1 now holds another window
    table[1] = (uint8_t*)&win_c;
    CHECK_EQ(restore_ui(s, ui, TABLE, pulse), 0);
    CHECK_EQ(win_a, 9);
    CHECK_EQ(win_c, 1);
}
