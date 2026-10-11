// Derived replacements (include/texture_derive.h, docs/texture_upscaling.md "Derived replacements").

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "common/rt64_live_textures.h"
#include "json/json.hpp"
#include "librecomp/mods.hpp"
#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"

#include "asset_catalog.h"
#include "assets.h"
#include "texture_derive.h"

namespace rush2::derive {

using rush2::upscale::Edge;
using rush2::upscale::Image;

namespace {
    // A catalog texture: what drawn textures are traced to.
    struct Base {
        uint64_t key = 0, index_key = 0, hash = 0;
        uint32_t width = 0, height = 0;
        bool indexed = false;           // Compared by palette index (else by color).
        Edge edge_s = Edge::Wrap, edge_t = Edge::Wrap;
        Image image;
        std::vector<uint8_t> indices;
        bool stripe = false;            // A Rush 2 stripe tile (asset 0x1C, cars/stripes).
        bool paint = false;             // A car paint base (rush2::catalog::paint_base).
        Image preview;                  // A paint base's paint_preview: what Dump Textures writes for it.
        bool ui = false;                // Only in interface containers (rush2::catalog::ui_path).
    };

    // A texture drawn that no pack replaces.
    struct Drawn {
        uint64_t hash, key, index_key;
        Image image;
        std::vector<uint8_t> indices;
        bool world;                     // Drawn in 3D (else 2D: interface).
    };

    // Where a drawn texture's rows are in a base: row r of the drawn texture is row r + dy of the base.
    struct Match {
        int base = -1;
        int dy = 0;
    };

    constexpr uint32_t segment = 8;     // Texels per row piece the index keys on.
    constexpr size_t common = 64;       // Row pieces in more places than this say nothing about where a texture is.
    std::atomic_bool keep_car_colors = true; // Keep Car Colors (set_keep_car_colors).

    // The value of texel x of row y, for comparing: its palette index, else its color.
    uint32_t texel(const Image& image, const std::vector<uint8_t>& indices, bool indexed, uint32_t x, uint32_t y) {
        size_t i = size_t(y) * image.width + x;
        if (indexed) return indices[i];
        uint32_t v;
        memcpy(&v, &image.rgba[i * 4], 4);
        return v;
    }

    // The key of the row piece at (x, y) (x a multiple of segment), or 0 when it's one value (blank).
    uint64_t piece(const Image& image, const std::vector<uint8_t>& indices, bool indexed, uint32_t x, uint32_t y) {
        uint32_t n = std::min(segment, image.width - x);
        uint32_t values[segment];
        bool blank = true;
        for (uint32_t i = 0; i < n; i++) {
            values[i] = texel(image, indices, indexed, x + i, y);
            blank = blank && values[i] == values[0];
        }
        if (blank) return 0;
        uint64_t seed = (uint64_t(image.width) << 32) | (uint64_t(x) << 1) | (indexed ? 1 : 0);
        return XXH3_64bits_withSeed(values, n * sizeof(uint32_t), seed) | 1;
    }

    class Deriver {
    public:
        void start() {
            std::thread([this]() {
                // The catalog reads the game's assets as loaded, so it waits for the game to draw its first 3D texture.
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [this]() { return !all.empty(); });
                }
                build();
                {
                    std::unique_lock lock(mutex);
                    ready = true;
                    reload = true;
                }
                run();
            }).detach();
        }

        void drawn(uint64_t hash, uint64_t key, uint64_t index_key, const Image& image, std::vector<uint8_t> indices,
                   bool world) {
            std::unique_lock lock(mutex);
            if (!seen.insert(hash).second) return;
            all.push_back({ hash, key, index_key, image, std::move(indices), world });
            queue.push_back(all.size() - 1);
            changed.notify_all();
        }

        Outcome outcome(uint64_t hash) {
            std::unique_lock lock(mutex);
            auto it = outcomes.find(hash);
            return it != outcomes.end() ? it->second : Outcome::Unknown;
        }

        void set_outcome(uint64_t hash, Outcome o) {
            std::unique_lock lock(mutex);
            outcomes[hash] = o;
        }

        bool owns(uint64_t hash) {
            std::unique_lock lock(mutex);
            return owned.count(hash) != 0;
        }

        std::unique_lock<std::mutex> hold_packs() {
            std::unique_lock pack_lock(pack_mutex);
            for (Pack& pack : packs) pack.zip.reset(); // base_upscale opens them again once the lock is let go.
            return pack_lock;
        }

        void packs_changed() {
            std::unique_lock lock(mutex);
            reload = true;
            changed.notify_all();
        }

