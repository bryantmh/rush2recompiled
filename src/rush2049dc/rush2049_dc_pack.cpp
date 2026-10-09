// The Dreamcast Textures option: a Dreamcast Rush 2049 disc's textures at their full size. See include/rush2049_dc.h
// and docs/rush2049_research/dreamcast.md (Full-size textures).
//
// The files made from the disc load each texture scaled down to fit TMEM where it doesn't
// (src/rush2049dc/rush2049_dc_model.cpp). Every texture's full-size image is kept in
// <app folder>/track_cache/dc_textures, built once per disc and build in the background (converting every file first,
// a few seconds on the first boot): one DDS (with mipmaps) per distinct image, "images" (each image's size, kind, an
// 8 x 8 thumbnail and the RT64 hashes its textures are drawn with, from replacement_hash) and the stamp file (format,
// source key, build) written last, so a build that was cut short is done again.
//
// Disc mode (the disc is the Rush 2049 source, whatever the option says: the disc's images are its own textures):
// rt64.json replaces every scaled-down texture with its image, and RT64
// loads the folder like any texture pack (any pack the player enables wins over it). Textures drawn at their full size
// already are only in it when the player installed an upscale of them.
//
// N64 mode (the N64 ROM is the source, a disc is stored and the option is on): src/texture_upscale.cpp asks match() about each Rush 2049
// texture drawn in 3D. The N64's versions are the disc's art redrawn in 16 or 256 colors, often at another aspect
// (the UVs make up for it), so texel differences don't tell a match from a dark or flat lookalike: what does is the
// picture's structure. Disc images at least the texture's size whose 8 x 8 thumbnails' brightness correlates are
// scaled down to the texture's size; the one whose brightness correlates best (0.84 or more, over a texture that
// isn't flat), with mostly the same transparency (the N64's cutouts are hard where the disc's fade, so up to a fifth
// of the texels may differ) and a close average color, is the same picture; an N64 intensity texture (tinted by the
// draw) only takes a gray image. Measured on Rush 2049's N64 textures against the disc's
// (docs/rush2049_research/dreamcast.md, Dreamcast Textures): true pairs reach 0.84, lookalikes stay below 0.82.
// src/texture_upscale.cpp draws it in the texture's place (as a live replacement), or upscales it when it is no larger
// than the texture. Results are kept in matches_<version> (N64 content key, image key) so each texture is compared
// once.
//
// The player's upscales of the disc images (Install Upscaled, from the rush2049dc dump) are kept in
// <app folder>/texture_upscale/dreamcast/<key>.dds and linked into the folder's "upscaled" directory, where rt64.json
// and image_file use them in place of the disc's.

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "recompui/renderer.h"
#include "util/file.h"

#include "rush2049_dc.h"
#include "texture_upscale.h"
#include "track_cache.h"

namespace upscale = rush2::upscale;
using rush2::rom2049::dc::Match;
using rush2::rom2049::dc::TextureMode;

namespace {
    constexpr const char* format = "v2"; // of the folder's files; a new one rebuilds it
    constexpr const char* matches_file = "matches_v4"; // named for the matcher that made them
    constexpr int thumb_size = 8;
    constexpr size_t thumb_bytes = thumb_size * thumb_size * 4;
    constexpr uint8_t flag_shrunk = 1, flag_job = 2, flag_damaged = 4;

    // An image of the folder.
    struct ImageInfo {
        uint64_t key = 0;
        uint32_t w = 0, h = 0;
        uint8_t flags = 0;
        uint8_t thumb[thumb_bytes] = {};
        std::vector<std::pair<uint64_t, bool>> hashes; // RT64 hash, whether that texture is scaled down
    };

    std::filesystem::path pack_dir() {
        return rush2::track_cache::directory() / "dc_textures";
    }

    std::filesystem::path upscale_store() {
        return recompui::file::get_app_folder_path() / "texture_upscale" / "dreamcast";
    }

