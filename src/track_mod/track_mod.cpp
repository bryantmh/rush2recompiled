// Custom tracks from mods at run time (include/track_mod.h, docs/custom_tracks.md): finds the enabled mods' tracks,
// converts the raced one (src/track_mod/track_mod_convert.cpp) through the disk cache, and remembers which one is raced.
// The race itself is hosted like a Rush 2049 track, in the HAWAII slot (src/rush2049/track2049.cpp).
//
// A track is the folder tracks/<folder>/ (or the older rush2/tracks/<folder>/) of an enabled mod (.rtz zip or folder in mods/), holding track.glb and
// track.toml. When two mods hold the same folder name, the first in the mod order wins. Each conversion writes the
// converter's report to <config>/track_reports/<folder>.txt (and the console), which is what the track's author reads.
//
// Textures: the converted geometry holds small true-color stand-ins (the game's memory); on screen each is replaced by
// its full-size image from the glb, the way car model mods' are (rush2::derive::set_mod_textures, source "tracks"). The
// stand-ins' keys are cached with the track; the images are decoded only when a stand-in is first drawn.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <set>
#include <thread>

#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "recompui/recompui.h"

#include "gltf_scene.h"
#include "texture_derive.h"
#include "texture_upscale.h"
#include "track2049.h"
#include "track_cache.h"
#include "track_mod.h"
#include "track_mod_runtime.h"

namespace {
    // Bump when the converter's output changes for the same input (the build stamp already covers new builds).
    constexpr uint64_t converter_version = 2;
    constexpr int cache_k = 1000;   // track_cache keys of mod tracks: 1000 + a hash of the folder (2049 tracks use 1-37)

    std::mutex mutex;
    std::vector<rush2::track_mod::Entry> entries;
    std::string entries_signature;
    std::atomic_int raced = -1;

    // The converter's last report on each track (by mod id + "/" + folder), for the Mods tab (register_mod_notes).
    struct KnownReport {
        rush2::track_mod::Report report;
        bool loads = false;
    };
    std::mutex reports_mutex;
    std::map<std::string, KnownReport> reports;
    std::set<std::string> checking;     // tracks a background check is converting
    std::mutex convert_mutex;           // one conversion at a time, so a track converted twice at once isn't cached twice

    std::string report_key(const rush2::track_mod::Entry& e) {
        return e.mod_id + "/" + e.folder;
    }

    void remember_report(const rush2::track_mod::Entry& e, const rush2::track_mod::Report& report, bool loads) {
        std::lock_guard lock{ reports_mutex };
        reports[report_key(e)] = { report, loads };
    }