        void set_ui(bool enabled) {
            std::unique_lock lock(mutex);
            if (ui_enabled == enabled) return;
            ui_enabled = enabled;
            reload = true; // Derives everything again, interface art in or out.
            changed.notify_all();
        }

        void set_keep_car_colors(bool enabled) {
            std::unique_lock lock(mutex);
            if (keep_car_colors.exchange(enabled) == enabled) return;
            reload = true; // Derives everything again, the car paint with or without the clamp.
            changed.notify_all();
        }

        bool index_ready() {
            std::unique_lock lock(mutex);
            return ready;
        }

        void set_mod_textures(std::unordered_map<uint64_t, ModTexture> textures) {
            std::unique_lock lock(mutex);
            mods = std::move(textures);
            mod_images_stale = true;
            reload = true;
            changed.notify_all();
        }

        bool mod_texture(uint64_t index_key, uint64_t key) {
            std::unique_lock lock(mutex);
            return (index_key != 0 && mods.count(index_key) != 0) || (key != 0 && mods.count(key) != 0);
        }

        bool interface_texture(uint64_t key, uint64_t index_key) {
            std::unique_lock lock(mutex);
            return ui_keys.count(key) != 0 || (index_key != 0 && ui_index_keys.count(index_key) != 0);
        }

    private:
        // The catalog's textures and the index of their row pieces, from the game's assets as they are now (the
        // recomp builds some as it goes: the track select's Rush 2049 previews appear when it's opened).
        void build() {
            auto started = std::chrono::steady_clock::now();
            index_generation = rush2::assets::generation();
            bases.clear();
            stripes_by_size.clear();
            by_key.clear();
            by_index.clear();
            piece_keys.clear();
            piece_at.clear();
            // Interface art: textures only in interface containers (one also on a car or a track isn't).
            std::unordered_set<uint64_t> ui, ui_index, other, other_index;
            std::vector<rush2::catalog::Entry> containers = rush2::catalog::containers([](const std::string&) {});
            for (const rush2::catalog::Entry& e : containers) {
                rush2::catalog::Container c;
                std::string error;
                if (!rush2::catalog::read_container(*e.data, c, error, false, false, &e.copies)) continue;
                const std::vector<uint8_t> paint_palette = e.path.rfind("cars/", 0) == 0
                    ? rush2::catalog::paint_preview_palette(containers, e) : std::vector<uint8_t>{};
                const bool ui_container = rush2::catalog::ui_path(e.path);
                for (rush2::catalog::Texture& t : c.textures) {
                    (ui_container ? ui : other).insert(t.key);
                    if (t.index_key) (ui_container ? ui_index : other_index).insert(t.index_key);
                    if (by_key.count(t.key)) continue;
                    Base b;
                    b.paint = rush2::catalog::paint_base(e, t);
                    if (b.paint) b.preview = rush2::catalog::paint_preview(t.indices, t.width, t.height, paint_palette);
                    b.key = t.key;
                    b.index_key = t.index_key;
                    b.hash = t.hash;
                    b.width = t.width;
                    b.height = t.height;
                    b.indexed = !t.indices.empty();
                    b.edge_s = t.edge_s;
                    b.edge_t = t.edge_t;
                    b.image = std::move(t.image);
                    b.indices = std::move(t.indices);
                    b.stripe = e.path == "cars/stripes";
                    int id = int(bases.size());
                    by_key.emplace(b.key, id);
                    if (b.index_key) by_index.emplace(b.index_key, id);
                    if (b.stripe) stripes_by_size[(uint64_t(b.width) << 32) | b.height].push_back(id);
                    bases.push_back(std::move(b));
                }
            }
            for (uint64_t k : other) ui.erase(k);
            for (uint64_t k : other_index) ui_index.erase(k);
            for (Base& b : bases) b.ui = ui.count(b.key) != 0;
            {
                std::unique_lock lock(mutex);
                ui_keys.swap(ui);
                ui_index_keys.swap(ui_index);
            }
            std::vector<std::pair<uint64_t, uint32_t>> pieces;
            for (size_t id = 0; id < bases.size(); id++) {
                const Base& b = bases[id];
                for (uint32_t y = 0; y < b.height; y++) {
                    for (uint32_t x = 0; x < b.width; x += segment) {
                        uint64_t p = piece(b.image, b.indices, b.indexed, x, y);
                        if (p != 0) pieces.emplace_back(p, uint32_t(id << 11) | y);
                    }
                }
            }
            std::sort(pieces.begin(), pieces.end());
            piece_keys.reserve(pieces.size());
            piece_at.reserve(pieces.size());
            for (const auto& [p, at] : pieces) {
                piece_keys.push_back(p);
                piece_at.push_back(at);
            }
            double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            printf("[derive] %zu catalog textures, %zu row pieces indexed in %.1f s\n", bases.size(), pieces.size(), seconds);
            fflush(stdout);
        }