    std::string stamp_of(const rush2::rom2049::Source& source) {
        char text[80];
        snprintf(text, sizeof(text), "%s %016" PRIx64 " %016" PRIx64, format, source.cache_key(), rush2::track_cache::build());
        return text;
    }

    bool up_to_date(const std::string& stamp) {
        std::ifstream in(pack_dir() / "stamp", std::ios::binary);
        std::string have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::error_code ec;
        return have == stamp && std::filesystem::exists(pack_dir() / "images", ec);
    }

    // Premultiplied color and alpha averaged over an 8 x 8 grid.
    void make_thumb(const upscale::Image& image, uint8_t* out) {
        for (int ty = 0; ty < thumb_size; ty++) {
            for (int tx = 0; tx < thumb_size; tx++) {
                uint32_t x0 = tx * image.width / thumb_size, x1 = std::max(x0 + 1, (tx + 1) * image.width / thumb_size);
                uint32_t y0 = ty * image.height / thumb_size, y1 = std::max(y0 + 1, (ty + 1) * image.height / thumb_size);
                uint64_t sum[4] = {};
                uint32_t n = 0;
                for (uint32_t y = y0; y < y1 && y < image.height; y++) {
                    for (uint32_t x = x0; x < x1 && x < image.width; x++) {
                        const uint8_t* p = &image.rgba[(size_t(y) * image.width + x) * 4];
                        for (int c = 0; c < 3; c++) sum[c] += uint32_t(p[c]) * p[3] / 255;
                        sum[3] += p[3];
                        n++;
                    }
                }
                for (int c = 0; c < 4; c++) out[(ty * thumb_size + tx) * 4 + c] = n ? uint8_t(sum[c] / n) : 0;
            }
        }
    }

    // Pearson correlation of two series (0 when either is flat).
    float correlation(const std::vector<float>& a, const std::vector<float>& b) {
        size_t n = a.size();
        if (n < 2) return 0.0f;
        double ma = 0, mb = 0;
        for (size_t i = 0; i < n; i++) {
            ma += a[i];
            mb += b[i];
        }
        ma /= n;
        mb /= n;
        double sab = 0, saa = 0, sbb = 0;
        for (size_t i = 0; i < n; i++) {
            sab += (a[i] - ma) * (b[i] - mb);
            saa += (a[i] - ma) * (a[i] - ma);
            sbb += (b[i] - mb) * (b[i] - mb);
        }
        return saa > 1e-6 && sbb > 1e-6 ? float(sab / std::sqrt(saa * sbb)) : 0.0f;
    }

    float luma(const uint8_t* p) {
        return p[0] * 0.299f + p[1] * 0.587f + p[2] * 0.114f;
    }

    // How alike two thumbnails' brightness is.
    float thumb_correlation(const uint8_t* a, const uint8_t* b) {
        std::vector<float> la(thumb_size * thumb_size), lb(thumb_size * thumb_size);
        for (int i = 0; i < thumb_size * thumb_size; i++) {
            la[i] = luma(a + i * 4);
            lb[i] = luma(b + i * 4);
        }
        return correlation(la, lb);
    }

    // The disc image averaged down to w x h (box filter; each output texel is the average of the area it covers).
    upscale::Image box_resize(const upscale::Image& in, uint32_t w, uint32_t h) {
        upscale::Image out;
        out.width = w;
        out.height = h;
        out.rgba.resize(size_t(w) * h * 4);
        for (uint32_t y = 0; y < h; y++) {
            uint32_t y0 = y * in.height / h, y1 = std::max(y0 + 1, (y + 1) * in.height / h);
            for (uint32_t x = 0; x < w; x++) {
                uint32_t x0 = x * in.width / w, x1 = std::max(x0 + 1, (x + 1) * in.width / w);
                uint32_t sum[4] = {}, n = 0;
                for (uint32_t yy = y0; yy < y1; yy++) {
                    for (uint32_t xx = x0; xx < x1; xx++) {
                        const uint8_t* p = &in.rgba[(size_t(yy) * in.width + xx) * 4];
                        for (int c = 0; c < 4; c++) sum[c] += p[c];
                        n++;
                    }
                }
                for (int c = 0; c < 4; c++) out.rgba[(size_t(y) * w + x) * 4 + c] = uint8_t(sum[c] / n);
            }
        }
        return out;
    }

