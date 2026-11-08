// A Dreamcast Rush 2049 disc's textures at their full size. See include/rush2049_dc.h and
// docs/rush2049_research/dreamcast.md (Full-size textures).
//
// The files made from the disc load each texture scaled down to fit TMEM where it doesn't
// (src/rush2049dc/rush2049_dc_model.cpp). Every texture's full-size image is kept in
// <app folder>/track_cache/dc_textures, built once per disc and build in the background (converting every file first,
// a few seconds on the first boot): one DDS (with mipmaps) per distinct image, "images" (each image's size, kind and
// the RT64 hashes its textures are drawn with, from replacement_hash) and the stamp file (format, source key, build)
// written last, so a build that was cut short is done again.
//
// rt64.json replaces every scaled-down texture with its image, and RT64 loads the folder like any texture pack (any
// pack the player enables wins over it). Textures drawn at their full size already are only in it when the player
// installed an upscale of them. It is in use whenever a disc is installed: with the disc as the Rush 2049 source (Disc
// mode), and with the N64 ROM as the source (N64 mode), since the track select offers the race tracks' Dreamcast
// versions with either source. The N64 ROM's own textures are left as they are.
//
// The player's upscales of the disc images (Install Upscaled Textures, from the rush2049dc dump) are kept in
// <app folder>/asset_dump/generated_texture_cache/dreamcast/<key>.dds (made only when there are some) and linked into
// the folder's "upscaled" directory, where rt64.json uses them in place of the disc's.

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <tuple>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "recompui/recompui.h"
#include "recompui/renderer.h"
#include "librecomp/game.hpp"
#include "util/file.h"

#include "rush2049_dc.h"
#include "texture_upscale.h"
#include "track_cache.h"

namespace upscale = rush2::upscale;
using rush2::rom2049::dc::TextureMode;

namespace {
    constexpr const char* format = "v5"; // of the folder's files; a new one rebuilds it (v5: no thumbnails)
    // The matching of N64 textures to disc images (removed) kept its results here; deleted when loading.
    constexpr const char* old_matches_files[] = { "matches_v4", "matches_v5" };
    constexpr uint8_t flag_shrunk = 1, flag_damaged = 4;

    // An image of the folder.
    struct ImageInfo {
        uint64_t key = 0;
        uint32_t w = 0, h = 0;
        uint8_t flags = 0;
        std::vector<std::pair<uint64_t, bool>> hashes; // RT64 hash, whether that texture is scaled down
        std::string name, file; // The first texture's record name and disc file (no extension), for the dumps.
    };

    std::filesystem::path pack_dir() {
        return rush2::track_cache::directory() / "dc_textures";
    }

