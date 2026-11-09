// CD and GD-ROM CHD images (include/chd_image.h), read with libchdr.
//
// A CD CHD is hunks of frames, each frame 2448 bytes: the 2352 a .bin holds, then 96 of subchannel. Its tracks are
// metadata entries, one per track, "TRACK:n TYPE:t SUBTYPE:s FRAMES:f PREGAP:p PGTYPE:g PGSUB:s POSTGAP:q" (tag CHT2;
// older images use CHTR without the pregap fields; a GD-ROM uses CHGD, which adds PAD:n). Tracks follow each other in
// the CHD, each padded with blank frames to a multiple of 4. A GD-ROM's FRAMES runs up to the next track's sector
// number: chdman adds PAD blank frames after a .gdi track's file to fill the gap (a .gdi with tracks 1, 2, 3 at 0, 450,
// 45000, 300 sectors each, gives FRAMES:450 PAD:150, then FRAMES:44550 PAD:44250). A PGTYPE starting with
// V means the track's pregap is stored at its start (FRAMES counts it); otherwise the pregap is only a gap in the
// disc's sector numbers. A data track's frame holds its data where a .bin's would be for the TYPE: MODE1 / MODE2_FORM1
// keep 2048 bytes at the frame's start, MODE2 / MODE2_FORM_MIX 2336 (8 bytes of subheader first), MODE1_RAW and
// MODE2_RAW the whole raw sector (16 or 24 bytes of sync and header first). Audio samples are stored big-endian (chdman
// swaps them), so read_frame swaps them back.
// Sector numbers: track 1's INDEX 01 is sector 0 on a CD; a GD-ROM's track 3 (the high-density area) starts at 45000.
// Each later track starts after the one before it plus its own pregap.

#include "chd_image.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include <libchdr/chd.h>

namespace chd = rush2::chd;

namespace {
    constexpr uint32_t chd_frame_bytes = 2448;

    // The value after `key:` in a metadata string ("" if missing).
    std::string field(const std::string& meta, const char* key) {
        std::string k = std::string(key) + ":";
        size_t at = meta.find(k);
        if (at == std::string::npos || (at > 0 && meta[at - 1] != ' ')) return {};
        at += k.size();
        size_t end = meta.find(' ', at);
        return meta.substr(at, end == std::string::npos ? std::string::npos : end - at);
    }

    uint32_t number(const std::string& s) { return s.empty() ? 0 : (uint32_t)std::strtoul(s.c_str(), nullptr, 10); }

    uint32_t data_header(const std::string& type) {
        if (type == "MODE1_RAW") return 16;
        if (type == "MODE2_RAW") return 24;
        if (type == "MODE2" || type == "MODE2_FORM_MIX") return 8;
        return 0;   // MODE1, MODE2_FORM1 (and AUDIO, which has none)
    }
}

struct chd::Image::State {
    FILE* fp = nullptr;
    chd_file* file = nullptr;
    uint32_t hunk_bytes = 0, total_hunks = 0;
    mutable std::mutex mutex;
    mutable std::vector<uint8_t> hunk;
    mutable uint32_t hunk_index = UINT32_MAX;
};

chd::Image::Image() = default;

chd::Image::~Image() {
    if (state) {
        if (state->file) chd_close(state->file);
        if (state->fp) fclose(state->fp);
    }
}

bool chd::Image::open(const std::filesystem::path& path) {
    auto s = std::make_unique<State>();
#ifdef _WIN32
    s->fp = _wfopen(path.c_str(), L"rb");
#else
    s->fp = fopen(path.c_str(), "rb");
#endif
    if (!s->fp) return false;
    if (chd_open_file(s->fp, CHD_OPEN_READ, nullptr, &s->file) != CHDERR_NONE) {
        fclose(s->fp);
        return false;
    }
    const chd_header* h = chd_get_header(s->file);
    if (h == nullptr || h->hunkbytes == 0 || h->hunkbytes % chd_frame_bytes != 0) {
        chd_close(s->file);
        fclose(s->fp);
        return false;
    }
    s->hunk_bytes = h->hunkbytes;
    s->total_hunks = h->totalhunks;
    s->hunk.resize(h->hunkbytes);

    std::vector<std::string> metas;
    bool gd = false;
    for (uint32_t tag : { CDROM_TRACK_METADATA2_TAG, CDROM_TRACK_METADATA_TAG, GDROM_TRACK_METADATA_TAG }) {
        for (uint32_t i = 0;; i++) {
            char buf[256] = {};
            uint32_t len = 0, result_tag = 0;
            uint8_t flags = 0;
            if (chd_get_metadata(s->file, tag, i, buf, sizeof(buf) - 1, &len, &result_tag, &flags) != CHDERR_NONE) break;
            metas.emplace_back(buf, strnlen(buf, sizeof(buf)));
        }
        if (!metas.empty()) {
            gd = tag == GDROM_TRACK_METADATA_TAG;
            break;
        }
    }
    if (metas.empty()) {
        chd_close(s->file);
        fclose(s->fp);
        s->file = nullptr;
        s->fp = nullptr;
        return false;
    }

    std::vector<Track> tracks;
    uint32_t chd_pos = 0, lba = 0;
    for (const std::string& m : metas) {
        Track t;
        t.number = (int)number(field(m, "TRACK"));
        t.type = field(m, "TYPE");
        t.audio = t.type == "AUDIO";
        t.header = data_header(t.type);
        uint32_t frames = number(field(m, "FRAMES"));
        uint32_t pregap = number(field(m, "PREGAP"));
        std::string pgtype = field(m, "PGTYPE");
        bool stored = !pgtype.empty() && pgtype[0] == 'V';
        uint32_t pad = ((frames + 3) & ~3u) - frames;
        if (gd && t.number == 3) {
            lba = 45000;
        }
        else if (!stored && !tracks.empty()) {
            lba += pregap;
        }
        uint32_t skip = stored ? std::min(pregap, frames) : 0;
        t.chd_frame = chd_pos + skip;
        t.frames = frames - skip;
        t.lba = lba + skip;
        lba += frames;
        chd_pos += frames + pad;
        tracks.push_back(t);
    }
    std::sort(tracks.begin(), tracks.end(), [](const Track& a, const Track& b) { return a.number < b.number; });
    state = std::move(s);
    list = std::move(tracks);
    is_gdrom = gd;
    return true;
}

bool chd::Image::read_frame(uint32_t frame, uint8_t* out) const {
    if (!state) return false;
    uint32_t per_hunk = state->hunk_bytes / chd_frame_bytes;
    uint32_t hunk = frame / per_hunk;
    if (hunk >= state->total_hunks) return false;
    std::lock_guard lock{ state->mutex };
    if (hunk != state->hunk_index) {
        if (chd_read(state->file, hunk, state->hunk.data()) != CHDERR_NONE) {
            state->hunk_index = UINT32_MAX;
            return false;
        }
        state->hunk_index = hunk;
    }
    const uint8_t* src = state->hunk.data() + size_t(frame % per_hunk) * chd_frame_bytes;
    // Audio is stored big-endian; which frames are audio is up to the track list.
    bool audio = false;
    for (const Track& t : list) {
        if (frame >= t.chd_frame && frame < t.chd_frame + t.frames) {
            audio = t.audio;
            break;
        }
    }
    if (audio) {
        for (uint32_t i = 0; i < frame_size; i += 2) {
            out[i] = src[i + 1];
            out[i + 1] = src[i];
        }
    }
    else {
        memcpy(out, src, frame_size);
    }
    return true;
}
