// SPDX-License-Identifier: Apache-2.0
// UI-001: screen fades (incl. the lightning flash) step inside their draw call, which also runs on
// render-only invocations, so fades last half as long. Fix: on those invocations let the fade draw, then
// restore its state, so it steps once per logic tick. Draw the intermediate
// opacity between native updates without changing the timers or fade outputs.
// Details: docs/fps60/rows/UI-001.md
#include "../fps60.h"
#include <string.h>

typedef void(__cdecl* fade_t)(void);

static fade_t   g_fade = nullptr;
static uint8_t* g_state = nullptr;    // active mask + 5 slots (+ two read-only dwords)
static uint8_t* g_out = nullptr;      // flag, flag, latch, progress float
static uint8_t* g_out2 = nullptr;     // fade type output
static const volatile uint32_t* g_capture_pending = nullptr;
static const uint8_t* g_screen_filter = nullptr;
static const uint32_t STATE_LEN = 0x58, OUT_LEN = 0x10, OUT2_LEN = 4;
static volatile LONG g_held = 0, g_passed = 0;
static int g_half = 0;                // UI-001-HALF: render-only frames draw the half step (60 Hz promotion)

using overlay_t = void (__cdecl*)(float, float, float, float, int);
using crossfade_t = void (__cdecl*)(int);
static overlay_t g_draw_overlay = nullptr;
static crossfade_t g_draw_crossfade = nullptr;

struct FadeSample {
    uint8_t state[0x10];
    float rgba[4];
    int crossfade_alpha;
    bool has_overlay;
    bool has_crossfade;
};

static FadeSample g_samples[5];
static bool g_record = false;
static unsigned g_continuing = 0;
static float g_fraction = 0.0f;

static FadeSample* fade_sample(const uint8_t* countdown) {
    const uintptr_t offset = reinterpret_cast<uintptr_t>(countdown) - reinterpret_cast<uintptr_t>(g_state + 0xC);
    if (offset >= 5 * 0x10 || offset % 0x10 != 0) return nullptr;
    const unsigned index = static_cast<unsigned>(offset / 0x10);
    return g_record || (g_continuing & (1u << index)) ? &g_samples[index] : nullptr;
}

static float fade_between(float from, float to) {
    return from + g_fraction * (to - from);
}

static void __cdecl draw_overlay(float r, float g, float b, float a, int mode, const uint8_t* countdown) {
    // Modes 7 and 8 are immediate filters. They share the native draw call
    // with one fade branch but both must take effect on the same frame.
    FadeSample* sample = mode == 0 ? fade_sample(countdown) : nullptr;
    if (sample && (countdown[-4] < 2 || countdown[-4] > 6)) sample = nullptr;
    float rgba[4] = {r, g, b, a};
    if (sample && g_record) {
        memcpy(sample->rgba, rgba, sizeof(rgba));
        sample->has_overlay = true;
    } else if (sample && sample->has_overlay) {
        for (int i = 0; i < 4; ++i) rgba[i] = fade_between(sample->rgba[i], rgba[i]);
    }
    g_draw_overlay(rgba[0], rgba[1], rgba[2], rgba[3], mode);
}

static void __cdecl draw_crossfade(int alpha, const uint8_t* countdown) {
    FadeSample* sample = fade_sample(countdown);
    if (sample && g_record) {
        sample->crossfade_alpha = alpha;
        sample->has_crossfade = true;
    } else if (sample && sample->has_crossfade) {
        alpha = static_cast<int>(fade_between(static_cast<float>(sample->crossfade_alpha), static_cast<float>(alpha)) + 0.5f);
    }
    g_draw_crossfade(alpha);
}

// The native loop keeps the countdown pointer in ESI. Pass it as an extra
// argument to distinguish multiple fade slots even if they share a draw call.
__declspec(naked) static void h_draw_overlay() {
    __asm {
        push ebp
        mov ebp, esp
        push esi
        push dword ptr [ebp + 24]
        push dword ptr [ebp + 20]
        push dword ptr [ebp + 16]
        push dword ptr [ebp + 12]
        push dword ptr [ebp + 8]
        call draw_overlay
        add esp, 24
        pop ebp
        ret
    }
}

__declspec(naked) static void h_draw_crossfade() {
    __asm {
        push esi
        push dword ptr [esp + 8]
        call draw_crossfade
        add esp, 8
        ret
    }
}

bool ui001_frame_capture_pending() {
    return g_capture_pending && *g_capture_pending != 0;
}