    // A report as lines of level letter, tab, text.
    std::vector<uint8_t> serialize_report(const rush2::track_mod::Report& report) {
        std::string s;
        for (const rush2::track_mod::Message& m : report.messages) {
            s += m.level == rush2::track_mod::Message::error ? 'E' : m.level == rush2::track_mod::Message::warning ? 'W' : 'I';
            s += '\t';
            for (char c : m.text) s += c == '\n' ? ' ' : c;
            s += '\n';
        }
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    rush2::track_mod::Report deserialize_report(const std::vector<uint8_t>& blob) {
        rush2::track_mod::Report report;
        std::string s(blob.begin(), blob.end());
        size_t at = 0;
        while (at + 2 <= s.size()) {
            size_t end = s.find('\n', at);
            if (end == std::string::npos) end = s.size();
            std::string text = s.substr(at + 2, end - at - 2);
            if (s[at] == 'E') report.error(text);
            else if (s[at] == 'W') report.warning(text);
            else report.info(text);
            at = end + 1;
        }
        return report;
    }

    bool read_file(const rush2::track_mod::Entry& e, const std::string& name, std::vector<uint8_t>& out) {
        std::string path = e.dir + name;
        rush2::upscale::ZipReader zip(e.mod_path);
        if (zip.is_open()) return zip.read(path, out);
        out = rush2::upscale::read_file(e.mod_path / std::filesystem::u8path(path));
        return !out.empty();
    }

    void scan() {
        std::string signature;
        std::vector<std::pair<std::string, std::filesystem::path>> mods;
        for (const recomp::mods::ModDetails& mod : recomp::mods::get_all_mod_details("rush2")) {
            bool on = recomp::mods::is_mod_enabled(mod.mod_id);
            signature += mod.mod_id + (on ? "+" : "-") + ";";
            if (on) mods.push_back({ mod.mod_id, recomp::mods::get_mod_filename(mod.mod_id) });
        }
        if (signature == entries_signature) return;
        entries_signature = signature;
        entries.clear();
        std::set<std::string> seen;
        for (const auto& [id, path] : mods) {
            // (folder, its path in the mod): tracks/<folder>/ first, then the older rush2/tracks/<folder>/.
            std::vector<std::pair<std::string, std::string>> folders;
            rush2::upscale::ZipReader zip(path);
            for (const std::string head : { "tracks/", "rush2/tracks/" }) {
                std::vector<std::pair<std::string, std::string>> found;
                if (zip.is_open()) {
                    for (const std::string& n : zip.names()) {
                        if (n.compare(0, head.size(), head) != 0) continue;
                        size_t slash = n.find('/', head.size());
                        if (slash == std::string::npos || n.substr(slash + 1) != "track.glb") continue;
                        std::string f = n.substr(head.size(), slash - head.size());
                        found.push_back({ f, head + f + "/" });
                    }
                }
                else {
                    std::error_code ec;
                    for (const auto& d : std::filesystem::directory_iterator(path / std::filesystem::u8path(head), ec)) {
                        if (d.is_directory(ec) && std::filesystem::exists(d.path() / "track.glb", ec)) {
                            auto u = d.path().filename().u8string();
                            std::string f(u.begin(), u.end());
                            found.push_back({ f, head + f + "/" });
                        }
                    }
                }
                std::sort(found.begin(), found.end());
                folders.insert(folders.end(), found.begin(), found.end());
            }
            for (const auto& [f, dir] : folders) {
                if (!seen.insert(f).second) continue;
                rush2::track_mod::Entry e;
                e.mod_id = id;
                e.mod_path = path;
                e.folder = f;
                e.dir = dir;
                std::vector<uint8_t> toml;
                read_file(e, "track.toml", toml);
                rush2::track_mod::Report report;
                rush2::track_mod::parse_settings(std::string(toml.begin(), toml.end()), e.settings, report);
                if (e.settings.name.empty()) e.settings.name = f;
                entries.push_back(std::move(e));
            }
        }
        printf("[trackmod] %zu custom track(s) in the enabled mods\n", entries.size());
    }

    void write_report(const std::string& folder, const rush2::track_mod::Report& report) {
        std::error_code ec;
        std::filesystem::path dir = recomp::get_config_path() / "track_reports";
        std::filesystem::create_directories(dir, ec);
        std::string text = report.text();
        std::vector<uint8_t> bytes(text.begin(), text.end());
        rush2::upscale::write_file(dir / std::filesystem::u8path(folder + ".txt"), bytes);
        printf("[trackmod] %s:\n%s", folder.c_str(), text.c_str());
    }
}

std::vector<rush2::track_mod::Entry> rush2::track_mod::tracks() {
    std::lock_guard lock{ mutex };
    scan();
    return entries;
}

int rush2::track_mod::race_track() {
    int r = raced;
    if (r < 0) {
        // Developer shortcut: race custom track n in place of HAWAII (no track select entry needed).
        static const char* test = std::getenv("RUSH2_TEST_MOD_TRACK");
        if (test != nullptr) r = std::atoi(test);
    }
    return r;
}

void rush2::track_mod::set_race_track(int index) {
    raced = index;
}

std::string rush2::track_mod::race_folder() {
    std::lock_guard lock{ mutex };
    int r = race_track();
    if (r < 0) return "";
    scan();
    return r < (int)entries.size() ? entries[r].folder : "";
}

bool rush2::track_mod::race_settings(Settings& out) {
    std::lock_guard lock{ mutex };
    int r = race_track();
    if (r < 0) return false;
    scan();
    if (r >= (int)entries.size()) return false;
    out = entries[r].settings;
    return true;
}

namespace {
    // Mods race in the host slot, HAWAII (src/rush2049/track2049.cpp prefix_of(host_slot)).
    const std::string host_prefix = "HAWAII";