    // How alike an N64 texture is to a disc image scaled down to its size.
    struct Likeness {
        float r = -1.0f;         // correlation of brightness over texels opaque in both
        float alpha = 1.0f;      // share of texels opaque in one and clear in the other
        float spread = 0.0f;     // the N64 texture's brightness deviation: below 6 it's too flat to tell
        float color = 255.0f;    // largest channel difference of the average colors
        float tint = 255.0f;     // the disc image's average spread between its channels (0: gray)
        bool same(bool intensity) const {
            return r >= 0.84f && alpha <= 0.2f && spread >= 6.0f && color <= 40.0f && (!intensity || tint <= 12.0f);
        }
    };

    Likeness compare(const upscale::Image& n64, const upscale::Image& disc) {
        Likeness d;
        upscale::Image small = box_resize(disc, n64.width, n64.height);
        size_t n = size_t(n64.width) * n64.height, mismatched = 0;
        std::vector<float> la, lb;
        double ca[3] = {}, cb[3] = {}, tint_sum = 0;
        for (size_t i = 0; i < n; i++) {
            const uint8_t* a = &n64.rgba[i * 4];
            const uint8_t* b = &small.rgba[i * 4];
            bool oa = a[3] >= 128, ob = b[3] >= 128;
            if (oa != ob) {
                mismatched++;
                continue;
            }
            if (!oa) continue;
            la.push_back(luma(a));
            lb.push_back(luma(b));
            tint_sum += std::max({ b[0], b[1], b[2] }) - std::min({ b[0], b[1], b[2] });
            for (int c = 0; c < 3; c++) {
                ca[c] += a[c];
                cb[c] += b[c];
            }
        }
        d.alpha = float(mismatched) / float(n);
        if (la.size() < 8) return d;
        double mean = 0, var = 0;
        for (float v : la) mean += v;
        mean /= la.size();
        for (float v : la) var += (v - mean) * (v - mean);
        d.spread = float(std::sqrt(var / la.size()));
        d.r = correlation(la, lb);
        d.tint = float(tint_sum / la.size());
        d.color = 0.0f;
        for (int c = 0; c < 3; c++) d.color = std::max(d.color, float(std::abs(ca[c] - cb[c]) / la.size()));
        return d;
    }

    // State. Lock order: build_mutex, then mutex.
    std::mutex build_mutex;      // one build or load at a time
    std::mutex mutex;
    bool enabled = true;
    std::shared_ptr<const rush2::rom2049::Source> active;
    std::filesystem::path disc_pack;
    TextureMode mode = TextureMode::Off;
    std::atomic<uint64_t> refreshes = 0;  // a build for an older refresh stops
    std::atomic<uint64_t> generation = 1;
    bool pack_set = false;                // the folder was given to the renderer
    // The folder as loaded.
    std::string loaded_stamp;
    bool index_ready = false;
    std::vector<ImageInfo> images;
    std::unordered_map<uint64_t, size_t> by_key;
    std::unordered_map<uint64_t, uint64_t> by_hash; // RT64 hash -> image key
    std::unordered_set<uint64_t> pack_hashes;  // in rt64.json
    std::unordered_map<uint64_t, uint64_t> matches; // N64 content key -> image key or 0
    std::set<uint64_t> upscaled;          // keys with an installed upscale
    uint64_t upscale_generation = 0;

    void set_renderer_pack(bool on) {
        if (on) {
            pack_set = true;
            recompui::renderer::set_generated_texture_pack(pack_dir());
        }
        else if (pack_set) {
            pack_set = false;
            recompui::renderer::set_generated_texture_pack({});
        }
    }

