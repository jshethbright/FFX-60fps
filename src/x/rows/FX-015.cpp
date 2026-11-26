// SPDX-License-Identifier: Apache-2.0
#include "../fps60.h"

namespace {
using PlayCue = void(__cdecl*)(int sound, int flags);
PlayCue play_cue = nullptr;

struct CueHeader {
    uint16_t marker;
    int16_t sound;
};
static_assert(sizeof(CueHeader) == 4);

void __cdecl play_optional_cue(const void* address, int) {
    // This sound-bank slot is optional. The engine normally ignores records
    // without its WD header, but dereferences the pointer before checking it.
    CueHeader cue{};
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), address, &cue, sizeof cue, &copied) ||
        copied != sizeof cue) {
        static LONG bad_cues = 0;
        if (InterlockedIncrement(&bad_cues) <= 4)
            log_line("FX-015: ignored unreadable optional battle sound cue at %p", address);
        return;
    }

    // The stock WD decoder sign-extends the ID and calls the same player with
    // flags zero. Decode the copied header once so it cannot be read again.
    if (cue.marker == 0x4457) play_cue(cue.sound, 0);
}

bool install() {
    auto* bank_call = sig_find_unique("FX-015 sound-bank cue call",
        "FF 75 0C FF 75 08 E8 ?? ?? ?? ?? 83 C4 08 E8 ?? ?? ?? ?? A1", 0x8874A5);
    if (!bank_call) return false;
    auto* site = bank_call + 6;
    auto* decoder = call_target(site);
    if (!decoder || !sig_match(decoder,
        "55 8B EC 8B 45 08 B9 57 44 00 00 66 39 08 75 14 C7 45 0C 00 00 00 00 "
        "0F BF 40 02 89 45 08 5D E9 ?? ?? ?? ?? 5D C3")) return false;
    auto* player = jmp_target(decoder + 0x1F);
    if (!player || !sig_match(player,
        "55 8B EC FF 75 0C 8B 0D ?? ?? ?? ?? FF 75 08 6A 30 E8 ?? ?? ?? ?? 5D C3")) return false;
    play_cue = reinterpret_cast<PlayCue>(player);
    return patch_call_rel32(site, decoder, reinterpret_cast<void*>(&play_optional_cue));
}
} // namespace

Row row_FX015 = {
    "FX-015",
    "guard unreadable optional battle sound cues",
    1, install, nullptr, nullptr, 0
};