        // The base a drawn texture comes from: the same image, the same palette indices, or most of its row pieces.
        Match find(const Drawn& d) const {
            if (auto it = by_key.find(d.key); it != by_key.end()) return { it->second, 0 };
            if (d.index_key)
                if (auto it = by_index.find(d.index_key); it != by_index.end()) return { it->second, 0 };
            bool indexed = !d.indices.empty();
            std::unordered_map<uint64_t, uint32_t> votes; // (base, dy) -> row pieces agreeing.
            uint32_t total = 0;
            for (uint32_t y = 0; y < d.image.height; y++) {
                for (uint32_t x = 0; x < d.image.width; x += segment) {
                    uint64_t p = piece(d.image, d.indices, indexed, x, y);
                    if (p == 0) continue;
                    total++;
                    auto [lo, hi] = std::equal_range(piece_keys.begin(), piece_keys.end(), p);
                    if (size_t(hi - lo) > common) continue;
                    for (auto it = lo; it != hi; ++it) {
                        uint32_t at = piece_at[it - piece_keys.begin()];
                        int base = int(at >> 11), by = int(at & 0x7FF);
                        if (bases[base].indexed != indexed) continue;
                        votes[(uint64_t(base) << 32) | uint32_t(by - int(y))]++;
                    }
                }
            }
            Match best;
            uint32_t most = 0;
            for (const auto& [k, n] : votes) {
                if (n > most) {
                    most = n;
                    best = { int(k >> 32), int(int32_t(uint32_t(k))) };
                }
            }
            // A quarter of its row pieces in place: stripes stamped over a car panel leave the rest of it.
            if (most < 2 || most * 4 < total) return {};
            return best;
        }

        // The enabled texture packs' images, by the RT64 hash (or catalog content key) they're listed under.
        struct PackFile {
            size_t pack;
            std::string path;
        };
        void read_packs() {
            pack_generation++; // Images made from the packs before are other images now.
            std::vector<Pack> new_packs; // Swapped in at the end (hold_packs).
            files.clear();
            for (const recomp::mods::ModDetails& mod : recomp::mods::get_all_mod_details("rush2")) {
                if (!recomp::mods::is_mod_enabled(mod.mod_id)) continue;
                std::filesystem::path path = recomp::mods::get_mod_filename(mod.mod_id);
                auto zip = std::make_shared<rush2::upscale::ZipReader>(path);
                std::vector<uint8_t> bytes;
                std::filesystem::path folder;
                if (zip->is_open()) {
                    if (!zip->read("rt64.json", bytes)) continue;
                }
                else {
                    bytes = rush2::upscale::read_file(path / "rt64.json");
                    if (bytes.empty()) continue;
                    zip.reset();
                    folder = path;
                }
                zip.reset(); // Opened again while a batch reads it: an open .rtz can't be replaced on Windows.
                nlohmann::json db = nlohmann::json::parse(bytes.begin(), bytes.end(), nullptr, false);
                if (!db.is_object() || !db.contains("textures") || !db["textures"].is_array()) continue;
                size_t index = new_packs.size();
                new_packs.push_back({ path, folder.empty(), nullptr });
                for (const auto& t : db["textures"]) {
                    if (!t.is_object() || !t.contains("path") || !t.contains("hashes")) continue;
                    const auto& h = t["hashes"];
                    if (!h.is_object() || !h.contains("rt64") || !h["rt64"].is_string()) continue;
                    uint64_t hash = 0;
                    if (!rush2::upscale::parse_key_name(h["rt64"].get<std::string>(), hash)) continue;
                    files.try_emplace(hash, PackFile{ index, t["path"].get<std::string>() }); // The first pack wins.
                }
            }
{
                std::unique_lock pack_lock(pack_mutex);
                packs = std::move(new_packs);
            }
            printf("[derive] %zu texture packs, %zu images\n", packs.size(), files.size());
            fflush(stdout);
        }