    std::filesystem::path upscale_store() {
        return upscale::generated_cache_dir() / "dreamcast";
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

    // State. Lock order: build_mutex, then mutex.
    std::mutex build_mutex;      // one build or load at a time
    std::mutex mutex;
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
                info.name = t.name;
                info.file = t.file.substr(0, t.file.rfind('.'));
                made.push_back(std::move(info));
            }
            ImageInfo& info = made[it->second];
            info.flags |= (t.shrunk ? flag_shrunk : 0) | (t.damaged ? flag_damaged : 0);
            if (hash != 0) info.hashes.emplace_back(hash, t.shrunk);
        }
        // Brake lights (issue #44): src/rush2049/car2049.cpp draws a car's scaled-down texture lit while braking, its
        // lights found by rush2::rom2049::dc::brake_light_mask over every job's copy of it; that copy gets the full-size
        // image lit the same way (the mask found again over the full-size images).
        {
            std::map<std::tuple<std::string, std::string, bool>, std::vector<const rush2::rom2049::SourceTexture*>> groups;
            for (const rush2::rom2049::SourceTexture& t : textures) {
                bool tail = t.job == 0 && !t.damaged && rush2::rom2049::dc::tail_texture_name(t.name);
                if ((t.job == 0 && !tail) || !t.shrunk || t.file.rfind("CAR", 0) != 0 ||
                    rush2::rom2049::dc::front_texture_name(t.name)) {
                    continue;
                }
                // The car (CARn of CARnPJm.xxx): its jobs' copies of a texture share the name.
                std::string car = t.file.substr(0, t.file.find_first_not_of("CAR0123456789"));
                groups[{ car, t.name, t.damaged }].push_back(&t);
            }
            for (const auto& [key, group] : groups) {
                if (refreshes != refresh) return false;
                const auto& [car, name, damaged] = key;
                // The jobs' pristine copies decide, for the damaged copies too.
                auto pristine = groups.find({ car, name, false });
                if (pristine == groups.end()) continue;
                const auto& jobs = pristine->second;
                const rush2::rom2049::SourceTexture& first = *jobs[0];
                std::vector<uint8_t> small_mask, full_mask;
                std::vector<upscale::Image> fulls(jobs.size());
                bool fulls_ok = true;
                for (size_t i = 0; i < jobs.size() && fulls_ok; i++) {
                    int w = 0, h = 0;
                    fulls_ok = source.source_image(*jobs[i], fulls[i].rgba, w, h) && w > 0 && h > 0;
                    fulls[i].width = (uint32_t)w;
                    fulls[i].height = (uint32_t)h;
                }
                if (!fulls_ok) continue;
                if (first.job == 0) {
                    small_mask = rush2::rom2049::dc::tail_texture_mask(first.texels, first.w, first.h, first.rgba32);
                    full_mask = rush2::rom2049::dc::tail_texture_mask(fulls[0].rgba, fulls[0].width, fulls[0].height, true);
                }
                else {
                    std::vector<const std::vector<uint8_t>*> small, full;
                    bool same = true;
                    for (size_t i = 0; i < jobs.size(); i++) {
                        small.push_back(&jobs[i]->texels);
                        full.push_back(&fulls[i].rgba);
                        same = same && fulls[i].width == fulls[0].width && fulls[i].height == fulls[0].height;
                    }
                    if (!same) continue;
                    small_mask = rush2::rom2049::dc::brake_light_mask(small, first.w, first.h, first.rgba32);
                    full_mask = rush2::rom2049::dc::brake_light_mask(full, fulls[0].width, fulls[0].height, true);
                }
                if (small_mask.empty()) continue;
                if (full_mask.empty()) {
                    full_mask = small_mask; // scaled up by light_texels
                }
                const bool from_small = full_mask.size() != size_t(fulls[0].width) * fulls[0].height;
                for (const rush2::rom2049::SourceTexture* t : group) {
                    rush2::rom2049::SourceTexture lit = *t;
                    lit.texels = rush2::rom2049::dc::light_texels(t->texels, t->w, t->h, t->rgba32, small_mask, first.w, first.h);
                    uint64_t hash = rush2::rom2049::dc::replacement_hash(lit);
                    if (hash == 0 || !hashes.insert(hash).second) continue;
                    upscale::Image image;
                    int w = 0, h = 0;
                    if (!source.source_image(*t, image.rgba, w, h) || w <= 0 || h <= 0) continue;
                    image.width = (uint32_t)w;
                    image.height = (uint32_t)h;
                    image.rgba = rush2::rom2049::dc::light_texels(image.rgba, image.width, image.height, true, full_mask,
                                                                 from_small ? first.w : fulls[0].width,
                                                                 from_small ? first.h : fulls[0].height);
                    uint64_t key = upscale::content_key(image);
                    auto [it, added] = index.try_emplace(key, made.size());
                    if (added) {
                        if (!upscale::write_file(dir / (upscale::key_name(key) + ".dds"), upscale::make_dds(image))) return false;
                        ImageInfo info;
                        info.key = key;
                        info.w = image.width;
                        info.h = image.height;
                        info.name = t->name + "_LIT";
                        info.file = t->file.substr(0, t->file.rfind('.'));
                        made.push_back(std::move(info));
                    }
                    ImageInfo& info = made[it->second];
                    // Only drawn for its lit texels.
                    info.flags |= flag_shrunk | (damaged ? flag_damaged : 0);
                    info.hashes.emplace_back(hash, true);
                }
            }
        }
        std::vector<uint8_t> out;
        auto put = [&](const void* p, size_t n) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
        uint32_t count = (uint32_t)made.size();
        put("DCI4", 4);
        put(&count, 4);
        for (const ImageInfo& info : made) {
            uint32_t nh = (uint32_t)info.hashes.size();
            put(&info.key, 8); put(&info.w, 4); put(&info.h, 4); put(&info.flags, 1);
            for (const std::string* s : { &info.name, &info.file }) {
                uint8_t n = (uint8_t)std::min<size_t>(s->size(), 255);
                put(&n, 1); put(s->data(), n);
            }
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

    // Reads the folder's images (mutex held).
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
        if (!get(magic, 4) || memcmp(magic, "DCI4", 4) != 0 || !get(&count, 4)) return false;
        for (uint32_t i = 0; i < count; i++) {
            ImageInfo info;
            uint32_t nh = 0;
            if (!get(&info.key, 8) || !get(&info.w, 4) || !get(&info.h, 4) || !get(&info.flags, 1)) {
                unload();
                return false;
            }
            for (std::string* s : { &info.name, &info.file }) {
                uint8_t n = 0;
                if (!get(&n, 1) || o + n > in.size()) {
                    unload();
                    return false;
                }
                s->assign((const char*)&in[o], n);
                o += n;
            }
            if (!get(&nh, 4) || nh > in.size()) {
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
        for (const char* old : old_matches_files) {
            std::error_code old_ec;
            std::filesystem::remove(pack_dir() / old, old_ec);
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
            // The converted files sample each scaled-down texture where the disc samples its image (half a texel in
            // from the disc's coordinate, rush2049_dc_model.cpp make()); RT64's usual half shift would put the image
            // half one of its texels off that, enough to pull the far edge of a wrapping texture into the seams
            // between a car's panels (docs/rush2049_research/dreamcast.md, Replacement texel centers).
            t.shift = "center";
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

    // The folder couldn't be built or read (a full disk, a read-only app folder): say so rather than quietly drawing
    // the scaled-down textures.
    void failed() {
        std::string message = "Failed to build the Dreamcast disc's textures in:\n" + upscale::path_utf8(pack_dir()) +
            "\n\nThe game uses the scaled-down textures until they can be written there.";
        recompui::message_box(message.c_str());
    }

    // Puts the source and the stored disc in effect: builds or loads the folder as needed, in the background.
    void refresh() {
        uint64_t refresh = ++refreshes;
        std::shared_ptr<const rush2::rom2049::Source> from;
        std::filesystem::path pack;
        TextureMode want;
        {
            std::lock_guard lock{ mutex };
            want = active == nullptr                ? TextureMode::Off
                 : active->is_dreamcast()             ? TextureMode::Disc
                 : !disc_pack.empty()                 ? TextureMode::N64
                                                      : TextureMode::Off;
            from = want == TextureMode::Disc ? active : nullptr;
            pack = disc_pack;
            if (want == TextureMode::Off) set_renderer_pack(false);
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
                    if (!build(*from, stamp, refresh)) {
                        if (refreshes == refresh) failed();
                        return;
                    }
                }
                bool indexed;
                {
                    std::lock_guard lock{ mutex };
                    indexed = load_index(stamp);
                    if (indexed) upscale_generation = 0;
                }
                if (!indexed) {
                    failed();
                    return;
                }
            }
            std::lock_guard lock{ mutex };
            if (refreshes != refresh) return;
            if (upscale_generation == 0 || !std::filesystem::exists(pack_dir() / "rt64.json")) {
                write_database();
                upscale_generation = 1;
            }
            // Both modes load the pack: the track select offers the race tracks' Dreamcast versions with either source
            // (rush2::track2049::disc_courses_available), whose textures it replaces.
            set_renderer_pack(true);
            generation++;
        }).detach();
    }
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
    return mode != TextureMode::Off && pack_set && pack_hashes.count(hash) != 0;
}

