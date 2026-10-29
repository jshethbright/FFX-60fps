// SPDX-License-Identifier: Apache-2.0
// Tests for X2-DIAG-PPPRATE (which particle call sites still advance on render-only invocations, i.e. run 2x). The
// row's .cpp is included so its static helpers can be called.
#include "test.h"
#include "fakes.h"
#include "../../src/x2/rows/X2-DIAG-PPPRATE.cpp"

using namespace x2;

TEST(ppprate_counts_advances_per_invocation_kind) {
    StepSite s = {};
    int prog = 0;
    count_step(s, INV_LOGIC, &prog, 0, 0x1000);
    count_step(s, INV_RENDER_ONLY, &prog, 0x1000, 0x1000);        // paused: no advance
    count_step(s, INV_LOGIC, &prog, 0x1000, 0x2000);
    CHECK_EQ(s.logic_steps, 2);
    CHECK_EQ(s.logic_advanced, 2);
    CHECK_EQ(s.render_steps, 1);
    CHECK_EQ(s.render_advanced, 0);
    CHECK_EQ(s.programs, 1);
    CHECK(!site_runs_double(s));
}

TEST(ppprate_flags_a_site_that_advances_on_render_only_frames) {
    StepSite s = {};
    int a = 0, b = 0;
    for (int i = 0; i < 30; i++) {
        count_step(s, INV_LOGIC, &a, i * 2, i * 2 + 1);
        count_step(s, INV_RENDER_ONLY, &a, i * 2 + 1, i * 2 + 2);
    }
    CHECK(site_runs_double(s));
    count_step(s, INV_LOGIC, &b, 0, 1);
    CHECK_EQ(s.programs, 2);
}

TEST(ppprate_ignores_a_rare_render_only_advance) {
    StepSite s = {};
    int a = 0;
    for (int i = 0; i < 30; i++) count_step(s, INV_LOGIC, &a, i, i + 1);
    count_step(s, INV_RENDER_ONLY, &a, 30, 31);                     // one fresh first step among 30 logic steps
    CHECK(!site_runs_double(s));
}