    void unload() {
        index_ready = false;
        loaded_stamp.clear();
        images.clear();
        by_key.clear();
        by_hash.clear();
        pack_hashes.clear();
        matches.clear();
    }

    bool build(const rush2::rom2049::Source& source, const std::string& stamp, uint64_t refresh) {
        std::vector<uint8_t> scratch;
        for (int i = 0; i < rush2::rom2049::file_count; i++) {
            if (refreshes != refresh) return false;
            source.read_file(i, scratch);
        }
        source.source_texture_count(); // Reads the textures of files cached before this session too.
        std::vector<rush2::rom2049::SourceTexture> textures = source.source_textures(0);

        std::filesystem::path dir = pack_dir();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        std::vector<ImageInfo> made;
        std::map<uint64_t, size_t> index; // content key -> made
        std::set<uint64_t> hashes;
        for (const rush2::rom2049::SourceTexture& t : textures) {
            if (refreshes != refresh) return false;
            uint64_t hash = rush2::rom2049::dc::replacement_hash(t);
            if (hash != 0 && !hashes.insert(hash).second) continue;
            upscale::Image image;
            int w = 0, h = 0;
            if (!source.source_image(t, image.rgba, w, h) || w <= 0 || h <= 0) continue;
            image.width = (uint32_t)w;
            image.height = (uint32_t)h;
            uint64_t key = upscale::content_key(image);
            auto [it, added] = index.try_emplace(key, made.size());
            if (added) {
                if (!upscale::write_file(dir / (upscale::key_name(key) + ".dds"), upscale::make_dds(image))) return false;
                ImageInfo info;
                info.key = key;
                info.w = image.width;
                info.h = image.height;
                make_thumb(image, info.thumb);
                made.push_back(std::move(info));
            }
            ImageInfo& info = made[it->second];
            info.flags |= (t.shrunk ? flag_shrunk : 0) | (t.job != 0 ? flag_job : 0) | (t.damaged ? flag_damaged : 0);
            if (hash != 0) info.hashes.emplace_back(hash, t.shrunk);
        }
        std::vector<uint8_t> out;
        auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
        uint32_t count = (uint32_t)made.size();
        put("DCI2", 4);
        put(&count, 4);
        for (const ImageInfo& info : made) {
            uint32_t nh = (uint32_t)info.hashes.size();
            put(&info.key, 8); put(&info.w, 4); put(&info.h, 4); put(&info.flags, 1); put(info.thumb, thumb_bytes);
            put(&nh, 4);
            for (const auto& [hash, shrunk] : info.hashes) {
                uint8_t s = shrunk;
                put(&hash, 8); put(&s, 1);
            }
        }
        if (!upscale::write_file(dir / "images", out) ||
            !upscale::write_file(dir / "stamp", std::vector<uint8_t>(stamp.begin(), stamp.end()))) {
            return false;
        }
        printf("[dc] textures: %zu full-size images for %zu textures\n", made.size(), textures.size());
        return true;
    }

