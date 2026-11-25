// SPDX-License-Identifier: Apache-2.0
#include "../fps60.h"

namespace {
using consume_fn = void (__thiscall*)(void*);

uint8_t* volatile* app = nullptr;
volatile uint32_t* frame_counter = nullptr;
consume_fn consume_frame = nullptr;
uint32_t last_group = 0;
int last_rate = 0;

constexpr size_t map_queue_offset = 0xE28;
constexpr size_t frames_shown_from_queue = 0xC0;

void __fastcall finish_frame(void* queue) {
    auto* map_app = *app;
    if (map_app && queue == map_app + map_queue_offset) {
        const int rate = g_disabled || !pacing_active() ? 1 : rate_mult();
        if (rate > 1) {
            // This runs on the upload worker. Read the host frame instead
            // of the game thread's per-invocation render/logic flag.
            const uint32_t group = *frame_counter / static_cast<uint32_t>(rate);
            if (last_rate == rate && group == last_group) {
                // The upload has already completed and the queue still owns
                // this sample. Undo the caller's display count increment
                // while replaying the current sample on the extra frames.
                --*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(queue) + frames_shown_from_queue);
                return;
            }
            last_group = group;
        }
        last_rate = rate;
    }
    consume_frame(queue);
}

bool install() {
    auto* job = sig_find_unique("MAPVIDEO-001 field video upload job",
        "E8 ?? ?? ?? ?? 84 C0 74 0C A1 ?? ?? ?? ?? C6 80 D1 0D 00 00 01 "
        "8B 46 14 56 C7 00 00 00 00 00", 0x672096);
    if (!job) return false;

    auto* upload = call_target(job);
    app = reinterpret_cast<uint8_t* volatile*>(read_abs(job + 10));
    frame_counter = reinterpret_cast<volatile uint32_t*>(sym_rt("frame_counter"));
    if (!upload || !app || !frame_counter ||
        !sig_match(upload + 0x386, "FF 86 00 01 00 00 8D 4E 40 E8 ?? ?? ?? ?? 8D 4D 8C")) {
        log_line("MAPVIDEO-001: unexpected video upload layout - refused");
        return false;
    }

    auto* finish = upload + 0x38F;
    auto* consume = call_target(finish);
    if (!consume || !sig_match(consume, "8B 51 14 FF 41 34 89 51 08 39 51 0C")) {
        log_line("MAPVIDEO-001: unexpected video queue consumer - refused");
        return false;
    }

    consume_frame = reinterpret_cast<consume_fn>(consume);
    return patch_call_rel32(finish, consume, reinterpret_cast<void*>(&finish_frame));
}
}  // namespace

Row row_MAPVIDEO001 = {
    "MAPVIDEO-001",
    "in-world video textures advance at stock speed and upload every render",
    1, install, nullptr, nullptr, 0
};