    struct Files {
        rush2::track_mod::Entry entry;
        std::vector<uint8_t> glb, toml;
        rush2::track_mod::Extras extras;
        uint64_t source = 0;        // hash of every file the conversion reads, and the converter's version
        int cache_key = 0;          // the track's own entry in the track cache
    };

    bool read_files(int index, Files& f) {
        {
            std::lock_guard lock{ mutex };
            scan();
            if (index < 0 || index >= (int)entries.size()) return false;
            f.entry = entries[index];
        }
        const rush2::track_mod::Entry& e = f.entry;
        if (!read_file(e, "track.glb", f.glb)) {
            printf("[trackmod] %s: can't read track.glb\n", e.folder.c_str());
            return false;
        }
        read_file(e, "track.toml", f.toml);
        if (!e.settings.logo.empty()) read_file(e, e.settings.logo, f.extras.logo);
        if (!e.settings.diorama.empty()) read_file(e, e.settings.diorama, f.extras.diorama);
        if (!e.settings.preview.empty()) read_file(e, e.settings.preview, f.extras.preview);
        uint64_t h = rush2::track_cache::hash(f.glb.data(), f.glb.size(), converter_version);
        h = rush2::track_cache::hash(f.toml.data(), f.toml.size(), h);
        h = rush2::track_cache::hash(f.extras.logo.data(), f.extras.logo.size(), h);
        h = rush2::track_cache::hash(f.extras.preview.data(), f.extras.preview.size(), h);
        f.source = rush2::track_cache::hash(f.extras.diorama.data(), f.extras.diorama.size(), h);
        uint64_t name = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(e.folder.data()), e.folder.size());
        f.cache_key = cache_k + int(name % 1000000);
        return true;
    }

    std::string menu_blob_name(const Files& f) {
        return "trackmod_menu_" + std::to_string(f.cache_key);
    }

    std::string texture_blob_name(const Files& f) {
        return "trackmod_textures_" + std::to_string(f.cache_key);
    }

    std::string report_blob_name(const Files& f) {
        return "trackmod_report_" + std::to_string(f.cache_key);
    }

    // The cached report of a track the cache holds (converted with these files), for the Mods tab.
    void cached_report(const Files& f) {
        std::vector<uint8_t> blob;
        if (rush2::track_cache::load_blob(report_blob_name(f), f.source, blob)) remember_report(f.entry, deserialize_report(blob), true);
    }

