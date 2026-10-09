// San Francisco Rush's own music and fireworks sound on the SF Rush tracks (docs/rush1_research.md, "Audio").
//
// Both games play libaudio sequences: one LZ-compressed song file (ALSeqFile of MIDI files) and one instrument bank
// (.ctl in RAM, samples streamed from the .tbl in ROM), so Rush 2's own player plays SF Rush's songs:
// - ROM: SF Rush keeps its music bank, samples and songs, then its sound effect bank and samples, in one block (ROM
//   0x5D9350-0x7A7930). At boot a block of that size is reserved past the end of Rush 2's ROM in the runtime's ROM image, and filled from the SF Rush ROM
//   once it is available, so the game's DMA reads it like its own data.
// - Songs: Rush 2's song header (pointer 0x800D0474, alSeqFileNew'd at boot by func_800B3B78) is replaced by one that
//   lists Rush 2's 13 songs and then SF Rush's 16 (songs 13-28); func_80061E68 plays any of them.
// - Bank: a copy of SF Rush's .ctl is patched with alBnkfNew for the block's copy of its samples. The copy lives
//   below 16 MB of RDRAM (the audio microcode reads its ADPCM codebooks with 24-bit addresses; above that the music
//   decodes to noise). Before a song is attached to the sequence player (func_8007733C, the music loader thread),
//   the player gets the bank of the song's game (alSeqpSetBank); the player is stopped there.
// - Choice: SF Rush has no per-track songs; with its music on "random" it picks one of 9 race songs (func_80099E44,
//   table 0x800D2910). src/music.cpp picks each race's song (by default an SF Rush track does the same) and asks
//   here for an SF Rush song's sequence number.
// - Sound effects: SF Rush's track emitters play samples Rush 2 has too, except FIRECRK's fireworks. Rush 2's sound
//   effect instrument gets a copy with SF Rush's fireworks as sound 116 (func_80062264's bound is raised in us.toml),
//   and on an SF Rush track the FIRECRCK emitter plays it as SF Rush does.

#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <vector>

#include "recomp.h"
#include "librecomp/game.hpp"
#include "librecomp/addresses.hpp"
#include "rush2_hooks.h"
#include "music.h"
#include "track1.h"
#include "track2049.h"

extern "C" void alBnkfNew(uint8_t* rdram, recomp_context* ctx);       // (ctl, tbl), 0x80009AA4
extern "C" void alSeqpSetBank(uint8_t* rdram, recomp_context* ctx);   // (player, bank), 0x8000B750
extern "C" void track_sound_emitter_add_80099C20(uint8_t* rdram, recomp_context* ctx);   // Looped sound emitter (sound, &pos, range)

namespace {
    constexpr uint32_t track_id = 0x8010C3F0;
    constexpr uint32_t song_header_pointer = 0x800D0474;
    constexpr uint32_t requested_song = 0x800F9550;     // Song func_80061E68 queued for the loader.
    constexpr int rush2_song_count = 13;

    // SF Rush ROM.
    constexpr uint32_t r1_block = 0x5D9350;             // Music bank (.ctl), then its samples, then the songs.
    constexpr uint32_t r1_block_end = 0x7A7930;         // End of the sound effect samples (the main code follows).
    constexpr uint32_t r1_sfx_ctl = 0x70A1D0;           // Sound effect bank, then its samples.
    constexpr uint32_t r1_sfx_tbl = 0x70DCC0;
    constexpr uint32_t r1_sfx_ctl_size = r1_sfx_tbl - r1_sfx_ctl;
    constexpr int r1_fireworks_sound = 62;              // FIRECRK's looped sound (func_800C3678: 0x3E, range 100).
    constexpr uint32_t r1_ctl_size = 0x5DF380 - r1_block;
    constexpr uint32_t r1_songs = 0x6F80A0;
    constexpr int r1_song_count = 16;

