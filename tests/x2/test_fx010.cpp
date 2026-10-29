// SPDX-License-Identifier: Apache-2.0
// Tests for X2-FX-010 (map particle programs paused on render-only invocations). The row's .cpp is included so its
// static helpers can be called.
#include "test.h"
#include "fakes.h"
#include "../../src/x2/rows/X2-FX-010.cpp"

using namespace x2;

TEST(fx010_pauses_started_programs_on_render_only_frames) {
    CHECK(field_step_should_pause(INV_RENDER_ONLY, 0x2000, 0x1000));
    CHECK(field_step_should_pause(INV_RENDER_ONLY, 0x1000, 0));      // advanced once: no longer fresh
}

TEST(fx010_leaves_logic_stock_and_backlog_steps_alone) {
    CHECK(!field_step_should_pause(INV_LOGIC, 0x2000, 0x1000));
    CHECK(!field_step_should_pause(INV_BACKLOG, 0x2000, 0x1000));
    CHECK(!field_step_should_pause(INV_STOCK, 0x2000, 0x1000));
}

TEST(fx010_fresh_program_steps_unpaused) {
    CHECK(!field_step_should_pause(INV_RENDER_ONLY, 0, 0));          // never advanced: its first draw needs real state
}