    // Reads the folder's images and matches (mutex held).
    bool load_index(const std::string& stamp) {
        unload();
        std::vector<uint8_t> in = upscale::read_file(pack_dir() / "images");
        size_t o = 0;
        auto get = [&](void* p, size_t n) {
            if (o + n > in.size()) return false;
            memcpy(p, &in[o], n);
            o += n;
            return true;
        };
        char magic[4];
        uint32_t count = 0;
        if (!get(magic, 4) || memcmp(magic, "DCI2", 4) != 0 || !get(&count, 4)) return false;
        for (uint32_t i = 0; i < count; i++) {
            ImageInfo info;
            uint32_t nh = 0;
            if (!get(&info.key, 8) || !get(&info.w, 4) || !get(&info.h, 4) || !get(&info.flags, 1) ||
                !get(info.thumb, thumb_bytes) || !get(&nh, 4) || nh > in.size()) {
                unload();
                return false;
            }
            for (uint32_t k = 0; k < nh; k++) {
                uint64_t hash;
                uint8_t s;
                if (!get(&hash, 8) || !get(&s, 1)) {
                    unload();
                    return false;
                }
                info.hashes.emplace_back(hash, s != 0);
            }
            by_key[info.key] = images.size();
            for (const auto& [hash, shrunk] : info.hashes) by_hash.emplace(hash, info.key);
            images.push_back(std::move(info));
        }
        std::vector<uint8_t> m = upscale::read_file(pack_dir() / matches_file);
        for (size_t i = 0; i + 16 <= m.size(); i += 16) {
            uint64_t a, b;
            memcpy(&a, &m[i], 8);
            memcpy(&b, &m[i + 8], 8);
            matches[a] = b;
        }
        upscaled.clear();
        std::error_code ec;
        for (const auto& file : std::filesystem::directory_iterator(upscale_store(), ec)) {
            uint64_t key;
            if (file.is_regular_file() && upscale::parse_key_name(upscale::path_utf8(file.path().filename()), key) &&
                by_key.count(key)) {
                upscaled.insert(key);
            }
        }
        loaded_stamp = stamp;
        index_ready = true;
        return true;
    }

    // Writes rt64.json (and the upscaled links) from the index and the installed upscales (mutex held).
    bool write_database() {
        std::filesystem::path dir = pack_dir(), linked = dir / "upscaled";
        std::error_code ec;
        std::filesystem::remove_all(linked, ec);
        std::vector<upscale::PackTexture> pack;
        pack_hashes.clear();
        for (const ImageInfo& info : images) {
            bool mine = upscaled.count(info.key) != 0;
            upscale::PackTexture t;
            for (const auto& [hash, shrunk] : info.hashes) {
                if (shrunk || mine) t.hashes.push_back(hash);
            }
            if (t.hashes.empty()) continue;
            std::string name = upscale::key_name(info.key) + ".dds";
            t.path = name;
            if (mine) {
                std::filesystem::create_directories(linked, ec);
                std::filesystem::path from = upscale_store() / name, to = linked / name;
                std::filesystem::create_hard_link(from, to, ec);
                if (ec) {
                    ec.clear();
                    std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
                }
                if (!ec) t.path = "upscaled/" + name;
            }
            pack_hashes.insert(t.hashes.begin(), t.hashes.end());
            pack.push_back(std::move(t));
        }
        std::string database = upscale::pack_database(pack);
        return upscale::write_file(dir / "rt64.json", std::vector<uint8_t>(database.begin(), database.end()));
    }

    // Puts the option's state in effect: builds or loads the folder as needed, in the background.
    void refresh() {
        uint64_t refresh = ++refreshes;
        std::shared_ptr<const rush2::rom2049::Source> from;
        std::filesystem::path pack;
        TextureMode want;
        {
            std::lock_guard lock{ mutex };
            want = active == nullptr                ? TextureMode::Off
                 : active->is_dreamcast()             ? TextureMode::Disc
                 : enabled && !disc_pack.empty()      ? TextureMode::N64
                                                      : TextureMode::Off;
            from = want == TextureMode::Disc ? active : nullptr;
            pack = disc_pack;
            if (want != TextureMode::Disc) set_renderer_pack(false);
            mode = want;
            generation++;
        }
        if (want == TextureMode::Off) {
            return;
        }
        std::thread([refresh, want, from, pack]() mutable {
            std::lock_guard build_lock{ build_mutex };
            if (refreshes != refresh) return;
            if (from == nullptr) {
                from = rush2::rom2049::dc::open_pack(pack); // N64 mode: the disc only for building its images
            }
            if (from == nullptr || from->cache_key() == 0) return;
            std::string stamp = stamp_of(*from);
            bool loaded;
            {
                std::lock_guard lock{ mutex };
                loaded = index_ready && loaded_stamp == stamp;
            }
            if (!loaded) {
                if (!up_to_date(stamp)) {
                    {
                        std::lock_guard lock{ mutex };
                        set_renderer_pack(false);
                        unload();
                    }
                    if (!build(*from, stamp, refresh)) return;
                }
                std::lock_guard lock{ mutex };
                if (!load_index(stamp)) return;
                upscale_generation = 0;
            }
            std::lock_guard lock{ mutex };
            if (refreshes != refresh) return;
            if (upscale_generation == 0 || !std::filesystem::exists(pack_dir() / "rt64.json")) {
                write_database();
                upscale_generation = 1;
            }
            set_renderer_pack(want == TextureMode::Disc);
            generation++;
        }).detach();
    }
}