        // A pack's upscale of a base (by its RT64 hash, else its content key), decoded once.
        bool base_upscale(const Base& b, Image& out) {
            if (auto it = upscales.find(b.key); it != upscales.end()) {
                out = it->second;
                return !out.empty();
            }
            Image image;
            for (uint64_t id : { b.hash, b.key }) {
                auto f = id ? files.find(id) : files.end();
                if (f == files.end()) continue;
                Pack& pack = packs[f->second.pack];
                std::unique_lock pack_lock(pack_mutex);
                if (pack.is_zip && !pack.zip) pack.zip = std::make_unique<rush2::upscale::ZipReader>(pack.path);
                for (const char* ext : { ".dds", ".png" }) {
                    std::vector<uint8_t> bytes;
                    std::string name = f->second.path + ext;
                    bool got = pack.is_zip ? pack.zip->read(name, bytes)
                                           : !(bytes = rush2::upscale::read_file(pack.path / std::filesystem::u8path(name))).empty();
                    if (got && rush2::upscale::decode_image(bytes, image)) break;
                }
                if (!image.empty()) break;
            }
            // A whole multiple of the base's size, as recolor needs.
            if (!image.empty()) {
                uint32_t scale = std::max<uint32_t>(1, (image.width + b.width / 2) / b.width);
                if (image.width != b.width * scale || image.height != b.height * scale) {
                    image = rush2::upscale::resize_bilinear(image, b.width * scale, b.height * scale);
                }
            }
            // A few hundred kept (a 4x upscale of a 64x64 texture is 256 KB): enough for the panels and their stripe
            // tiles a car brings, which are wanted together.
            if (upscales.size() >= 256) upscales.clear();
            upscales[b.key] = image;
            out = std::move(image);
            return !out.empty();
        }

        // A Rush 2 car panel with stripes stamped on (car_stamp_stripes_on_panel_80083F50): each texel where the stripe
        // tile's alpha nibble reaches the detail level's threshold (16 / (lod + 1) - 1) is remapped to the STRIPE
        // COLOR by its palette index, the rest keeps the paint. The tile is the panel's size and lies on it texel for
        // texel. The tile and threshold are found from which texels changed; then the panel's upscale is recolored to
        // the paint and to the stripe color, and the two are blended by the tile's upscale (its alpha, at the
        // threshold), so the stripes' edges come from the stripe tile's upscale. False when it isn't one.
        bool derive_stripes(const Drawn& d, const Base& b, const Image& up, Image& out) {
            if (!b.indexed || d.indices.size() != b.indices.size() || b.stripe) return false;
            auto tiles = stripes_by_size.find((uint64_t(b.width) << 32) | b.height);
            if (tiles == stripes_by_size.end()) return false;
            const size_t n = d.indices.size();
            std::vector<uint8_t> changed(n);
            size_t changes = 0;
            for (size_t i = 0; i < n; i++) changes += changed[i] = d.indices[i] != b.indices[i];
            if (changes == 0) return false;
            // The tile and threshold whose stamp covers every changed texel and the fewest others (a stamped texel can
            // keep its index).
            int best = -1, best_threshold = 0;
            size_t best_extra = SIZE_MAX;
            for (int id : tiles->second) {
                const Image& tile = bases[id].image;
                for (int threshold : { 15, 7, 4, 3 }) {
                    size_t missed = 0, extra = 0;
                    for (size_t i = 0; i < n && missed == 0; i++) {
                        bool stamped = (tile.rgba[i * 4 + 3] + 8) / 17 >= threshold;
                        missed += changed[i] && !stamped;
                        extra += stamped && !changed[i];
                    }
                    if (missed == 0 && extra < best_extra) {
                        best = id;
                        best_threshold = threshold;
                        best_extra = extra;
                    }
                }
            }
            if (best < 0 || best_extra * 4 > changes) return false;
            const Image& tile = bases[best].image;
            // Each palette index's color with the paint and with the stripe, from the texels drawn each way.
            uint32_t paint[256] = {}, stripe[256] = {};
            bool has_paint[256] = {}, has_stripe[256] = {};
            for (size_t i = 0; i < n; i++) {
                uint8_t index = b.indices[i];
                bool stamped = (tile.rgba[i * 4 + 3] + 8) / 17 >= best_threshold;
                uint32_t color;
                memcpy(&color, &d.image.rgba[i * 4], 4);
                (stamped ? stripe : paint)[index] = color;
                (stamped ? has_stripe : has_paint)[index] = true;
            }
            // An index drawn only one way takes the nearest index's color the other way (the ramps run in steps).
            auto fill = [](uint32_t* colors, bool* has) {
                for (int i = 0; i < 256; i++) {
                    if (has[i]) continue;
                    for (int k = 1; k < 256; k++) {
                        if (i - k >= 0 && has[i - k]) { colors[i] = colors[i - k]; break; }
                        if (i + k < 256 && has[i + k]) { colors[i] = colors[i + k]; break; }
                    }
                }
            };
            fill(paint, has_paint);
            fill(stripe, has_stripe);
            Image painted = d.image, striped = d.image;
            for (size_t i = 0; i < n; i++) {
                memcpy(&painted.rgba[i * 4], &paint[b.indices[i]], 4);
                memcpy(&striped.rgba[i * 4], &stripe[b.indices[i]], 4);
            }
            Image up_paint = recolor(b, up, painted);
            Image up_stripe = recolor(b, up, striped);
            // The tile at the panel upscale's size: its own upscale when a pack has one, else its texels.
            Image mask;
            if (!base_upscale(bases[best], mask) || mask.width == 0) mask = tile;
            if (mask.width != up.width || mask.height != up.height) {
                mask = rush2::upscale::resize_bilinear(mask, up.width, up.height);
            }
            out = up_paint;
            for (size_t i = 0; i < size_t(up.width) * up.height; i++) {
                float level = mask.rgba[i * 4 + 3] / 17.0f;
                float w = std::clamp(level - (best_threshold - 1), 0.0f, 1.0f);
                for (int c = 0; c < 4; c++) {
                    out.rgba[i * 4 + c] = uint8_t(up_paint.rgba[i * 4 + c] * (1 - w) + up_stripe.rgba[i * 4 + c] * w + 0.5f);
                }
            }
            return true;
        }

