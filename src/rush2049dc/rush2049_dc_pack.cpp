// The texture pack of a Dreamcast Rush 2049 disc's full-size textures. See include/rush2049_dc.h and
// docs/rush2049_research/dreamcast.md (Full-size textures).
//
// The files made from the disc load each texture scaled down to fit TMEM (src/rush2049_dc_model.cpp). This pack
// replaces every one of them, by the RT64 hash it is drawn with (replacement_hash, worked out from the converter's own
// load list), with the disc's image: RT64 loads it and streams its textures like any installed texture pack, and any
// pack the player enables wins over it. Painted car textures, whose palette the game sets per paint job, are left to
// src/texture_upscale.cpp.
//
// Built once per disc and build, in the background, into <app folder>/track_cache/dc_textures: rt64.json, one DDS
// (with mipmaps) per distinct image, "hashes" (the replaced hashes, u64 each, for in_texture_pack) and the stamp file
// (source key, build) written last, so a build that was cut short is done again. Every file is converted first (and
// so cached for the game), a few seconds on the first boot.

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>

#include "recompui/renderer.h"

#include "rush2049_dc.h"
#include "texture_upscale.h"
#include "track_cache.h"

namespace {
    std::atomic<uint64_t> generation = 0; // use_texture_pack calls; a build for an older call stops
    std::mutex build_mutex;                // one build at a time
    std::mutex hashes_mutex;
    std::unordered_set<uint64_t> pack_hashes; // of the pack in use

    std::filesystem::path pack_dir() {
        return rush2::track_cache::directory() / "dc_textures";
    }

    std::string stamp_of(const rush2::rom2049::Source& source) {
        char text[64];
        snprintf(text, sizeof(text), "%016" PRIx64 " %016" PRIx64, source.cache_key(), rush2::track_cache::build());
        return text;
    }

    bool up_to_date(const std::string& stamp) {
        std::ifstream in(pack_dir() / "stamp", std::ios::binary);
        std::string have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        std::error_code ec;
        return have == stamp && std::filesystem::exists(pack_dir() / "rt64.json", ec);
    }

    std::unordered_set<uint64_t> read_hashes() {
        std::vector<uint8_t> bytes = rush2::upscale::read_file(pack_dir() / "hashes");
        std::unordered_set<uint64_t> out;
        for (size_t i = 0; i + 8 <= bytes.size(); i += 8) {
            uint64_t h;
            memcpy(&h, &bytes[i], 8);
            out.insert(h);
        }
        return out;
    }

    bool build(const rush2::rom2049::Source& source, const std::string& stamp, uint64_t gen) {
        namespace upscale = rush2::upscale;
        std::vector<uint8_t> scratch;
        for (int i = 0; i < rush2::rom2049::file_count; i++) {
            if (generation != gen) return false;
            source.read_file(i, scratch);
        }
        source.source_texture_count(); // Reads the textures of files cached before this session too.
        std::vector<rush2::rom2049::SourceTexture> textures = source.source_textures(0);

        std::filesystem::path dir = pack_dir();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        std::vector<upscale::PackTexture> pack;
        std::map<uint64_t, size_t> by_content; // content key -> its entry in pack
        std::set<uint64_t> hashes;
        for (const rush2::rom2049::SourceTexture& t : textures) {
            if (generation != gen) return false;
            uint64_t hash = rush2::rom2049::dc::replacement_hash(t);
            if (hash == 0 || !hashes.insert(hash).second) continue;
            upscale::Image image;
            int w = 0, h = 0;
            if (!source.source_image(t, image.rgba, w, h) || w <= 0 || h <= 0) continue;
            image.width = (uint32_t)w;
            image.height = (uint32_t)h;
            uint64_t key = upscale::content_key(image);
            auto [it, added] = by_content.try_emplace(key, pack.size());
            if (added) {
                std::string name = upscale::key_name(key) + ".dds";
                if (!upscale::write_file(dir / name, upscale::make_dds(image))) return false;
                pack.push_back({ name, {} });
            }
            pack[it->second].hashes.push_back(hash);
        }
        std::string database = upscale::pack_database(pack);
        std::vector<uint8_t> hash_bytes(hashes.size() * 8);
        size_t at = 0;
        for (uint64_t h : hashes) {
            memcpy(&hash_bytes[at], &h, 8);
            at += 8;
        }
        if (!upscale::write_file(dir / "rt64.json", std::vector<uint8_t>(database.begin(), database.end())) ||
            !upscale::write_file(dir / "hashes", hash_bytes) ||
            !upscale::write_file(dir / "stamp", std::vector<uint8_t>(stamp.begin(), stamp.end()))) {
            return false;
        }
        printf("[dc] texture pack: %zu full-size images for %zu textures\n", pack.size(), hashes.size());
        return true;
    }
}

namespace {
    bool pack_set = false; // a pack path was given to the renderer (hashes_mutex held)

    // Takes the pack off the renderer, if it has one, before its folder is rebuilt or another source is used.
    void drop_pack() {
        std::lock_guard lock{ hashes_mutex };
        pack_hashes.clear();
        if (pack_set) {
            pack_set = false;
            recompui::renderer::set_generated_texture_pack({});
        }
    }
}

void rush2::rom2049::dc::use_texture_pack(std::shared_ptr<const Source> source) {
    uint64_t gen = ++generation;
    if (source == nullptr || !source->is_dreamcast() || source->cache_key() == 0) {
        drop_pack();
        return;
    }
    std::thread([source, gen] {
        std::lock_guard lock{ build_mutex };
        std::string stamp = stamp_of(*source);
        bool ready = up_to_date(stamp);
        if (!ready) {
            drop_pack();
            ready = build(*source, stamp, gen);
        }
        if (ready && generation == gen) {
            std::unordered_set<uint64_t> hashes = read_hashes();
            std::lock_guard lock{ hashes_mutex };
            pack_hashes = std::move(hashes);
            if (!pack_set) {
                pack_set = true;
                recompui::renderer::set_generated_texture_pack(pack_dir());
            }
        }
    }).detach();
}

bool rush2::rom2049::dc::in_texture_pack(uint64_t hash) {
    std::lock_guard lock{ hashes_mutex };
    return pack_hashes.count(hash) != 0;
}