size_t rush2::rom2049::dc::dump_images(const std::filesystem::path& dir, bool interface, std::atomic<size_t>* done,
                                       std::vector<std::pair<std::string, uint64_t>>* named) {
    std::vector<std::pair<uint64_t, std::string>> keys; // With their paths.
    {
        std::lock_guard lock{ mutex };
        if (!index_ready) return 0;
        std::set<uint64_t> used;
        for (const auto& [key, at] : by_key) used.insert(key);
        // Named <disc file>/<texture name>, "~2"... for another image of the same name; by key when it has none.
        std::set<std::string> taken;
        auto safe = [](const std::string& s) {
            std::string o;
            for (char c : s) o += (isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.') ? c : '_';
            return o;
        };
        for (uint64_t key : used) {
            auto it = by_key.find(key);
            if (it == by_key.end() || (images[it->second].flags & flag_damaged)) continue;
            const ImageInfo& info = images[it->second];
            if (!interface && (info.file == "HUD" || info.file == "HUDBATTLE" || info.file == "HUDCOM" || info.file == "SELCAR" ||
                               info.file == "SELTRK")) {
                continue; // Interface art (Upscale Interface off).
            }
            std::string path = upscale::key_name(key);
            if (!info.name.empty()) {
                std::string base = (info.file.empty() ? std::string() : safe(info.file) + "/") + safe(info.name);
                path = base;
                for (int n = 2; taken.count(path); n++) path = base + "~" + std::to_string(n);
            }
            taken.insert(path);
            keys.emplace_back(key, path);
        }
    }
    std::error_code ec;
    for (const auto& [key, path] : keys) {
        std::filesystem::path png = dir / std::filesystem::u8path(path + ".png");
        upscale::Image image;
        if (!std::filesystem::exists(png, ec) &&
            upscale::read_dds(upscale::read_file(pack_dir() / (upscale::key_name(key) + ".dds")), image)) {
            std::filesystem::create_directories(png.parent_path(), ec);
            upscale::write_png(png, image);
        }
        if (named != nullptr) named->emplace_back(path, key);
        if (done != nullptr) (*done)++;
    }
    return keys.size();
}

bool rush2::rom2049::dc::images_ready() {
    std::lock_guard lock{ mutex };
    return index_ready;
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
    std::lock_guard lock{ mutex };
    upscaled.clear();
}

bool rush2::rom2049::dc::add_upscale(uint64_t key, const upscale::Image& image) {
    std::error_code ec;
    std::filesystem::create_directories(upscale_store(), ec);
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
        if (mode != TextureMode::Off) set_renderer_pack(true); // reloads it
    }
    generation++;
}