        // The replacement of a drawn texture from its base's upscale: the base's rows where the drawn one has them
        // (with the drawn colors), its own texels, scaled up, past the base's ends.
        Image derive(const Drawn& d, const Base& b, int dy, const Image& up) {
            const Image& source = upscaled_from(b);
            uint32_t s = up.width / b.width;
            Image old_colors, upscaled;
            old_colors.width = d.image.width;
            old_colors.height = d.image.height;
            old_colors.rgba.resize(d.image.rgba.size());
            upscaled.width = d.image.width * s;
            upscaled.height = d.image.height * s;
            upscaled.rgba.resize(size_t(upscaled.width) * upscaled.height * 4);
            const size_t row = size_t(d.image.width) * 4, up_row = size_t(upscaled.width) * 4;
            for (uint32_t r = 0; r < d.image.height; r++) {
                int br = int(r) + dy;
                if (br >= 0 && br < int(b.height) && b.width == d.image.width) {
                    memcpy(&old_colors.rgba[r * row], &source.rgba[size_t(br) * row], row);
                    memcpy(&upscaled.rgba[size_t(r) * s * up_row], &up.rgba[size_t(br) * s * up_row], up_row * s);
                }
                else {
                    memcpy(&old_colors.rgba[r * row], &d.image.rgba[r * row], row);
                    for (uint32_t sy = 0; sy < s; sy++) {
                        for (uint32_t x = 0; x < upscaled.width; x++) {
                            memcpy(&upscaled.rgba[(size_t(r) * s + sy) * up_row + x * 4], &d.image.rgba[r * row + (x / s) * 4], 4);
                        }
                    }
                }
            }
            if (old_colors.rgba == d.image.rgba) return upscaled;
            if (dy == 0 && d.image.width == b.width && d.image.height == b.height) return recolor(b, up, d.image);
            return rush2::upscale::recolor(upscaled, old_colors, d.image, b.edge_s, b.edge_t);
        }

        // A base's upscale in a drawn texture's colors (same size as the base). A paint base's upscale is of its
        // colored preview, whose colors tell the paints apart, and what the preview's colors don't explain goes on as
        // brightness only.
        static Image recolor(const Base& b, const Image& up, const Image& colors) {
            Image out = rush2::upscale::recolor(up, upscaled_from(b), colors, b.edge_s, b.edge_t, b.paint);
            // Keep Car Colors: no colors the drawn texture's texels don't have nearby (an upscaler's spill onto the
            // fixed parts, which recolor passes through as drawn).
            if (b.paint && keep_car_colors) rush2::upscale::clamp_chroma(out, colors, b.edge_s, b.edge_t);
            return out;
        }

        // The image a base's upscale is of: what Dump Textures wrote, a paint base's preview (gray for a Dreamcast
        // disc's car, which has none).
        static const Image& upscaled_from(const Base& b) {
            return b.paint && !b.preview.empty() ? b.preview : b.image;
        }