void rush2::rom2049::dc::set_textures_enabled(bool on) {
    {
        std::lock_guard lock{ mutex };
        if (enabled == on) return;
        enabled = on;
    }
    refresh();
}

void rush2::rom2049::dc::set_texture_sources(std::shared_ptr<const Source> source, const std::filesystem::path& disc) {
    {
        std::lock_guard lock{ mutex };
        if (active == source && disc_pack == disc) return;
        active = source;
        disc_pack = disc;
    }
    refresh();
}

TextureMode rush2::rom2049::dc::texture_mode() {
    std::lock_guard lock{ mutex };
    return mode;
}

uint64_t rush2::rom2049::dc::texture_generation() {
    return generation;
}

bool rush2::rom2049::dc::in_texture_pack(uint64_t hash) {
    std::lock_guard lock{ mutex };
    return mode == TextureMode::Disc && pack_set && pack_hashes.count(hash) != 0;
}

Match rush2::rom2049::dc::match(const upscale::Image& n64, uint64_t n64_key, bool intensity, uint64_t& image_key) {
    image_key = 0;
    // Candidates, picked with the lock held; the images are compared without it.
    struct Candidate {
        uint64_t key;
        float closeness;
    };
    std::vector<Candidate> candidates;
    {
        std::lock_guard lock{ mutex };
        if (mode != TextureMode::N64) return Match::None;
        if (!index_ready) return Match::NotReady;
        auto it = matches.find(n64_key);
        if (it == matches.end()) {
            uint8_t thumb[thumb_bytes];
            make_thumb(n64, thumb);
            for (const ImageInfo& info : images) {
                if ((info.flags & (flag_job | flag_damaged)) || info.w < n64.width || info.h < n64.height) {
                    continue;
                }
                float r = thumb_correlation(thumb, info.thumb);
                if (r >= 0.5f) candidates.push_back({ info.key, r });
            }
        }
        else if (it->second != 0) {
            image_key = it->second;
            return Match::Found;
        }
        else {
            return Match::None;
        }
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.closeness > b.closeness; });
    size_t considered = candidates.size();
    if (candidates.size() > 12) candidates.resize(12);
    uint64_t best = 0;
    Likeness best_d;
    for (const Candidate& c : candidates) {
        upscale::Image disc;
        if (!upscale::read_dds(upscale::read_file(pack_dir() / (upscale::key_name(c.key) + ".dds")), disc)) continue;
        Likeness d = compare(n64, disc);
        if (d.same(intensity) && d.r > best_d.r) {
            best = c.key;
            best_d = d;
        }
    }
    static const bool log = getenv("RUSH2_DC_MATCH_LOG") != nullptr;
    if (log) {
        printf("[dc match] %s %ux%u: %zu candidates, %s (r %.3f alpha %.3f spread %.1f color %.1f)\n",
               upscale::key_name(n64_key).c_str(), n64.width, n64.height, considered,
               best ? upscale::key_name(best).c_str() : "none", best_d.r, best_d.alpha, best_d.spread, best_d.color);
        fflush(stdout);
    }
    std::lock_guard lock{ mutex };
    if (mode != TextureMode::N64 || !index_ready) return Match::NotReady;
    matches[n64_key] = best;
    uint64_t pair[2] = { n64_key, best };
    std::ofstream(pack_dir() / matches_file, std::ios::binary | std::ios::app).write((const char*)pair, sizeof(pair));
    if (best == 0) return Match::None;
    image_key = best;
    return Match::Found;
}