    // The stand-ins without their texels: source, image, tint, cutoff and key each.
    std::vector<uint8_t> serialize_textures(const std::vector<rush2::track_mod::Texture>& textures) {
        std::vector<uint8_t> out;
        auto add = [&](const void* p, size_t n) { out.insert(out.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
        uint32_t n = uint32_t(textures.size());
        add(&n, 4);
        for (const rush2::track_mod::Texture& t : textures) {
            add(&t.source, 4);
            add(&t.image, 4);
            add(t.tint.data(), 16);
            add(&t.cutoff, 4);
            add(&t.key, 8);
        }
        return out;
    }

    bool deserialize_textures(const std::vector<uint8_t>& in, std::vector<rush2::track_mod::Texture>& textures) {
        size_t o = 0;
        auto get = [&](void* p, size_t n) {
            if (o + n > in.size()) return false;
            memcpy(p, in.data() + o, n);
            o += n;
            return true;
        };
        uint32_t n = 0;
        if (!get(&n, 4) || size_t(n) * 36 != in.size() - 4) return false;
        textures.assign(n, {});
        for (rush2::track_mod::Texture& t : textures) {
            get(&t.source, 4);
            get(&t.image, 4);
            get(t.tint.data(), 16);
            get(&t.cutoff, 4);
            get(&t.key, 8);
        }
        return true;
    }

    // A glb the full-size images are decoded from, read on the first draw of one of its stand-ins and kept.
    struct ImageSource {
        rush2::track_mod::Entry entry;
        std::string file;
        std::mutex mutex;
        bool tried = false;
        std::vector<rush2::gltf::Image> images;
    };

    // Every track's stand-ins by folder (from menu_art and load), published as one set.
    std::map<std::string, std::pair<rush2::track_mod::Entry, std::vector<rush2::track_mod::Texture>>> known_textures;
    std::string published_signature;

    void remember_textures(const rush2::track_mod::Entry& e, std::vector<rush2::track_mod::Texture> textures) {
        std::lock_guard lock{ mutex };
        known_textures[e.folder] = { e, std::move(textures) };
    }

    // Converts the track and stores the race files and the menu art in the cache.
    bool convert_and_store(const Files& f, const std::string& prefix, rush2::track_mod::Converted& out) {
        rush2::track_mod::Report report;
        bool ok = rush2::track_mod::convert(f.glb, std::string(f.toml.begin(), f.toml.end()), prefix, out, report, f.extras);
        if (ok) rush2::track_mod::make_menu_art(out, f.extras, out.menu, report);
        write_report(f.entry.folder, report);
        remember_report(f.entry, report, ok);
        if (!ok) return false;
        rush2::track_cache::save_blob(report_blob_name(f), f.source, serialize_report(report));
        rush2::track_cache::save_2049(f.cache_key, prefix, f.source, out.track);
        rush2::track_cache::save_blob(menu_blob_name(f), f.source, rush2::track_mod::serialize(out.menu));
        rush2::track_cache::save_blob(texture_blob_name(f), f.source, serialize_textures(out.textures));
        remember_textures(f.entry, out.textures);
        return true;
    }

    // The stand-ins' keys from the cache (false when the track wasn't converted with these files).
    bool cached_textures(const Files& f) {
        std::vector<uint8_t> blob;
        std::vector<rush2::track_mod::Texture> textures;
        if (!rush2::track_cache::load_blob(texture_blob_name(f), f.source, blob) || !deserialize_textures(blob, textures)) {
            return false;
        }
        remember_textures(f.entry, std::move(textures));
        return true;
    }
}

void rush2::track_mod::publish_textures() {
    std::unordered_map<uint64_t, rush2::derive::ModTexture> textures;
    std::string signature;
    {
        std::lock_guard lock{ mutex };
        scan();
        std::set<std::string> enabled;
        for (const Entry& e : entries) enabled.insert(e.folder);
        std::map<std::pair<std::string, uint32_t>, std::shared_ptr<ImageSource>> sources;
        for (const auto& [folder, known] : known_textures) {
            if (!enabled.count(folder)) continue;
            const Entry& entry = known.first;
            for (const Texture& t : known.second) {
                if (t.image < 0 || t.key == 0) continue;    // no image: a color, the stand-in is exact
                std::shared_ptr<ImageSource>& src = sources[{ folder, t.source }];
                if (!src) {
                    src = std::make_shared<ImageSource>();
                    src->entry = entry;
                    src->file = t.source == Texture::diorama ? entry.settings.diorama : "track.glb";
                }
                std::shared_ptr<ImageSource> from = src;
                int image = t.image;
                std::array<float, 4> tint = t.tint;
                float cutoff = t.cutoff;
                rush2::derive::ModTexture mt;
                uint64_t seed = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(folder.data()), folder.size(),
                                                         0x747261636Bull);
                mt.id = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(&t.key), 8, seed ^ t.source);
                mt.load = [from, image, tint, cutoff](rush2::upscale::Image& out, rush2::upscale::Image&) {
                    {
                        std::lock_guard lock{ from->mutex };
                        if (!from->tried) {
                            from->tried = true;
                            std::vector<uint8_t> glb;
                            rush2::gltf::Scene scene;
                            std::string error;
                            if (read_file(from->entry, from->file, glb) && rush2::gltf::read_glb(glb, scene, error)) {
                                from->images = std::move(scene.images);
                            }
                            else {
                                printf("[trackmod] %s: can't read %s for its textures %s\n", from->entry.folder.c_str(),
                                       from->file.c_str(), error.c_str());
                            }
                        }
                        if (image >= (int)from->images.size()) return false;
                        if (!rush2::upscale::decode_image(from->images[image].data, out) || out.empty()) return false;
                    }
                    // As make_texture made the stand-in: tinted by the material's color, alpha cut out at cutoff.
                    for (size_t i = 0; i + 3 < out.rgba.size(); i += 4) {
                        for (int k = 0; k < 3; k++) out.rgba[i + k] = uint8_t(std::clamp(out.rgba[i + k] * tint[k], 0.0f, 255.0f));
                        out.rgba[i + 3] = out.rgba[i + 3] * tint[3] >= cutoff * 255.0f ? 255 : 0;
                    }
                    return true;
                };
                textures[t.key] = mt;
                signature += rush2::upscale::key_name(t.key) + rush2::upscale::key_name(mt.id);
            }
        }
        if (signature == published_signature) return;
        published_signature = signature;
    }
    printf("[trackmod] %zu custom track textures drawn full size\n", textures.size());
    rush2::derive::set_mod_textures("tracks", std::move(textures));
}