        // A car mod's stand-in drawn (set_mod_textures): its replacement is the mod's image in the paint it's drawn
        // with. False if d isn't one.
        bool mod_replace(const Drawn& d) {
            ModTexture mt;
            {
                std::unique_lock lock(mutex);
                auto it = d.index_key ? mods.find(d.index_key) : mods.end();
                if (it == mods.end() && d.key != 0) it = mods.find(d.key); // a model mod's true-color stand-in
                if (it == mods.end()) return false;
                mt = it->second;
            }
            ModImage& m = mod_images[mt.id];
            if (!m.tried) {
                m.tried = true;
                m.ok = mt.load && mt.load(m.image, m.mask) && !m.image.empty();
            }
            if (!m.ok) {
                set_outcome(d.hash, Outcome::NoUpscale);
                return true;
            }
            uint8_t paint[2][3] = { { 255, 255, 255 }, { 255, 255, 255 } };
            bool found[2] = {};
            for (size_t i = 0; i < d.indices.size() && i * 4 + 3 < d.image.rgba.size(); i++) {
                for (int k = 0; k < 2; k++) {
                    if (!found[k] && d.indices[i] == (k == 0 ? mt.main_entry : mt.accent_entry)) {
                        memcpy(paint[k], &d.image.rgba[i * 4], 3);
                        found[k] = true;
                    }
                }
            }
            Image out = paint_mod(m.image, m.mask, paint, mt.damaged, mt.seed);
            uint64_t content = d.hash ^ 0xCA2B0D7ull ^ (pack_generation * 0x9E3779B97F4A7C15ull);
            RT64::addLiveReplacement(d.hash, content, rush2::upscale::make_dds(out));
            set_outcome(d.hash, Outcome::Derived);
            std::unique_lock lock(mutex);
            owned.insert(d.hash);
            return true;
        }

        // The mod's image with its painted texels in the drawn paint: a body texel's brightness over the stand-in
        // ramp's lightest grey (Rush 2's CARPALETTE entry 1, 230) times the paint, so shading in the image stays.
        // Damaged: the paint mottled lighter and darker in blotches, with a few bright scrapes, as Rush 2's D1 panel
        // textures are.
        static Image paint_mod(const Image& image, const Image& mask, const uint8_t paint[2][3], bool damaged,
                               uint32_t seed) {
            Image out = image;
            const bool has_mask = !mask.empty();
            auto hash = [seed](int x, int y) {
                uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ seed * 83492791u;
                h ^= h >> 13;
                h *= 0x5BD1E995u;
                h ^= h >> 15;
                return float(h & 0xFFFF) / 65535.0f;
            };
            auto noise = [&](float x, float y) {
                int x0 = int(std::floor(x)), y0 = int(std::floor(y));
                float fx = x - x0, fy = y - y0;
                fx = fx * fx * (3.0f - 2.0f * fx);
                fy = fy * fy * (3.0f - 2.0f * fy);
                float a = hash(x0, y0) + (hash(x0 + 1, y0) - hash(x0, y0)) * fx;
                float b = hash(x0, y0 + 1) + (hash(x0 + 1, y0 + 1) - hash(x0, y0 + 1)) * fx;
                return a + (b - a) * fy;
            };
            const float cell = std::max(4.0f, float(image.width) / 24.0f); // blotch size
            for (uint32_t y = 0; y < image.height; y++) {
                for (uint32_t x = 0; x < image.width; x++) {
                    size_t i = (size_t(y) * image.width + x) * 4;
                    float wm = 0.0f, wa = 0.0f;
                    if (has_mask) {
                        size_t mi = (size_t(y * mask.height / image.height) * mask.width + x * mask.width / image.width) * 4;
                        wm = mask.rgba[mi] / 255.0f;
                        wa = mask.rgba[mi + 1] / 255.0f;
                    }
                    float total = wm + wa;
                    if (total <= 0.0f) continue;
                    if (total > 1.0f) {
                        wm /= total;
                        wa /= total;
                        total = 1.0f;
                    }
                    float lum = (0.299f * out.rgba[i] + 0.587f * out.rgba[i + 1] + 0.114f * out.rgba[i + 2]) / 230.0f;
                    if (damaged) {
                        float n = (noise(x / cell, y / cell) - 0.5f) * 0.45f + (noise(x / cell * 2.7f + 17.0f, y / cell * 2.7f) - 0.5f) * 0.2f;
                        lum *= 1.0f + n;
                    }
                    for (int c = 0; c < 3; c++) {
                        float painted = (paint[0][c] * wm + paint[1][c] * wa) * lum;
                        float v = out.rgba[i + c] * (1.0f - total) + painted;
                        out.rgba[i + c] = uint8_t(std::clamp(v, 0.0f, 255.0f));
                    }
                }
            }
            if (damaged) {
                // Scrapes: short bright streaks, about one per 40 x 40 texels of paint.
                uint32_t scrapes = std::max(1u, image.width * image.height / 1600 / 16);
                for (uint32_t s = 0; s < scrapes; s++) {
                    float x = hash(int(s), 101) * image.width, y = hash(int(s), 202) * image.height;
                    float angle = (hash(int(s), 303) - 0.5f) * 1.2f;
                    int length = int(cell * (1.0f + hash(int(s), 404) * 2.0f));
                    for (int t = 0; t < length; t++) {
                        int px = int(x + std::cos(angle) * t), py = int(y + std::sin(angle) * t);
                        if (px < 0 || py < 0 || px >= int(image.width) || py >= int(image.height)) break;
                        size_t i = (size_t(py) * image.width + px) * 4;
                        float w = has_mask ? mask.rgba[(size_t(py * mask.height / image.height) * mask.width + px * mask.width / image.width) * 4] / 255.0f : 0.0f;
                        for (int c = 0; c < 3; c++) out.rgba[i + c] = uint8_t(out.rgba[i + c] + (220 - out.rgba[i + c]) * w * 0.7f);
                    }
                }
            }
            return out;
        }