uint64_t ui001_screen_filter() {
    // Types 7 and 8 are immediate subtractive/alternate color filters. The
    // renderer reads their mode, RGB and strength directly from this slot.
    if (!g_screen_filter || (g_state[0] & 8) == 0 ||
        (g_screen_filter[0] != 7 && g_screen_filter[0] != 8)) return 0;
    uint64_t filter = 0;
    memcpy(&filter, g_screen_filter, sizeof(filter));
    return (filter & 0x0000FFFF00000000ull) ? filter : 0;  // +4: active word
}

// UI-001-HALF: rescale each active slot so the original call draws the midpoint between two logic ticks
// (the state is restored afterwards). Per-type formulas: docs/fps60/rows/UI-001.md.
static void half_step() {
    for (int n = 0; n < 5; n++) {
        uint8_t* slot = g_state + 8 + n * 0x10;
        int16_t* c = (int16_t*)(slot + 4);
        int16_t* t = (int16_t*)(slot + 6);
        int16_t* step = (int16_t*)(slot + 0xA);
        if (*c == 0) continue;
        switch (slot[0]) {
        case 0: *step = (int16_t)(*step / 2); break;
        case 2: case 3: case 5:
            if (*c > 0 && *t > 0 && *t <= 0x3FFF && *c <= 0x3FFF) { *c = (int16_t)(*c * 2); *t = (int16_t)(*t * 2); }
            break;
        case 4: case 6:
            if (*c > 0 && *t > 0 && *t <= 0x3FFF && *c < 0x3FFF) { *c = (int16_t)(*c * 2 + 1); *t = (int16_t)(*t * 2); }
            break;
        default: break;
        }
    }
}

static void __cdecl h_fade() {
    const int kind = pacing_kind();
    const bool enabled = row_UI001.on && !g_disabled;
    const bool smooth = g_draw_overlay || g_draw_crossfade;
    if (!enabled || kind != INV_BUILD) {
        InterlockedIncrement(&g_passed);
        memset(g_samples, 0, sizeof(g_samples));
        g_record = enabled && smooth && (kind == INV_LOGIC || kind == INV_BACKLOG);
        g_fade();
        if (g_record) {
            for (int i = 0; i < 5; ++i) memcpy(g_samples[i].state, g_state + 8 + i * 0x10, 0x10);
        }
        g_record = false;
        return;
    }
    uint8_t s[STATE_LEN], o[OUT_LEN], o2[OUT2_LEN];
    memcpy(s, g_state, STATE_LEN);
    memcpy(o, g_out, OUT_LEN);
    memcpy(o2, g_out2, OUT2_LEN);
    if (g_half) half_step();
    if (smooth && !ui001_frame_capture_pending()) {
        // The logic frame already rendered the first endpoint. The following
        // render-only frames approach the next native tick without advancing it.
        const int pass = pacing_pass();
        g_fraction = pacing_t() * static_cast<float>(pass) / static_cast<float>(pass + 1);
        for (int i = 0; i < 5; ++i) {
            const auto& sample = g_samples[i];
            if ((sample.has_overlay || sample.has_crossfade) && memcmp(sample.state, s + 8 + i * 0x10, 0x10) == 0)
                g_continuing |= 1u << i;
        }
    }
    g_fade();
    g_continuing = 0;
    memcpy(g_state, s, STATE_LEN);
    memcpy(g_out, o, OUT_LEN);
    memcpy(g_out2, o2, OUT2_LEN);
    InterlockedIncrement(&g_held);
}

static void install_fade_draws(uint8_t* update) {
    uint8_t* sites[] = {update + 0x1C9, update + 0x268, update + 0x363, update + 0x3E0, update + 0x450};
    uint8_t* overlay = call_target(sites[0]);
    if (!sig_match(overlay, "55 8B EC D9 45 14 68 01 10 00 00 FF 75 18")) return;
    for (uint8_t* site : sites)
        if (call_target(site) != overlay) return;
    uint8_t* cross_site = update + 0x4D6;
    uint8_t* crossfade = call_target(cross_site);
    if (!sig_match(cross_site - 2, "98 50 E8 ?? ?? ?? ?? 83 C4 04 D9 E8")) return;
    if (!sig_match(crossfade, "55 8B EC E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 08 81 E1")) return;

    g_draw_overlay = reinterpret_cast<overlay_t>(overlay);
    g_draw_crossfade = reinterpret_cast<crossfade_t>(crossfade);
    for (unsigned i = 0; i < sizeof(sites) / sizeof(sites[0]); ++i) {
        if (!patch_call_rel32(sites[i], overlay, (void*)&h_draw_overlay)) {
            for (unsigned j = 0; j < i; ++j) patch_call_rel32(sites[j], (void*)&h_draw_overlay, overlay);
            log_line("  UI-001: overlay fade draw hooks could not be installed");
            return;
        }
    }
    if (!patch_call_rel32(cross_site, crossfade, (void*)&h_draw_crossfade))
        log_line("  UI-001: captured-frame fade draw hook could not be installed");
}