namespace {
    // (mutex held)
    uint64_t content_of(uint64_t key) {
        bool mine = upscaled.count(key) != 0;
        return key ^ 0xDC7E57A9E5000000ull ^ (mine ? (upscale_generation * 0x9E3779B97F4A7C15ull) : 0);
    }
}

uint64_t rush2::rom2049::dc::image_content(uint64_t key) {
    std::lock_guard lock{ mutex };
    return index_ready && by_key.count(key) ? content_of(key) : 0;
}

bool rush2::rom2049::dc::image_file(uint64_t key, std::vector<uint8_t>& dds, uint64_t& content) {
    std::filesystem::path path;
    {
        std::lock_guard lock{ mutex };
        if (!index_ready || !by_key.count(key)) return false;
        bool mine = upscaled.count(key) != 0;
        path = mine ? upscale_store() / (upscale::key_name(key) + ".dds") : pack_dir() / (upscale::key_name(key) + ".dds");
        content = content_of(key);
    }
    dds = upscale::read_file(path);
    return !dds.empty();
}

size_t rush2::rom2049::dc::dump_images(const std::filesystem::path& dir, const std::vector<uint64_t>& hashes,
                                       const std::vector<uint64_t>& image_keys, std::atomic<size_t>* done) {
    std::vector<uint64_t> keys;
    {
        std::lock_guard lock{ mutex };
        if (!index_ready) return 0;
        std::set<uint64_t> used(image_keys.begin(), image_keys.end());
        for (uint64_t hash : hashes) {
            auto it = by_hash.find(hash);
            if (it != by_hash.end()) used.insert(it->second);
        }
        for (uint64_t key : used) {
            auto it = by_key.find(key);
            if (it != by_key.end() && !(images[it->second].flags & flag_damaged)) keys.push_back(key);
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    for (uint64_t key : keys) {
        std::filesystem::path png = dir / (upscale::key_name(key) + ".png");
        upscale::Image image;
        if (!std::filesystem::exists(png, ec) &&
            upscale::read_dds(upscale::read_file(pack_dir() / (upscale::key_name(key) + ".dds")), image)) {
            upscale::write_png(png, image);
        }
        if (done != nullptr) (*done)++;
    }
    return keys.size();
}

bool rush2::rom2049::dc::upscaled_by_player(uint64_t key) {
    std::lock_guard lock{ mutex };
    return upscaled.count(key) != 0;
}

bool rush2::rom2049::dc::is_image(uint64_t key) {
    std::lock_guard lock{ mutex };
    return index_ready && by_key.count(key) != 0;
}

bool rush2::rom2049::dc::original_image(uint64_t key, upscale::Image& image) {
    if (!is_image(key)) return false;
    return upscale::read_dds(upscale::read_file(pack_dir() / (upscale::key_name(key) + ".dds")), image);
}

void rush2::rom2049::dc::begin_upscales() {
    std::error_code ec;
    std::filesystem::remove_all(upscale_store(), ec);
    std::filesystem::create_directories(upscale_store(), ec);
    std::lock_guard lock{ mutex };
    upscaled.clear();
}

bool rush2::rom2049::dc::add_upscale(uint64_t key, const upscale::Image& image) {
    if (!upscale::write_file(upscale_store() / (upscale::key_name(key) + ".dds"), upscale::make_dds(image))) return false;
    std::lock_guard lock{ mutex };
    upscaled.insert(key);
    return true;
}

void rush2::rom2049::dc::end_upscales() {
    std::lock_guard lock{ mutex };
    upscale_generation++;
    if (index_ready) {
        write_database();
        if (mode == TextureMode::Disc) set_renderer_pack(true); // reloads it
    }
    generation++;
}