        void run() {
            std::vector<size_t> todo;
            while (true) {
                bool do_reload = false;
                {
                    std::unique_lock lock(mutex);
                    changed.wait_for(lock, std::chrono::seconds(2), [this]() {
                        return ready && (reload || !queue.empty() ||
                                         (!untraced.empty() && rush2::assets::generation() != index_generation));
                    });
                    if (!ready) continue;
                    do_reload = reload;
                    reload = false;
                    if (!do_reload && !untraced.empty() && rush2::assets::generation() != index_generation) {
                        // Textures that didn't trace get another go once the assets have changed.
                        queue.insert(queue.end(), untraced.begin(), untraced.end());
                        untraced.clear();
                    }
                    if (do_reload) {
                        untraced.clear();
                        for (uint64_t hash : owned) RT64::removeLiveReplacement(hash);
                        owned.clear();
                        queue.clear();
                        for (size_t i = 0; i < all.size(); i++) queue.push_back(i);
                    }
                    todo.assign(queue.begin(), queue.end());
                    queue.clear();
                }
                if (do_reload) {
                    upscales.clear();
                    read_packs();
                    std::unique_lock lock(mutex);
                    if (mod_images_stale) {
                        mod_images.clear();
                        mod_images_stale = false;
                    }
                }
                size_t made = 0, unmatched = 0;
                for (size_t i : todo) {
                    Drawn d;
                    bool ui_on;
                    {
                        std::unique_lock lock(mutex);
                        d = all[i];
                        ui_on = ui_enabled;
                    }
                    if (mod_replace(d)) {
                        made++;
                        continue;
                    }
                    if (!d.world && !ui_on) {
                        set_outcome(d.hash, Outcome::Interface);
                        continue;
                    }
                    if (!rush2::upscale::worth_upscaling(d.image)) { // Never dumped, so no pack has it.
                        set_outcome(d.hash, Outcome::Small);
                        continue;
                    }
                    if (files.empty()) continue;
                    if (files.count(d.hash)) { // A pack replaces it as drawn.
                        set_outcome(d.hash, Outcome::Pack);
                        continue;
                    }
                    Match m = find(d);
                    if (m.base < 0 && rush2::assets::generation() != index_generation) {
                        build(); // The assets changed since the index was made: it may be there now.
                        m = find(d);
                    }
                    if (m.base < 0) {
                        unmatched++;
                        untraced.push_back(i); // Tried again after the next rebuild.
                        set_outcome(d.hash, Outcome::Untraced);
                        continue;
                    }
                    if (bases[m.base].ui && !ui_on) {
                        set_outcome(d.hash, Outcome::Interface);
                        continue;
                    }
                    Image up;
                    if (!base_upscale(bases[m.base], up)) {
                        set_outcome(d.hash, Outcome::NoUpscale);
                        continue;
                    }
                    const Base& b = bases[m.base];
                    const size_t row = size_t(b.width) * 4;
                    bool rows_of_base = b.width == d.image.width && m.dy >= 0 && m.dy + d.image.height <= b.height &&
                                        memcmp(&b.image.rgba[m.dy * row], d.image.rgba.data(), d.image.rgba.size()) == 0;
                    if (rows_of_base) {
                        // The base itself, or a strip of it (a big image the game draws in pieces): the base's
                        // upscale, uploaded once, as the region of it this texture is, so filtering runs on across the
                        // strips' edges.
                        uint64_t content = b.key ^ 0xBA5E0000BA5Eull ^ (pack_generation * 0x9E3779B97F4A7C15ull);
                        std::vector<uint8_t> bytes;
                        if (!RT64::hasLiveReplacementContent(content)) bytes = rush2::upscale::make_dds(up);
                        RT64::ReplacementRegion region;
                        if (d.image.height != b.height) {
                            region.y = uint32_t(m.dy);
                            region.sourceWidth = b.width;
                            region.sourceHeight = b.height;
                        }
                        RT64::addLiveReplacement(d.hash, content, bytes, region);
                    }
                    else {
                        Image image;
                        if (m.dy != 0 || !derive_stripes(d, b, up, image)) image = derive(d, b, m.dy, up);
                        rush2::upscale::restore_alpha(image, d.image);
                        uint64_t content = d.hash ^ 0xDE51BEDull ^ (pack_generation * 0x9E3779B97F4A7C15ull);
                        RT64::addLiveReplacement(d.hash, content, rush2::upscale::make_dds(image));
                    }
                    set_outcome(d.hash, Outcome::Derived);
                    std::unique_lock lock(mutex);
                    owned.insert(d.hash);
                    made++;
                }
                {
                    std::unique_lock pack_lock(pack_mutex);
                    for (Pack& pack : packs) pack.zip.reset();
                }
                // A summary now and then, not a line per texture.
                made_total += made;
                unmatched_total += unmatched;
                auto now = std::chrono::steady_clock::now();
                if ((made_total || unmatched_total) && now - last_log > std::chrono::seconds(10)) {
                    printf("[derive] %zu replacements derived, %zu textures not traced to the catalog\n", made_total,
                           unmatched_total);
                    fflush(stdout);
                    made_total = unmatched_total = 0;
                    last_log = now;
                }
            }
        }

