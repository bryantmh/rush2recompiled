// Texture census, a test aid (include/texture_census.h): which drawn textures the catalog can't name, and where in the
// ROM data their texels come from.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "asset_catalog.h"
#include "texture_census.h"
#include "texture_derive.h"
#include "texture_origin.h"

namespace rush2::census {

namespace {
    struct Drawn {
        uint64_t hash, key, index_key;
        uint32_t address;
        uint8_t fmt, siz;
        uint16_t line, tmem, width, height;
        uint32_t tlut;
        std::vector<uint8_t> bytes; // The RDRAM bytes at address when first drawn.
        rush2::upscale::Image image;
    };

    const char* const path = getenv("RUSH2_TEXTURE_CENSUS");
    const uint8_t* rdram_base = nullptr;
    std::mutex mutex;
    std::vector<Drawn> drawn;
    std::set<uint64_t> hashes;
    constexpr uint32_t sample = 32;
}

bool enabled() {
    return path != nullptr;
}

void set_rdram(const uint8_t* rdram) {
    rdram_base = rdram;
}

void record(uint64_t hash, uint64_t key, uint64_t index_key, uint32_t address, uint8_t fmt, uint8_t siz, uint16_t line,
            uint16_t tmem, uint16_t width, uint16_t height, uint32_t tlut, const rush2::upscale::Image& image) {
    std::unique_lock lock(mutex);
    if (!hashes.insert(hash).second) {
        return;
    }
    Drawn d{ hash, key, index_key, address, fmt, siz, line, tmem, width, height, tlut };
    if (rdram_base != nullptr) {
        uint32_t at = address & 0x7FFFFF; // 8 MB with the Expansion Pak; RDRAM is stored word-swapped.
        // The texels as stored: rows of the texture's own width (the tile's line can be wider).
        uint32_t size = std::min<uint32_t>(std::max<uint32_t>(((uint32_t(width) << siz) >> 1) * height, sample), 4096);
        for (uint32_t i = 0; i < size && at + i < 0x800000; i++) {
            d.bytes.push_back(rdram_base[(at + i) ^ 3]);
        }
    }
    d.image = image;
    drawn.push_back(std::move(d));
}

void write() {
    std::vector<Drawn> list;
    {
        std::unique_lock lock(mutex);
        list = drawn;
    }
    std::vector<rush2::catalog::Entry> containers = rush2::catalog::containers([](const std::string&) {});
    std::set<uint64_t> keys, index_keys, catalog_hashes;
    // Container -> texel offset -> texture names there.
    std::map<std::string, std::multimap<uint32_t, std::string>> at_offset;
    for (const rush2::catalog::Entry& e : containers) {
        rush2::catalog::Container c;
        std::string error;
        if (!rush2::catalog::read_container(*e.data, c, error, false, false, &e.copies)) continue;
        for (const rush2::catalog::Texture& t : c.textures) {
            keys.insert(t.key);
            if (t.index_key) index_keys.insert(t.index_key);
            if (t.hash) catalog_hashes.insert(t.hash);
            at_offset[e.path].emplace(t.texels, t.name);
        }
    }
    std::filesystem::path out = std::filesystem::u8path(path);
    std::filesystem::path images = out;
    images += "_images";
    std::error_code ec;
    std::filesystem::create_directories(images, ec);
    FILE* f = fopen(path, "w");
    if (f == nullptr) return;
    size_t named = 0, variants = 0;
    // What the derived replacements made of each (with a texture pack enabled): every texture drawn should end up
    // replaced by a pack or derived from one.
    const char* outcome_names[] = { "not looked at", "pack", "derived", "no upscale of its base", "not traced",
                                    "interface (left alone)", "too small or flat" };
    size_t outcomes[7] = {};
    for (const Drawn& d : list) {
        int o = int(rush2::derive::outcome(d.hash));
        outcomes[o]++;
        bool replaced = o == int(rush2::derive::Outcome::Pack) || o == int(rush2::derive::Outcome::Derived) ||
                        o == int(rush2::derive::Outcome::Interface) || o == int(rush2::derive::Outcome::Small);
        if (keys.count(d.key) || catalog_hashes.count(d.hash)) {
            named++;
            if (replaced || o == int(rush2::derive::Outcome::Unknown)) continue;
        }
        else if (d.index_key && index_keys.count(d.index_key)) {
            variants++;
            if (replaced || o == int(rush2::derive::Outcome::Unknown)) continue;
        }
        std::string where;
        bool uniform = !d.bytes.empty() && std::all_of(d.bytes.begin(), d.bytes.begin() + std::min<size_t>(sample, d.bytes.size()),
                                                         [&](uint8_t b) { return b == d.bytes[0]; });
        if (d.bytes.size() >= sample && !uniform) {
            for (const rush2::catalog::Entry& e : containers) {
                const std::vector<uint8_t>& data = *e.data;
                // The first bytes, else ones from the middle (when the game rewrote the start).
                uint32_t off = 0;
                bool found = false;
                for (size_t from : { size_t(0), (d.bytes.size() / 2) & ~size_t(7) }) {
                    if (from + sample > d.bytes.size()) continue;
                    auto it = std::search(data.begin(), data.end(), std::boyer_moore_horspool_searcher(
                        d.bytes.begin() + from, d.bytes.begin() + from + sample));
                    if (it != data.end() && size_t(it - data.begin()) >= from) {
                        off = uint32_t(it - data.begin() - from);
                        found = true;
                        break;
                    }
                }
                if (!found) continue;
                // How much of the whole loaded block differs from the file there (the game rewrote texels).
                size_t diff = 0, first = SIZE_MAX;
                for (size_t i = 0; i < d.bytes.size(); i++) {
                    if (off + i >= data.size() || data[off + i] != d.bytes[i]) {
                        diff++;
                        if (first == SIZE_MAX) first = i;
                    }
                }
                char buf[128];
                if (diff) snprintf(buf, sizeof(buf), "%s+%X (%zu of %zu bytes differ from +%zX)", e.path.c_str(), off, diff,
                                   d.bytes.size(), first);
                else snprintf(buf, sizeof(buf), "%s+%X (all %zu bytes)", e.path.c_str(), off, d.bytes.size());
                if (!where.empty()) where += " ";
                where += buf;
                auto& names = at_offset[e.path];
                auto range = names.equal_range(off);
                for (auto n = range.first; n != range.second; ++n) where += "=" + n->second;
                if (range.first == range.second) {
                    // The nearest texture starting before it (texels inside a bigger image).
                    auto below = names.upper_bound(off);
                    if (below != names.begin()) {
                        --below;
                        snprintf(buf, sizeof(buf), "(in %s +%X)", below->second.c_str(), off - below->first);
                        where += buf;
                    }
                }
                if (where.size() > 300) break;
            }
            if (where.empty()) where = "not in any container";
        }
        else {
            where = uniform ? "uniform bytes" : "no bytes";
        }
        std::string kn = rush2::upscale::key_name(d.key);
        fprintf(f, "%s hash %016llx idx %016llx addr %08X %s fmt %u siz %u %ux%u line %u tmem %u tlut %X [%s] | %s\n",
                kn.c_str(), (unsigned long long)d.hash, (unsigned long long)d.index_key, d.address,
                rush2::origin::folder(rush2::origin::at(d.address)), d.fmt, d.siz, d.width, d.height, d.line, d.tmem,
                d.tlut, outcome_names[o], where.c_str());
        rush2::upscale::write_png(images / (kn + ".png"), d.image);
    }
    fprintf(f, "# drawn %zu, named %zu, palette variants %zu\n", list.size(), named, variants);
    fprintf(f, "# replacements: pack %zu, derived %zu, no upscale of its base %zu, not traced %zu, interface %zu, "
            "too small or flat %zu, not looked at %zu\n", outcomes[1], outcomes[2], outcomes[3], outcomes[4], outcomes[5],
            outcomes[6], outcomes[0]);
    fclose(f);
    printf("[census] drawn %zu, named %zu, palette variants %zu, written to %s\n", list.size(), named, variants, path);
    fflush(stdout);
}

}