bool rush2::track_mod::load(int index, const std::string& prefix, Converted& out) {
    Files f;
    if (!read_files(index, f)) return false;
    std::lock_guard converting{ convert_mutex };
    out = Converted{};
    Report settings_report;
    parse_settings(std::string(f.toml.begin(), f.toml.end()), out.settings, settings_report);
    if (rush2::track_cache::load_2049(f.cache_key, prefix, f.source, out.track) && cached_textures(f)) {
        for (const auto& [name, sf] : out.settings.surfaces) out.custom_drag[sf.material] = sf.drag;
        publish_textures();
        return true;
    }
    bool ok = convert_and_store(f, prefix, out);
    if (ok) publish_textures();
    return ok;
}

bool rush2::track_mod::menu_art(int index, MenuArt& out) {
    Files f;
    if (!read_files(index, f)) return false;
    std::lock_guard converting{ convert_mutex };
    std::vector<uint8_t> blob;
    if (rush2::track_cache::load_blob(menu_blob_name(f), f.source, blob) && deserialize(blob, out) && cached_textures(f)) {
        cached_report(f);
        return true;
    }
    Converted converted;
    if (!convert_and_store(f, host_prefix, converted)) return false;
    out = std::move(converted.menu);
    return true;
}

void rush2::track_mod::register_mod_notes() {
    // A mod's tracks and what the converter found wrong with each. A track not checked yet is converted (or read
    // from the cache) on a background thread; the panel asks again each frame until it is done.
    recompui::set_mod_notes_provider([](const std::string& mod_id) {
        recompui::ModNotes notes;
        std::vector<Entry> list = tracks();
        for (size_t i = 0; i < list.size(); i++) {
            const Entry& e = list[i];
            if (e.mod_id != mod_id) continue;
            std::string key = report_key(e);
            std::string title = e.settings.name + " (tracks/" + e.folder + ")";
            std::lock_guard lock{ reports_mutex };
            auto it = reports.find(key);
            if (it == reports.end()) {
                notes.pending = true;
                notes.sections.push_back({ title + ": checking...", {} });
                if (checking.insert(key).second) {
                    std::thread([i, key]() {
                        MenuArt art;
                        bool ok = menu_art(int(i), art);
                        std::lock_guard lock{ reports_mutex };
                        checking.erase(key);
                        if (!reports.count(key)) reports[key] = { {}, ok };   // cached before reports were kept, or unreadable
                    }).detach();
                }
                continue;
            }
            recompui::ModNoteSection section;
            size_t problems = 0;
            for (const Message& m : it->second.report.messages) {
                if (m.level == Message::info) continue;
                section.notes.push_back({ m.level == Message::error ? recompui::ModNote::Level::Error
                                                                    : recompui::ModNote::Level::Warning, m.text });
                problems++;
            }
            if (!it->second.loads) section.title = title + ": can't be raced";
            else if (problems == 0) section.title = title + ": no problems found";
            else section.title = title + ": loads, " + std::to_string(problems) + (problems == 1 ? " problem" : " problems");
            if (!it->second.loads && section.notes.empty()) {
                section.notes.push_back({ recompui::ModNote::Level::Error, "track.glb couldn't be read." });
            }
            notes.sections.push_back(std::move(section));
        }
        return notes;
    });
}
