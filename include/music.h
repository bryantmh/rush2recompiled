#ifndef __MUSIC_H__
#define __MUSIC_H__

#include <cstdint>
#include <string>

#include "recomp.h"

// Race music across Rush 2, SF Rush and Rush 2049 (src/music.cpp): which songs the races may play, how a race picks
// one, and song previews on the Sound tab.
namespace rush2::music {
    // The Sound tab (replaces the frontend's): main volume, Race Music and the song list. Call instead of
    // recompui::config::create_sound_tab(), before finalize().
    void create_sound_tab();
    // Applies the loaded settings. Call after finalize().
    void load_config();
    // Start of func_80062F50 (queues music command $a0): notes the music the game asks for, and while a song preview
    // plays holds it by turning $a0 into a volume update. Returns true if it held the command.
    bool game_command(uint8_t* rdram, recomp_context* ctx);
    // The pause menu's SKIP TRACK (game thread): plays another song now, or makes it the song CONTINUE starts if the
    // pause stopped the music. False if there is none to play (MUSIC off, or no other song allowed).
    bool skip_song(uint8_t* rdram, recomp_context* ctx);
    // The race's song: its name, its game and the courses it plays on ("" for SF Rush's, which play on any of its
    // tracks). False if none is known (MUSIC off, or outside a race).
    struct NowPlaying {
        std::string song, game, course;
    };
    bool now_playing(NowPlaying& out);
    // Which Rush 2049 songs races and menus may play (the Sound tab's Rush 2049 Soundtrack): the N64 ROM's, the
    // Dreamcast disc's or both. Without both installed, the Rush 2049 source's alone.
    void rush2049_soundtracks(bool& n64, bool& disc);
}

namespace rush2::track1 {
    // Rush 2's sequence number for SF Rush song `song` (0-15), installing SF Rush's songs and bank first (src/
    // track1_audio.cpp), or -1 if they aren't available. Call from a game thread.
    int song_sequence(uint8_t* rdram, recomp_context* ctx, int song);
}

namespace rush2::calspeed {
    // Rush 2's sequence number for California Speed song `song` (0-8), installing its songs and bank first (src/rush1/
    // track1_audio.cpp), or -1 if they aren't available. Call from a game thread.
    int song_sequence(uint8_t* rdram, recomp_context* ctx, int song);
}

#endif