    std::mutex mutex;
    uint32_t block_rom = 0;          // Where the block is in the ROM image, or 0.
    bool block_filled = false;
    uint32_t rush2_bank = 0;         // ALBank* of Rush 2's music bank.
    uint32_t rush1_bank = 0;         // ALBank* of SF Rush's, once installed.
    uint32_t current_bank = 0;
    bool installed = false;
    // SF Rush's bank (.ctl) copy plus an empty instrument (see install). It must be in the first 16 MB of RDRAM, as
    // the audio microcode reads the ADPCM codebooks in it with 24-bit addresses, so it uses spare RDRAM there (other
    // users: src/interpolation.cpp 0x80B00000-0x80C00000, src/controls_menu.cpp 0x80C00000-0x80C90000,
    // src/widescreen.cpp 0x80CF0000-0x80D00000, src/rush2049/wings_render.cpp 0x80D00000-0x80E10000).
    constexpr uint32_t empty_instrument = (r1_ctl_size + 15) & ~15u;
    constexpr uint32_t ctl_buffer_size = empty_instrument + 0x20;
    constexpr uint32_t ctl_buffer = 0x80C90000;
    // SF Rush's sound effect bank copy, and Rush 2's sound effect instrument extended by SF Rush's fireworks.
    constexpr uint32_t sfx_ctl_buffer = 0x80C96100;
    constexpr uint32_t sfx_instrument_buffer = 0x80C99C00;
    constexpr uint32_t sfx_instrument = 0x800D2478;     // ALInstrument* of Rush 2's sound effects (func_800B3B78).
    constexpr int rush2_sfx_count = 0x74;               // func_80062264 accepts sound ids below this (us.toml: 0x75).
    constexpr int fireworks_sound = rush2_sfx_count;    // SF Rush's fireworks as sound 116.
    bool sfx_installed = false;

    uint32_t alloc(uint8_t* rdram, size_t size) {
        return (uint32_t)((uint8_t*)recomp::alloc(rdram, (size + 15) & ~size_t(15)) - rdram) + 0x80000000;
    }

    uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
        return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
    }

    // Copies the block from the SF Rush ROM into the reserved space (the image keeps its size, so nothing moves).
    bool fill_block() {
        if (block_filled) {
            return true;
        }
        auto rom = rush2::track1::get_rom();
        if (block_rom == 0 || rom == nullptr || rom->size() < r1_block_end) {
            return false;
        }
        std::span<const uint8_t> image = recomp::get_rom();
        if (image.size() < block_rom + (r1_block_end - r1_block)) {
            return false;
        }
        memcpy(const_cast<uint8_t*>(image.data()) + block_rom, rom->data() + r1_block, r1_block_end - r1_block);
        block_filled = true;
        return true;
    }

    // Loads SF Rush's bank and installs the combined song header (game thread).
    bool install(uint8_t* rdram, recomp_context* ctx) {
        if (installed) {
            return true;
        }
        if (rush2_bank == 0 || !fill_block()) {
            return false;
        }
        auto rom = rush2::track1::get_rom();
        const std::vector<uint8_t>& r1 = *rom;
        uint32_t old_header = (uint32_t)MEM_W(0, (int32_t)song_header_pointer);
        if (old_header == 0 || (int16_t)MEM_H(0, (int32_t)(old_header + 2)) != rush2_song_count ||
            (int16_t)((r1[r1_songs + 2] << 8) | r1[r1_songs + 3]) != r1_song_count) {
            return false;
        }

        // SF Rush's bank has empty instrument slots (offset 0). alBnkfNew rebases those too and then patches the bank
        // header as an instrument (harmless on the console, out of bounds here), so they point at an instrument with no
        // sounds appended to the copy: ALInstrument {u8 x12, s16 bend range, s16 sound count 0}.
        uint32_t ctl = ctl_buffer;
        for (uint32_t i = 0; i < ctl_buffer_size; i++) {
            MEM_B(0, (int32_t)(ctl + i)) = i < r1_ctl_size ? r1[r1_block + i] : 0;
        }
        uint32_t bank = ctl + be32(r1, r1_block + 4);
        int instruments = (int16_t)MEM_H(0, (int32_t)bank);
        for (int i = 0; i < instruments; i++) {
            if (MEM_W(0, (int32_t)(bank + 12 + i * 4)) == 0) {
                MEM_W(0, (int32_t)(bank + 12 + i * 4)) = (int32_t)empty_instrument;
            }
        }
        recomp_context call = *ctx;
        call.r4 = (int32_t)ctl;
        call.r5 = (int32_t)(block_rom + r1_ctl_size);
        alBnkfNew(rdram, &call);
        rush1_bank = (uint32_t)MEM_W(0, (int32_t)(ctl + 4));

        int count = rush2_song_count + r1_song_count;
        uint32_t header = alloc(rdram, 4 + count * 8);
        MEM_H(0, (int32_t)header) = MEM_H(0, (int32_t)old_header);
        MEM_H(0, (int32_t)(header + 2)) = (int16_t)count;
        for (int i = 0; i < rush2_song_count * 2; i++) {
            MEM_W(0, (int32_t)(header + 4 + i * 4)) = MEM_W(0, (int32_t)(old_header + 4 + i * 4));
        }
        uint32_t songs_rom = block_rom + (r1_songs - r1_block);
        for (int k = 0; k < r1_song_count; k++) {
            uint32_t e = header + 4 + (rush2_song_count + k) * 8;
            MEM_W(0, (int32_t)e) = (int32_t)(songs_rom + be32(r1, r1_songs + 4 + k * 8));
            MEM_W(0, (int32_t)(e + 4)) = (int32_t)be32(r1, r1_songs + 8 + k * 8);
        }
        MEM_W(0, (int32_t)song_header_pointer) = (int32_t)header;
        installed = true;
        fprintf(stderr, "[Rush1] SF Rush music ready (bank 0x%08X, songs %d-%d)\n", rush1_bank, rush2_song_count,
                count - 1);
        return true;
    }

    // SF Rush's fireworks (its other track emitters play samples Rush 2 has too): SF Rush's sound effect bank is
    // loaded like its music bank, and Rush 2's sound effect instrument is replaced by a copy with SF Rush's fireworks
    // as sound 116. The copy keeps Rush 2's 116 sounds, so it stays in place.
    bool install_sfx(uint8_t* rdram, recomp_context* ctx) {
        if (sfx_installed) {
            return true;
        }
        if (!fill_block()) {
            return false;
        }
        auto rom = rush2::track1::get_rom();
        const std::vector<uint8_t>& r1 = *rom;
        uint32_t rush2_instrument = (uint32_t)MEM_W(0, (int32_t)sfx_instrument);
        if (rush2_instrument == 0 || (int16_t)MEM_H(0, (int32_t)(rush2_instrument + 0xE)) != rush2_sfx_count) {
            return false;
        }
        for (uint32_t i = 0; i < r1_sfx_ctl_size; i++) {
            MEM_B(0, (int32_t)(sfx_ctl_buffer + i)) = r1[r1_sfx_ctl + i];
        }
        recomp_context call = *ctx;
        call.r4 = (int32_t)sfx_ctl_buffer;
        call.r5 = (int32_t)(block_rom + (r1_sfx_tbl - r1_block));
        alBnkfNew(rdram, &call);
        uint32_t bank = (uint32_t)MEM_W(0, (int32_t)(sfx_ctl_buffer + 4));
        uint32_t instrument = (uint32_t)MEM_W(0, (int32_t)(bank + 12));
        if ((int16_t)MEM_H(0, (int32_t)(instrument + 0xE)) <= r1_fireworks_sound) {
            return false;
        }
        uint32_t fireworks = (uint32_t)MEM_W(0, (int32_t)(instrument + 0x10 + r1_fireworks_sound * 4));
        for (uint32_t i = 0; i < 0x10 + rush2_sfx_count * 4; i += 4) {
            MEM_W(0, (int32_t)(sfx_instrument_buffer + i)) = MEM_W(0, (int32_t)(rush2_instrument + i));
        }
        MEM_H(0, (int32_t)(sfx_instrument_buffer + 0xE)) = (int16_t)(rush2_sfx_count + 1);
        MEM_W(0, (int32_t)(sfx_instrument_buffer + 0x10 + fireworks_sound * 4)) = (int32_t)fireworks;
        MEM_W(0, (int32_t)sfx_instrument) = (int32_t)sfx_instrument_buffer;
        sfx_installed = true;
        return true;
    }
}