        std::mutex mutex;
        std::condition_variable changed;
        bool ready = false, reload = false;
        bool ui_enabled = false;           // Upscale Interface.
        std::unordered_set<uint64_t> seen, owned;
        std::unordered_set<uint64_t> ui_keys, ui_index_keys; // Interface art (build()).
        std::unordered_map<uint64_t, ModTexture> mods;       // Car mods' stand-ins by index_key.
        bool mod_images_stale = false;
        struct ModImage {
            bool tried = false, ok = false;
            Image image, mask;
        };
        std::unordered_map<uint64_t, ModImage> mod_images; // Worker only: decoded once per ModTexture::id.
        std::unordered_map<uint64_t, Outcome> outcomes;
        std::vector<Drawn> all;
        std::deque<size_t> queue;

        // Worker only (after build).
        std::vector<Base> bases;
        std::unordered_map<uint64_t, int> by_key, by_index;
        std::unordered_map<uint64_t, std::vector<int>> stripes_by_size; // width << 32 | height -> stripe tiles
        uint64_t index_generation = 0;     // rush2::assets::generation() the index was built at.
        uint64_t pack_generation = 0;      // Counts read_packs: part of the live replacements' content keys.
        std::vector<size_t> untraced;      // Drawn textures (in all) not traced yet.
        size_t made_total = 0, unmatched_total = 0;
        std::chrono::steady_clock::time_point last_log;
        std::vector<uint64_t> piece_keys;
        std::vector<uint32_t> piece_at;   // base << 11 | row
        struct Pack {
            std::filesystem::path path;
            bool is_zip;
            std::unique_ptr<rush2::upscale::ZipReader> zip; // Open during a batch.
        };
        std::mutex pack_mutex; // Guards packs (hold_packs).
        std::vector<Pack> packs;
        std::unordered_map<uint64_t, PackFile> files;
        std::unordered_map<uint64_t, Image> upscales;
    };

    Deriver& deriver() {
        static Deriver d;
        return d;
    }
}

void start() {
    deriver().start();
}

void drawn(uint64_t hash, uint64_t key, uint64_t index_key, const Image& image, std::vector<uint8_t> indices, uint8_t,
           bool world) {
    deriver().drawn(hash, key, index_key, image, std::move(indices), world);
}

void set_ui(bool enabled) {
    deriver().set_ui(enabled);
}

void set_keep_car_colors(bool enabled) {
    deriver().set_keep_car_colors(enabled);
}

bool index_ready() {
    return deriver().index_ready();
}

bool interface_texture(uint64_t key, uint64_t index_key) {
    return deriver().interface_texture(key, index_key);
}

bool owns(uint64_t hash) {
    return deriver().owns(hash);
}

Outcome outcome(uint64_t hash) {
    return deriver().outcome(hash);
}

void packs_changed() {
    deriver().packs_changed();
}

std::unique_lock<std::mutex> hold_packs() {
    return deriver().hold_packs();
}

void set_mod_textures(std::unordered_map<uint64_t, ModTexture> textures) {
    deriver().set_mod_textures(std::move(textures));
}

bool mod_texture(uint64_t index_key, uint64_t key) {
    return deriver().mod_texture(index_key, key);
}

}