static bool install() {
    // Sg_MainLoop tail: the call to Sg_ScreenFadeUpdate (third CALL)
    uint8_t* at = sig_find_unique("UI-001 Sg_ScreenFadeUpdate call (Sg_MainLoop tail)",
        "6A 07 E8 ?? ?? ?? ?? 83 C4 08 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 80 3D", 0x8214E0);
    if (!at) return false;
    uint8_t* site = at + 0x0A;
    uint8_t* fn = call_target(site);
    // Sg_ScreenFadeUpdate prologue: two calls, then the active-mask test
    if (!sig_match(fn, "55 8B EC 83 EC 0C E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 3D")) {
        log_line("  UI-001: CALL target %p is not Sg_ScreenFadeUpdate (prologue mismatch) - refused", fn);
        return false;
    }
    uint32_t mask = *(uint32_t*)(fn + 0x12);
    uint32_t slots = 0;
    // MOV ESI,imm32 with imm == mask+8 (slot 0 countdown)
    for (uint32_t i = 0x20; i < 0x200; i++)
        if (fn[i] == 0xBE && *(uint32_t*)(fn + i + 1) == mask + 8) { slots = mask + 8; break; }
    // MOV [out2],EAX; MOV dword [out],1; FSTP [out+0xC]
    uint32_t out2 = 0, out = 0;
    for (uint32_t i = 0x100; i + 21 <= 0x600; i++) {
        const uint8_t* p = fn + i;
        if (p[0] == 0xA3 && p[5] == 0xC7 && p[6] == 0x05 && *(uint32_t*)(p + 11) == 1 && p[15] == 0xD9 && p[16] == 0x1D &&
            *(uint32_t*)(p + 17) == *(uint32_t*)(p + 7) + 0xC) {
            out2 = *(uint32_t*)(p + 1); out = *(uint32_t*)(p + 7); break;
        }
    }
    if (!slots || !out || !out2) {
        log_line("  UI-001: fade globals not found (mask=0x%08X slots=0x%08X out=0x%08X out2=0x%08X) - refused",
                 mask, slots, out, out2);
        return false;
    }
    g_state = (uint8_t*)(uintptr_t)mask;
    g_out = (uint8_t*)(uintptr_t)out;
    g_out2 = (uint8_t*)(uintptr_t)out2;
    g_fade = (fade_t)fn;
    g_half = config_row("UI-001-HALF", 0);
    if (!patch_call_rel32(site, fn, (void*)&h_fade)) {
        log_line("  UI-001: patch failed");
        return false;
    }
    if (!g_half && config_row("UI-001-SMOOTH", 1)) install_fade_draws(fn);
    // The immediately following call consumes the ATEL framebuffer request after the actors,
    // camera and fade have drawn. It clears the request after storing the framebuffer.
    const uint8_t* capture = call_target(site + 5);
    if (capture && sig_match(capture, "83 3D ?? ?? ?? ?? 00 0F 84 D9 00 00 00 56 68") &&
        sig_match(capture + 0xD6, "C7 05 ?? ?? ?? ?? 00 00 00 00 5E E9") &&
        read_abs(capture + 2) == read_abs(capture + 0xD8))
        g_capture_pending = (const volatile uint32_t*)read_abs(capture + 2);
    const uint8_t* filter = sig_find_unique("UI-001 immediate screen filter setter",
        "55 8B EC 8B 4D 14 83 0D ?? ?? ?? ?? 08 C6 05 ?? ?? ?? ?? 07 85 C9 74 2C",
        0x82CC20);
    if (filter && read_abs(filter + 8) == g_state && read_abs(filter + 15) == g_state + 0x34)
        g_screen_filter = g_state + 0x34;
    log_line("  UI-001: CALL at gva=0x%08X -> stub; Sg_ScreenFadeUpdate gva=0x%08X, state gva=0x%08X+0x%X "
             "(expected 0x01300704), outputs gva=0x%08X (0x012FFAE8), 0x%08X (0x00C496D0): render-only invocations draw, state restored; UI-001-HALF=%d",
             rt_to_gva_exe(site), rt_to_gva_exe(fn), rt_to_gva_exe(g_state), STATE_LEN, rt_to_gva_exe(g_out), rt_to_gva_exe(g_out2), g_half);
    return true;
}

static void post() {
    static LONG reported = 0;
    if (!reported && g_held > 0) {
        reported = 1;
        log_line("UI-001: first render-only fade update held (held=%ld passed=%ld)", g_held, g_passed);
    }
}

Row row_UI001 = {
    "UI-001",
    "screen fades (Sg_ScreenFadeUpdate, incl. lightning flash) step once per logic tick; render-only frames draw with state restored",
    1, install, nullptr, post, 0
};
