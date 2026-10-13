// Derived replacements (include/texture_derive.h, docs/texture_upscaling.md "Derived replacements").

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <list>
#include <map>
#include <memory>
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
#include "body_sheet.h"
#include "car1_stripes.h"
#include "texture_derive.h"
#include "track_cache.h"

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
        int car = -1;                   // From car container car_entries[car] (built-in upscaling, sheets).
        int decal = -1;                 // A Rush 2 panel with an SF Rush decal: the base of its decal.
        // An SF Rush decal (rush2::car1stripes::DecalTexture): image is the decal alone, indices the panel's with
        // the decal written in (what the game draws, for tracing), plain the panel's base. One per panel (the clean
        // and damaged panels' share the key, so the image and its upscale).
        int plain = -1;
        bool decal_stripe = false;      // The decal takes the STRIPE COLOR (only its shape counts).
        std::vector<uint8_t> decal_mask; // Per texel: 1 where the decal is.
    };

    // A texture drawn that no pack replaces.
    struct Drawn {
        uint64_t hash, key, index_key;
        Image image;
        std::vector<uint8_t> indices;
        bool world;                     // Drawn in 3D (else 2D: interface).
        std::chrono::steady_clock::time_point at; // When it was first drawn (RUSH2_DERIVE_TIMING).
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
            all.push_back({ hash, key, index_key, image, std::move(indices), world, std::chrono::steady_clock::now() });
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

        void set_mod_textures(const std::string& source, std::unordered_map<uint64_t, ModTexture> textures) {
            std::unique_lock lock(mutex);
            mod_sources[source] = std::move(textures);
            mods.clear();
            for (const auto& [name, set] : mod_sources) mods.insert(set.begin(), set.end());
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

        bool builtin_car(uint64_t key, uint64_t index_key) {
            if (rush2::upscale::builtin_scale() == 0) return false;
            std::unique_lock lock(mutex);
            return car_keys.count(key) != 0 || (index_key != 0 && car_index_keys.count(index_key) != 0);
        }

    private:
        // The catalog's textures and the index of their row pieces, from the game's assets as they are now (the
        // recomp builds some as it goes: the track select's Rush 2049 previews appear when it's opened).
        void build() {
            auto started = std::chrono::steady_clock::now();
            index_generation = rush2::assets::generation();
            bases.clear();
            car_entries.clear();
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
                const int car = e.path.rfind("cars/", 0) == 0 ? int(car_entries.size()) : -1;
                if (car >= 0) car_entries.push_back(e);
                // The SF Rush decal panels, before the textures move into the bases.
                std::vector<rush2::car1stripes::DecalTexture> decals;
                if (car >= 0) decals = rush2::car1stripes::decal_textures(e, c, containers, paint_palette);
                std::vector<uint64_t> texture_keys;
                for (const rush2::catalog::Texture& t : c.textures) texture_keys.push_back(t.key);
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
                    b.car = car;
                    int id = int(bases.size());
                    by_key.emplace(b.key, id);
                    if (b.index_key) by_index.emplace(b.index_key, id);
                    if (b.stripe) stripes_by_size[(uint64_t(b.width) << 32) | b.height].push_back(id);
                    bases.push_back(std::move(b));
                }
                // Each panel's SF Rush decal is a base of its own, in the dump as <panel>_SFRUSH: traced from the
                // drawn panel by its stamped indices (the fixed colors' decals) or through its plain panel
                // (find_decal), and drawn over the plain panel's upscale (derive_decal).
                for (rush2::car1stripes::DecalTexture& dt : decals) {
                    auto plain = by_key.find(texture_keys[dt.texture]);
                    if (plain == by_key.end() || bases[plain->second].decal >= 0) continue;
                    const Base& p = bases[plain->second];
                    Base b;
                    b.key = dt.key;
                    b.width = p.width;
                    b.height = p.height;
                    b.indexed = true;
                    b.edge_s = p.edge_s;
                    b.edge_t = p.edge_t;
                    b.image = std::move(dt.image);
                    b.indices = std::move(dt.indices);
                    b.decal_mask = std::move(dt.mask);
                    b.decal_stripe = dt.stripe_color;
                    b.plain = plain->second;
                    b.car = car;
                    int id = int(bases.size());
                    bases[plain->second].decal = id;
                    by_key.emplace(b.key, id); // The first of the panels sharing it stands for the image.
                    bases.push_back(std::move(b));
                }
            }
            car_sheets.assign(car_entries.size(), {});
            for (uint64_t k : other) ui.erase(k);
            for (uint64_t k : other_index) ui_index.erase(k);
            for (Base& b : bases) b.ui = ui.count(b.key) != 0;
            std::unordered_set<uint64_t> car_key_set, car_index_set;
            for (const Base& b : bases) {
                if (b.car < 0) continue;
                car_key_set.insert(b.key);
                if (b.index_key) car_index_set.insert(b.index_key);
            }
            {
                std::unique_lock lock(mutex);
                ui_keys.swap(ui);
                ui_index_keys.swap(ui_index);
                car_keys.swap(car_key_set);
                car_index_keys.swap(car_index_set);
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
        // A Rush 2 panel drawn with its SF Rush decal: the plain panel with only the decal's texels changed (a STRIPE
        // COLOR decal's indices follow the color, so the decal base's own indices match only one) has its decal's
        // base returned, drawn over it (derive_decal). Else m's base.
        int find_decal(const Drawn& d, const Match& m) const {
            const Base& plain = bases[m.base];
            if (m.dy != 0 || plain.decal < 0 || d.indices.size() != plain.indices.size()) return m.base;
            const Base& decal = bases[plain.decal];
            size_t changed = 0;
            for (size_t i = 0; i < d.indices.size(); i++) {
                if (d.indices[i] == plain.indices[i]) continue;
                if (!decal.decal_mask[i]) return m.base; // Something else changed it (Rush 2's own stripes).
                changed++;
            }
            return changed > 0 ? plain.decal : m.base;
        }

        // Built-in upscaling of a car's textures (HQ2x or HQ4x, rush2::upscale::builtin_scale): the upscale a pack
        // would have, made here, so built-in mode gets what a pack gets (the paint recolored from the preview, Keep Car
        // Colors, stripes, SF Rush decals). Body tiles are upscaled on their car's sheets (rush2::sheet), so hqx sees
        // across the seams; anything else alone. Each kept on disk (track_cache, per base and build). A car's sheets
        // are rendered and cut by one thread at a time (car_locks); its other tiles' threads then find their cuts.
        bool builtin_upscale(const Base& b, uint32_t scale, Image& out) {
            const std::string blob = "hq" + std::to_string(scale) + "_" + rush2::upscale::key_name(b.key);
            // A stripe tile's blob is keyed apart: the first ones were padded with the tile's own edge modes.
            const uint64_t source = (b.key | 1) ^ (b.stripe ? 0x5354524950453200ull : 0);
            std::vector<uint8_t> bytes;
            if (rush2::track_cache::load_blob(blob, source, bytes) && rush2::upscale::read_dds(bytes, out)) return true;
            auto alone = [&](const Base& t) {
                const Image& source = upscaled_from(t);
                // A stripe tile (or an SF Rush decal) lies on its panel texel for texel and never repeats: padded by
                // wrapping, hqx blended the stripe's opaque edge with the clear far side, and derive_stripes (stripe at
                // full alpha only) drew a line of paint down the middle of a stripe where two mirrored panels meet.
                const bool stamp = t.stripe || t.plain >= 0;
                const Edge s = stamp ? Edge::Clamp : t.edge_s, tt = stamp ? Edge::Clamp : t.edge_t;
                return rush2::upscale::unpad(
                    rush2::upscale::hq_upscale(rush2::upscale::pad(source, hq_border, s, tt), scale), source, hq_border);
            };
            const std::string name = rush2::upscale::key_name(b.key);
            if (b.car >= 0) {
                std::unique_lock car_lock(car_locks[size_t(b.car) % std::size(car_locks)]);
                {
                    std::unique_lock cache_lock(cache_mutex); // Cut while this thread waited for the car.
                    if (auto it = upscales.find(b.key); it != upscales.end() && !it->second.empty()) {
                        out = it->second;
                        return true;
                    }
                }
                for (const rush2::sheet::Sheet& sheet : sheets_of(b.car)) {
                    std::vector<std::string> tiles = rush2::sheet::tiles(sheet);
                    if (std::find(tiles.begin(), tiles.end(), name) == tiles.end()) continue;
                    // The whole sheet at once: every tile of it is wanted with this one.
                    auto base_of = [&](const std::string& tile) -> const Base* {
                        uint64_t key;
                        if (!rush2::upscale::parse_key_name(tile, key)) return nullptr;
                        auto it = by_key.find(key);
                        return it == by_key.end() ? nullptr : &bases[it->second];
                    };
                    Image image = rush2::sheet::render(sheet, [&](const std::string& tile) -> const Image* {
                        const Base* t = base_of(tile);
                        return t ? &upscaled_from(*t) : nullptr;
                    });
                    Image up = rush2::upscale::unpad(
                        rush2::upscale::hq_upscale(rush2::upscale::pad(image, hq_border, Edge::Clamp, Edge::Clamp), scale),
                        image, hq_border);
                    for (const std::string& tile : tiles) {
                        const Base* t = base_of(tile);
                        Image cut;
                        if (t == nullptr || !rush2::sheet::cut(sheet, up, tile, upscaled_from(*t), alone(*t), cut)) continue;
                        rush2::track_cache::save_blob("hq" + std::to_string(scale) + "_" + tile, t->key | 1,
                                                      rush2::upscale::make_dds(cut));
                        if (t->key == b.key) {
                            out = cut;
                        }
                        else {
                            std::unique_lock cache_lock(cache_mutex);
                            upscales[t->key] = std::move(cut);
                        }
                    }
                    if (!out.empty()) return true;
                }
            }
            out = alone(b);
            rush2::track_cache::save_blob(blob, source, rush2::upscale::make_dds(out));
            return !out.empty();
        }

        // A car's sheets for built-in upscaling, laid out as Dump Textures lays them (body, damaged, SF Rush decal),
        // tiles by key name. Kept on disk (track_cache) per car file and build. Its car_locks entry held.
        const std::vector<rush2::sheet::Sheet>& sheets_of(int car) {
            CarSheets& cs = car_sheets[size_t(car)];
            if (cs.built) return cs.sheets;
            cs.built = true;
            const rush2::catalog::Entry& e = car_entries[size_t(car)];
            if (e.data == nullptr) return cs.sheets;
            const std::string blob = "body_sheets_" + rush2::catalog::file_name(e.path);
            // ^ 2: the layouts before the decals were laid out alone (a damaged decal sheet, panels with decals).
            const uint64_t source = (rush2::track_cache::hash(e.data->data(), e.data->size()) | 1) ^ 2;
            std::vector<uint8_t> bytes;
            if (rush2::track_cache::load_blob(blob, source, bytes)) {
                cs.sheets = rush2::sheet::read_layout(std::string(bytes.begin(), bytes.end()));
                return cs.sheets;
            }
            rush2::catalog::Container c;
            std::string error;
            if (rush2::catalog::read_container(*e.data, c, error, true, false, &e.copies)) {
                std::vector<std::string> paths(c.textures.size());
                std::unordered_map<std::string, std::string> by_name; // Texture name -> key name.
                std::unordered_map<std::string, std::string> name_of;  // Key name -> texture name.
                for (size_t i = 0; i < c.textures.size(); i++) {
                    if (!by_key.count(c.textures[i].key)) continue;
                    paths[i] = rush2::upscale::key_name(c.textures[i].key);
                    by_name.emplace(c.textures[i].name, paths[i]);
                    name_of.emplace(paths[i], c.textures[i].name);
                }
                rush2::sheet::Sheet sheet;
                if (rush2::sheet::build(c, paths, "body", sheet)) {
                    // Another version of each tile, laid out the same (as write_sheets in src/texture_upscale.cpp).
                    auto variant = [&](const rush2::sheet::Sheet& from, const std::string& suffix, auto version_of) {
                        rush2::sheet::Sheet v = from;
                        v.path += suffix;
                        v.pieces.clear();
                        for (rush2::sheet::Piece piece : from.pieces) {
                            piece.tile = version_of(piece.tile);
                            if (!piece.tile.empty()) v.pieces.push_back(std::move(piece));
                        }
                        return v;
                    };
                    auto damaged = [&](const std::string& tile) -> std::string {
                        auto n = name_of.find(tile);
                        if (n == name_of.end()) return "";
                        if (auto d = by_name.find(n->second + "_D1"); d != by_name.end()) return d->second;
                        size_t at = n->second.rfind("D0_");
                        if (at == std::string::npos) return "";
                        auto d = by_name.find(n->second.substr(0, at) + "D1_" + n->second.substr(at + 3));
                        return d == by_name.end() ? "" : d->second;
                    };
                    auto decal = [&](const std::string& tile) -> std::string {
                        uint64_t key;
                        if (!rush2::upscale::parse_key_name(tile, key)) return "";
                        auto it = by_key.find(key);
                        if (it == by_key.end() || bases[it->second].decal < 0) return "";
                        return rush2::upscale::key_name(bases[bases[it->second].decal].key);
                    };
                    rush2::sheet::Sheet hurt = variant(sheet, "_D1", damaged);
                    rush2::sheet::Sheet decals = variant(sheet, "_SFRUSH", decal);
                    cs.sheets.push_back(std::move(sheet));
                    for (rush2::sheet::Sheet* v : { &hurt, &decals }) {
                        if (!v->pieces.empty()) cs.sheets.push_back(std::move(*v));
                    }
                }
            }
            std::string text = rush2::sheet::write_layout(cs.sheets);
            rush2::track_cache::save_blob(blob, source, std::vector<uint8_t>(text.begin(), text.end()));
            return cs.sheets;
        }

        // Called from several threads at once (a batch loads its bases' upscales on every core, then recolors): the
        // cache is locked only around its map, a pack's zip only while bytes are read from it, so the decoding, disk
        // reads and hqx run side by side.
        bool base_upscale(const Base& b, Image& out) {
            {
                std::unique_lock cache_lock(cache_mutex);
                if (auto it = upscales.find(b.key); it != upscales.end()) {
                    out = it->second;
                    return !out.empty();
                }
            }
            Image image;
            for (uint64_t id : { b.hash, b.key }) {
                auto f = id ? files.find(id) : files.end();
                if (f == files.end()) continue;
                Pack& pack = packs[f->second.pack];
                for (const char* ext : { ".dds", ".png" }) {
                    std::vector<uint8_t> bytes;
                    std::string name = f->second.path + ext;
                    bool got;
                    {
                        std::unique_lock pack_lock(pack_mutex);
                        if (pack.is_zip && !pack.zip) pack.zip = std::make_unique<rush2::upscale::ZipReader>(pack.path);
                        got = pack.is_zip ? pack.zip->read(name, bytes)
                                          : !(bytes = rush2::upscale::read_file(pack.path / std::filesystem::u8path(name))).empty();
                    }
                    if (got && rush2::upscale::decode_image(bytes, image)) break;
                }
                if (!image.empty()) break;
            }
            if (image.empty() && b.car >= 0) {
                if (uint32_t scale = rush2::upscale::builtin_scale()) builtin_upscale(b, scale, image);
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
            std::unique_lock cache_lock(cache_mutex);
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

        // A Rush 2 panel drawn with its SF Rush decal (car1stripes, written into the panel's texels): the plain panel's
        // upscale in the drawn paint, with the decal over it by its upscale's alpha, as derive_stripes lays a stripe.
        // A STRIPE COLOR decal's texels are the panel's remapped to the color (so the panel's upscale recolored to the
        // stripe shades); a fixed-color decal's are its own (its upscale recolored to the colors drawn).
        Image derive_decal(const Drawn& d, const Base& b, const Base& decal, const Image& up, Image decal_up) {
            const size_t n = size_t(b.width) * b.height;
            if (d.indices.size() != n || b.indices.size() != n || decal.decal_mask.size() != n) return derive(d, b, 0, up);
            // Each palette index's color off the decal and under it, from the texels drawn each way.
            uint32_t paint[256] = {}, stamped[256] = {};
            bool has_paint[256] = {}, has_stamped[256] = {};
            for (size_t i = 0; i < n; i++) {
                uint32_t color;
                memcpy(&color, &d.image.rgba[i * 4], 4);
                (decal.decal_mask[i] ? stamped : paint)[b.indices[i]] = color;
                (decal.decal_mask[i] ? has_stamped : has_paint)[b.indices[i]] = true;
            }
            // An index drawn only one way takes the nearest index's color that way (the ramps run in steps).
            auto fill = [](uint32_t* colors, const bool* has) {
                for (int i = 0; i < 256; i++) {
                    if (has[i]) continue;
                    for (int k = 1; k < 256; k++) {
                        if (i - k >= 0 && has[i - k]) { colors[i] = colors[i - k]; break; }
                        if (i + k < 256 && has[i + k]) { colors[i] = colors[i + k]; break; }
                    }
                }
            };
            fill(paint, has_paint);
            Image painted = d.image;
            for (size_t i = 0; i < n; i++) memcpy(&painted.rgba[i * 4], &paint[b.indices[i]], 4);
            Image under = recolor(b, up, painted), over;
            if (decal_up.width != up.width || decal_up.height != up.height) {
                decal_up = rush2::upscale::resize_bilinear(decal_up, up.width, up.height);
            }
            if (decal.decal_stripe) {
                fill(stamped, has_stamped);
                Image striped = d.image;
                for (size_t i = 0; i < n; i++) memcpy(&striped.rgba[i * 4], &stamped[b.indices[i]], 4);
                over = recolor(b, up, striped);
            }
            else {
                // The decal's own image in the colors drawn on it (clear off it, as the image is).
                Image colors = decal.image;
                for (size_t i = 0; i < n; i++) {
                    if (decal.decal_mask[i]) memcpy(&colors.rgba[i * 4], &d.image.rgba[i * 4], 4);
                }
                over = recolor(decal, decal_up, colors);
            }
            Image out = under;
            for (size_t i = 0; i < size_t(up.width) * up.height; i++) {
                float w = decal_up.rgba[i * 4 + 3] / 255.0f;
                if (w <= 0.0f || (!decal.decal_stripe && over.rgba[i * 4 + 3] == 0)) continue;
                // A fixed-color decal's recolor mixes toward its clear texels (black) at its edges: undone by its alpha.
                float a = decal.decal_stripe ? 1.0f : std::max(over.rgba[i * 4 + 3] / 255.0f, 1.0f / 255.0f);
                for (int c = 0; c < 3; c++) {
                    float o = std::min(255.0f, over.rgba[i * 4 + c] / a);
                    out.rgba[i * 4 + c] = uint8_t(under.rgba[i * 4 + c] * (1 - w) + o * w + 0.5f);
                }
            }
            return out;
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
        // brightness only. The recolor plan is kept per base (plans), so another paint of the same panel is quick.
        Image recolor(const Base& b, const Image& up, const Image& colors) {
            std::shared_ptr<const rush2::upscale::RecolorPlan> plan;
            {
                std::unique_lock cache_lock(cache_mutex);
                if (auto it = plans.find(b.key); it != plans.end()) plan = it->second;
            }
            if (!plan || plan->width != up.width || plan->height != up.height) {
                plan = std::make_shared<const rush2::upscale::RecolorPlan>(
                    rush2::upscale::plan_recolor(up, upscaled_from(b), b.edge_s, b.edge_t, plan_threads));
                std::unique_lock cache_lock(cache_mutex);
                // A few hundred kept (a 4x upscale of a 64x32 panel's plan is 400 KB): a car's panels and their
                // stripe tiles, which are wanted together.
                if (plans.size() >= 256) plans.clear();
                plans.insert_or_assign(b.key, plan);
            }
            Image out = rush2::upscale::apply_recolor(*plan, up, upscaled_from(b), colors, b.edge_s, b.edge_t, b.paint);
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
                    std::unique_lock cache_lock(cache_mutex);
                    upscales.clear();
                    plans.clear();
                    cache_lock.unlock();
                    read_packs();
                    std::unique_lock lock(mutex);
                    if (mod_images_stale) {
                        mod_images.clear();
                        mod_images_stale = false;
                    }
                }
                size_t made = 0, unmatched = 0;
                using clock = std::chrono::steady_clock;
                const clock::time_point batch_start = clock::now();
                clock::time_point first_drawn = batch_start;
                // Matched first, one by one (the index, the packs); the recolors, a car's 20-odd panels at once on a
                // paint or car change, then run on every core; RT64 gets them in order after.
                struct Job {
                    Drawn d;
                    int base = -1, dy = 0;
                    Image up;
                    std::vector<uint8_t> dds;
                    int decal = -1;             // An SF Rush decal drawn over base (derive_decal), and its upscale.
                    Image decal_up;
                };
                std::vector<Job> jobs;
                struct Traced {
                    Drawn d;
                    int base = -1, dy = 0, decal = -1;
                };
                std::vector<Traced> traced;
                for (size_t i : todo) {
                    Drawn d;
                    bool ui_on;
                    {
                        std::unique_lock lock(mutex);
                        d = all[i];
                        ui_on = ui_enabled;
                    }
                    first_drawn = std::min(first_drawn, d.at);
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
                    if (files.empty() && rush2::upscale::builtin_scale() == 0) continue;
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
                    // A panel with an SF Rush decal: the plain panel's upscale, with the decal's over it.
                    int decal = -1;
                    if (bases[m.base].plain >= 0) {
                        decal = m.base;
                        m.base = bases[decal].plain;
                    }
                    else if (int found = find_decal(d, m); found != m.base) {
                        decal = found;
                    }
                    traced.push_back({ std::move(d), m.base, m.dy, decal });
                }
                // The bases' upscales, loaded on every core: disk blobs, pack images to decode, hqx of a car's sheets.
                // A car's first view brings 20-odd bases, which loaded one by one took 40-120 ms.
                {
                    std::vector<int> wanted;
                    for (const Traced& t : traced) {
                        wanted.push_back(t.base);
                        if (t.decal >= 0) wanted.push_back(t.decal);
                    }
                    std::sort(wanted.begin(), wanted.end());
                    wanted.erase(std::unique(wanted.begin(), wanted.end()), wanted.end());
                    std::atomic<size_t> next_base = 0;
                    auto load = [&]() {
                        Image scratch;
                        for (size_t k; (k = next_base++) < wanted.size();) base_upscale(bases[wanted[k]], scratch);
                    };
                    const size_t loaders = std::min<size_t>(wanted.size(), std::max(1u, std::thread::hardware_concurrency()) - 1);
                    std::vector<std::thread> threads;
                    for (size_t t = 1; t < loaders; t++) threads.emplace_back(load);
                    load();
                    for (std::thread& t : threads) t.join();
                }
                for (Traced& t : traced) {
                    Drawn& d = t.d;
                    Image up, decal_up;
                    if (!base_upscale(bases[t.base], up)) {
                        set_outcome(d.hash, Outcome::NoUpscale);
                        continue;
                    }
                    if (t.decal >= 0) {
                        if (!base_upscale(bases[t.decal], decal_up)) decal_up = bases[t.decal].image; // Its texels, scaled.
                        jobs.push_back({ std::move(d), t.base, 0, std::move(up), {}, t.decal, std::move(decal_up) });
                        continue;
                    }
                    const Base& b = bases[t.base];
                    const size_t row = size_t(b.width) * 4;
                    bool rows_of_base = b.width == d.image.width && t.dy >= 0 && t.dy + d.image.height <= b.height &&
                                        memcmp(&b.image.rgba[t.dy * row], d.image.rgba.data(), d.image.rgba.size()) == 0;
                    if (rows_of_base) {
                        // The base itself, or a strip of it (a big image the game draws in pieces): the base's
                        // upscale, uploaded once, as the region of it this texture is, so filtering runs on across the
                        // strips' edges.
                        uint64_t content = b.key ^ 0xBA5E0000BA5Eull ^ (pack_generation * 0x9E3779B97F4A7C15ull);
                        std::vector<uint8_t> bytes;
                        if (!RT64::hasLiveReplacementContent(content)) bytes = rush2::upscale::make_dds(up);
                        RT64::ReplacementRegion region;
                        if (d.image.height != b.height) {
                            region.y = uint32_t(t.dy);
                            region.sourceWidth = b.width;
                            region.sourceHeight = b.height;
                        }
                        RT64::addLiveReplacement(d.hash, content, bytes, region);
                        set_outcome(d.hash, Outcome::Derived);
                        std::unique_lock lock(mutex);
                        owned.insert(d.hash);
                        made++;
                        continue;
                    }
                    jobs.push_back({ std::move(d), t.base, t.dy, std::move(up), {} });
                }
                const clock::time_point matched = clock::now();
                // The cores the jobs leave over split each plan search (a lone job gets them all).
                plan_threads = std::max<size_t>(1, (std::max(1u, std::thread::hardware_concurrency()) - 1) /
                                                       std::max<size_t>(1, jobs.size()));
                std::atomic<size_t> next_job = 0;
                auto work = [&]() {
                    for (size_t j; (j = next_job++) < jobs.size();) {
                        Job& job = jobs[j];
                        const Base& b = bases[job.base];
                        Image image;
                        if (job.decal >= 0) image = derive_decal(job.d, b, bases[job.decal], job.up, job.decal_up);
                        else if (job.dy != 0 || !derive_stripes(job.d, b, job.up, image)) image = derive(job.d, b, job.dy, job.up);
                        rush2::upscale::restore_alpha(image, job.d.image);
                        job.dds = rush2::upscale::make_dds(image);
                    }
                };
                const size_t helpers = std::min<size_t>(jobs.size(), std::max(1u, std::thread::hardware_concurrency()) - 1);
                std::vector<std::thread> threads;
                for (size_t t = 1; t < helpers; t++) threads.emplace_back(work);
                work();
                for (std::thread& t : threads) t.join();
                const clock::time_point worked = clock::now();
                for (Job& job : jobs) {
                    uint64_t content = job.d.hash ^ 0xDE51BEDull ^ (pack_generation * 0x9E3779B97F4A7C15ull);
                    RT64::addLiveReplacement(job.d.hash, content, std::move(job.dds));
                    set_outcome(job.d.hash, Outcome::Derived);
                    std::unique_lock lock(mutex);
                    owned.insert(job.d.hash);
                    made++;
                }
                {
                    std::unique_lock pack_lock(pack_mutex);
                    for (Pack& pack : packs) pack.zip.reset();
                }
                // RUSH2_DERIVE_TIMING: where a batch's time went, from its first texture's first draw to RT64 having
                // every replacement (waited: drawn to the batch's start; matched: tracing and the bases' upscales;
                // derived: the recolors and DDS encoding on every core; handed: addLiveReplacement).
                static const bool timing = getenv("RUSH2_DERIVE_TIMING") != nullptr;
                if (timing && !todo.empty()) {
                    auto ms = [](clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); };
                    const clock::time_point done = clock::now();
                    printf("[derive] batch %zu drawn, %zu jobs: waited %.1f ms, matched %.1f, derived %.1f, handed %.1f, "
                           "total %.1f ms\n", todo.size(), jobs.size(), ms(batch_start - first_drawn),
                           ms(matched - batch_start), ms(worked - matched), ms(done - worked), ms(done - first_drawn));
                    fflush(stdout);
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
        std::unordered_set<uint64_t> car_keys, car_index_keys; // Car textures (build(): built-in upscaling's own).
        std::unordered_map<uint64_t, ModTexture> mods;       // Mods' stand-ins (every source's) by key.
        std::map<std::string, std::unordered_map<uint64_t, ModTexture>> mod_sources;
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
        std::mutex cache_mutex; // Guards upscales and plans while a batch's recolors run on several threads.
        // By base key: recolor's plan of its upscale.
        std::unordered_map<uint64_t, std::shared_ptr<const rush2::upscale::RecolorPlan>> plans;
        // Built-in upscaling of the cars (builtin_upscale): their containers (Base::car) and sheets.
        struct CarSheets {
            bool built = false;
            std::vector<rush2::sheet::Sheet> sheets;
        };
        std::vector<rush2::catalog::Entry> car_entries;
        std::vector<CarSheets> car_sheets;  // Each guarded by its car_locks entry.
        std::mutex car_locks[16];
        unsigned plan_threads = 1;         // Threads a recolor plan search may use (set per batch, run()).          // car_sheets[car] and its sheets' rendering: car % 16.
        static constexpr uint32_t hq_border = 8; // Texels of padding around an image given to hqx.
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

bool builtin_car(uint64_t key, uint64_t index_key) {
    return deriver().builtin_car(key, index_key);
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

void set_mod_textures(const std::string& source, std::unordered_map<uint64_t, ModTexture> textures) {
    deriver().set_mod_textures(source, std::move(textures));
}

bool mod_texture(uint64_t index_key, uint64_t key) {
    return deriver().mod_texture(index_key, key);
}

}