// func_800B3B78 at 0x800B4000, after alSeqpSetBank: Rush 2's music bank is in place. Reserves the SF Rush block in
// the ROM image (before any other thread reads the ROM) and fills it if the SF Rush ROM is already there.
extern "C" void rush2_track1_audio_init(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    uint32_t ctl = (uint32_t)MEM_W(0, (int32_t)(ctx->r29 + 0x144));
    rush2_bank = (uint32_t)MEM_W(0, (int32_t)(ctl + 4));
    current_bank = rush2_bank;
    if (block_rom == 0) {
        std::span<const uint8_t> image = recomp::get_rom();
        std::vector<uint8_t> grown(image.begin(), image.end());
        block_rom = (uint32_t)((grown.size() + 0xFFFF) & ~size_t(0xFFFF));
        grown.resize(block_rom + (r1_block_end - r1_block), 0);
        recomp::set_rom_contents(std::move(grown));
    }
    fill_block();
}

int rush2::track1::song_sequence(uint8_t* rdram, recomp_context* ctx, int song) {
    if (song < 0 || song >= r1_song_count) {
        return -1;
    }
    std::lock_guard lock{ mutex };
    if (!install(rdram, ctx)) {
        return -1;
    }
    return rush2_song_count + song;
}

// func_8007733C at 0x80077424, before alSeqpSetSeq: gives the player the bank of the song being attached.
extern "C" void rush2_track1_song_bank(uint8_t* rdram, recomp_context* ctx) {
    std::lock_guard lock{ mutex };
    if (rush2_bank == 0) {
        return;
    }
    int song = (int32_t)MEM_W(0, (int32_t)requested_song);
    uint32_t bank = (song >= rush2_song_count && rush1_bank != 0) ? rush1_bank : rush2_bank;
    if (bank == current_bank) {
        return;
    }
    recomp_context call = *ctx;
    call.r4 = MEM_W(0, ctx->r18);   // $s2 = &player (0x800FAEB4)
    call.r5 = (int32_t)bank;
    alSeqpSetBank(rdram, &call);
    current_bank = bank;
}

// Start of func_800BBB04, Rush 2's FIRECRCK emitter (a firecracker effect at the record's position 0x800D0178): on
// an SF Rush track it is SF Rush's FIRECRK instead, a looped sound at the position (func_80099C20(sound, &pos, range),
// as SF Rush's func_800C3678 does with its fireworks sound). Returns true when it replaced the emitter.
extern "C" int rush2_track1_fireworks(uint8_t* rdram, recomp_context* ctx) {
    if (rush2::track1::race_track() == 0 || MEM_B(0, (int32_t)track_id) != rush2::track2049::host_slot) {
        return 0;
    }
    std::lock_guard lock{ mutex };
    if (!install_sfx(rdram, ctx)) {
        return 0;
    }
    recomp_context call = *ctx;
    call.r4 = fireworks_sound;
    call.r5 = (int32_t)0x800D0178;
    call.r6 = 100;
    track_sound_emitter_add_80099C20(rdram, &call);
    return 1;
}
