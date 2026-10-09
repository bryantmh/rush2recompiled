// Texture upscaling (Settings > Graphics > Texture Upscaling). See docs/texture_upscaling.md.
//
// RT64 shows the recomp every texture a draw call samples (RT64::TextureObserver, added in lib/patches/rt64.patch).
// Textures drawn by 3D geometry with a perspective projection on a few separate frames are decoded from TMEM and
// kept; any texture also drawn in 2D (HUD, menus, fonts: rectangles and orthographic draws) is left alone, and if
// it was upscaled already its replacement is taken off again. Textures are identified by their decoded pixels
// (content_key), so the same image loaded under different RT64 hashes is upscaled and stored once.
//
// Paletted textures that differ only in their palette (car paint: every paint job is a palette) share an index key.
// Only the first one seen is upscaled; every other variant is made from that upscale by recolor(), on the CPU in
// about a millisecond, instead of running the upscaler again for each of the thousands of paint combinations.
//
// A background thread does the work in batches and caches each result as a DDS with mipmaps in
// <app folder>/texture_upscale/cache/<mode>/<key>.dds, so nothing is upscaled twice, across sessions too. Results
// reach the renderer as RT64 live replacements (rt64_live_textures.h): no pack reload, and any installed texture pack
// that replaces the same texture wins. HQ2x and HQ4x run hqx (lib/hqx) on the CPU. Custom runs any command line on a
// folder of images.
//
// A Dreamcast Rush 2049 source scales its textures down to fit TMEM. Their full-size images come from the texture
// pack it builds (src/rush2049_dc_pack.cpp), which RT64 loads like any pack, so they are never upscaled here
// (set_texture_source).
//
// For upscalers without a command line (Topaz Gigapixel's app, say): Dump Textures writes the kept textures as
// <key>.png into texture_upscale/dump, the user saves upscaled copies (any size, file names starting with the key)
// into texture_upscale/upscaled, and Install Upscaled turns those into the texture pack mod "rush2_custom_textures"
// (a folder in the mods folder, enabled right away) plus a shareable .rtz of it.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#include "common/rt64_live_textures.h"
#include "librecomp/config.hpp"
#include "librecomp/mods.hpp"
#include "recompui/config.h"
#include "recompui/recompui.h"
#include "recompui/renderer.h"
#include "util/file.h"

#include "options_page.h"
#include "rush2049_dc.h"
#include "texture_upscale.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace rush2::upscale {

const char* const mode_option_id = "texture_upscaling";
const char* const command_option_id = "texture_upscale_command";

namespace {
    // Saved by name ("Hq2x"...), so the values only matter inside this file.
    enum class Mode : uint32_t { Off, Hq2x, Hq4x, Custom };

    constexpr uint32_t stable_frames = 3;  // Frames a texture is drawn in 3D before it's kept (skips one-frame textures).
    constexpr uint32_t border = 8;         // Texels of padding around images given to an upscaler.
    constexpr size_t batch_size = 16;      // Images per batch (one custom upscaler run).
    constexpr auto run_pause = std::chrono::milliseconds(250); // Between custom upscaler runs, to give the game room.
    const std::string pack_id = "rush2_custom_textures";

    std::string quoted(const std::filesystem::path& path) {
        return "\"" + path_utf8(path) + "\"";
    }

    std::filesystem::path root() { return recompui::file::get_app_folder_path() / "texture_upscale"; }
    std::filesystem::path dump_dir() { return root() / "dump"; }
    std::filesystem::path upscaled_dir() { return root() / "upscaled"; }
    std::filesystem::path work_dir() { return root() / "work"; }
    std::filesystem::path ui_list() { return root() / "ui_textures.txt"; }

    uint32_t mode_scale(Mode mode) {
        return mode == Mode::Hq4x ? 4 : 2;
    }

    const char* mode_folder(Mode mode) {
        switch (mode) {
            case Mode::Hq2x: return "hq2x";
            case Mode::Hq4x: return "hq4x";
            case Mode::Custom: return "custom";
            default: return "";
        }
    }

    Edge edge_of(uint8_t cm, uint8_t mask) {
        if ((cm & 2) || mask == 0) {
            return Edge::Clamp; // G_TX_CLAMP, or no mask (which clamps).
        }
        return (cm & 1) ? Edge::Mirror : Edge::Wrap;
    }

    void open_folder(const std::filesystem::path& folder) {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
#ifdef _WIN32
        ShellExecuteW(nullptr, L"open", folder.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
        std::thread([folder]() { run_command("open " + quoted(folder)); }).detach();
#else
        std::thread([folder]() { run_command("xdg-open " + quoted(folder)); }).detach();
#endif
    }

    // A texture kept for upscaling: its decoded pixels and the RT64 hashes it's drawn under.
    struct Entry {
        Image image;
        Edge edge_s = Edge::Wrap;
        Edge edge_t = Edge::Wrap;
        std::vector<uint64_t> hashes;
        uint64_t base = 0;          // Content key of the palette variant this one is recolored from, or 0.
        std::vector<uint64_t> dependents; // Variants waiting for this one's upscale.
        bool ui = false;            // Also drawn in 2D: never replaced.
        bool queued = false;        // Waiting for the worker in the current mode.
        bool ready = false;         // Upscaled in the current mode; its hashes get the replacement as they show up.
        bool failed = false;        // Nothing usable came out for it in the current mode.
        uint64_t index_key = 0;     // A paletted texture's index_key, else 0.
    };

    // What the observer knows of an RT64 hash.
    struct Seen {
        uint64_t key = 0;
        uint64_t last_frame = UINT64_MAX;
        uint32_t frames = 0;
        bool ui = false;
        bool kept = false;          // Decoded already (key is 0 when it wasn't worth keeping).
    };

    // A piece of work the worker took from the queue, copied out so it runs without the lock.
    struct Job {
        uint64_t key;
        Entry entry;
        uint64_t base_key = 0;      // With base_image: recolor from that upscale instead of upscaling.
        Image base_image;
        Edge base_edge_s = Edge::Wrap;
        Edge base_edge_t = Edge::Wrap;
    };

    class Upscaler : public RT64::TextureObserver {
    public:
        void start() {
            load_ui_list();
            // Left by the ESRGAN modes, which are gone: the downloaded program and their caches.
            std::error_code ec;
            for (const char* folder : { "realesrgan", "cache/esrgan_x2", "cache/esrgan_x4", "cache/esrgan_x2_from_x4" }) {
                std::filesystem::remove_all(root() / folder, ec);
            }
            worker = std::thread([this]() { run(); });
            worker.detach(); // Lives as long as the game; the process ends with it.
            RT64::ActiveTextureObserver = this;
        }

        void set_mode(Mode new_mode, const std::string& new_command) {
            std::unique_lock lock(mutex);
            if (new_mode == mode && new_command == command) {
                return;
            }
            mode = new_mode;
            command = new_command;
            generation++;
            cancel_run = true; // Stops an upscaler that's running for the old mode.
            clear_requested = true;
            message.clear();
            pending.clear();
            for (auto& [key, entry] : entries) {
                entry.ready = false;
                entry.failed = false;
                entry.dependents.clear();
                entry.queued = (mode != Mode::Off) && !entry.ui;
                if (entry.queued) {
                    pending.push_back(key);
                }
            }
            // And the exact images.
            for (const auto& [hash, strip] : exact_strips) {
                exact_applies.push_back(hash);
            }
            changed.notify_all();
        }

        std::string status() {
            std::unique_lock lock(mutex);
            if (!message.empty()) {
                return message;
            }
            if (mode != Mode::Off && (!pending.empty() || busy)) {
                return busy_message.empty() ? "Upscaling textures..." : busy_message;
            }
            return "";
        }

        // RT64::TextureObserver. Runs on the display list thread for every textured tile, so it does as little as it
        // can: one map lookup for textures it has decided on already.
        void textureDrawn(uint64_t hash, const uint8_t* tmem, const RT64::LoadTile& tile, uint16_t width, uint16_t height,
                          uint32_t tlut, bool world, uint64_t frame) override {
            std::unique_lock lock(mutex);
            Seen& seen = seen_hashes[hash];
            if (!world) {
                if (!seen.ui) {
                    seen.ui = true;
                    if (!exact_images.empty() && tlut != 0 && tile.siz == 1) {
                        find_exact(hash, tmem, tile, width, height, tlut);
                    }
                    if (seen.key != 0) {
                        mark_ui(seen.key);
                    }
                }
                return;
            }
            if (seen.ui || seen.kept || frame == seen.last_frame) {
                return;
            }
            seen.last_frame = frame;
            if (++seen.frames < stable_frames) {
                return;
            }

            seen.kept = true;
            if (width > 1024 || height > 1024) {
                return; // A tile sized past what TMEM holds; nothing real to upscale.
            }
            if (source != nullptr && rush2::rom2049::dc::in_texture_pack(hash)) {
                source_packed++; // Drawn with the disc's image from its texture pack.
                return;
            }
            std::vector<uint8_t> indices;
            Image image = decode_tmem(tmem, tile.fmt, tile.siz, tile.tmem, tile.line, tile.palette, tlut, width, height,
                                      tlut != 0 ? &indices : nullptr);
            if (!worth_upscaling(image)) {
                return;
            }
            uint64_t key = content_key(image);
            seen.key = key;
            auto [it, inserted] = entries.try_emplace(key);
            Entry& entry = it->second;
            if (inserted) {
                entry.image = std::move(image);
                entry.edge_s = edge_of(tile.cms, tile.masks);
                entry.edge_t = edge_of(tile.cmt, tile.maskt);
                entry.ui = ui_keys.count(key) != 0;
                if (tlut != 0) {
                    entry.index_key = index_key(indices, width, height, tile.siz);
                    // The first palette variant of these indices is the one that gets upscaled.
                    auto [base_it, new_base] = bases.try_emplace(entry.index_key, key);
                    if (!new_base && base_it->second != key) {
                        entry.base = base_it->second;
                    }
                }
            }
            entry.hashes.push_back(hash);
            if (entry.ui || mode == Mode::Off || entry.failed) {
                return;
            }
            if (entry.ready) {
                applies.emplace_back(hash, key);
                changed.notify_all();
            }
            else if (!entry.queued) {
                entry.queued = true;
                pending.push_back(key);
                changed.notify_all();
            }
        }

        // Writes every kept 3D texture to the dump folder (skipping ones already there). Returns how many it has.
        size_t dump() {
            std::vector<std::pair<uint64_t, Entry>> snapshot;
            {
                std::unique_lock lock(mutex);
                for (const auto& [key, entry] : entries) {
                    if (!entry.ui) {
                        snapshot.emplace_back(key, entry);
                    }
                }
            }

            std::error_code ec;
            std::filesystem::create_directories(dump_dir(), ec);
            std::filesystem::create_directories(upscaled_dir(), ec);
            std::map<uint64_t, std::set<uint64_t>> index = read_dump_index();
            for (const auto& [key, entry] : snapshot) {
                std::filesystem::path path = dump_dir() / (key_name(key) + ".png");
                if (!std::filesystem::exists(path, ec)) {
                    write_png(path, entry.image);
                }
                index[key].insert(entry.hashes.begin(), entry.hashes.end());
            }
            write_dump_index(index);
            write_dump_readme();
            return index.size();
        }

        // Builds the texture pack mod from the upscaled folder. Returns how many textures it has, or -1 on failure.
        int install() {
            std::map<uint64_t, std::set<uint64_t>> index = read_dump_index();
            std::filesystem::path pack_dir = recomp::mods::get_mods_directory() / pack_id;
            std::filesystem::path texture_dir = pack_dir / "textures";
            std::error_code ec;
            std::filesystem::create_directories(texture_dir, ec);
            for (const auto& file : std::filesystem::directory_iterator(texture_dir, ec)) {
                std::filesystem::remove(file.path(), ec); // Textures no longer in the upscaled folder.
            }

            std::vector<PackTexture> textures;
            std::set<uint64_t> done;
            for (const auto& file : std::filesystem::directory_iterator(upscaled_dir(), ec)) {
                uint64_t key;
                if (!file.is_regular_file() || !parse_key_name(path_utf8(file.path().filename()), key)) {
                    continue;
                }
                auto it = index.find(key);
                Image upscaled, original;
                if (it == index.end() || done.count(key) || !read_image(file.path(), upscaled) ||
                    !read_image(dump_dir() / (key_name(key) + ".png"), original)) {
                    continue;
                }
                restore_alpha(upscaled, original);
                std::string name = "textures/" + key_name(key) + ".dds";
                if (!write_file(pack_dir / name, make_dds(upscaled))) {
                    continue;
                }
                done.insert(key);
                textures.push_back({ name, std::vector<uint64_t>(it->second.begin(), it->second.end()) });
            }
            if (textures.empty()) {
                return 0;
            }

            std::string database = pack_database(textures);
            std::string manifest = pack_manifest(pack_id, "Custom Upscaled Textures",
                "Textures you upscaled from Settings > Graphics > Dump Textures, installed with Install Upscaled.");
            if (!write_file(pack_dir / "rt64.json", std::vector<uint8_t>(database.begin(), database.end())) ||
                !write_file(pack_dir / "mod.json", std::vector<uint8_t>(manifest.begin(), manifest.end()))) {
                return -1;
            }
            zip_folder(pack_dir, root() / (pack_id + ".rtz")); // A copy to share; the folder is what's installed.

            recomp::mods::scan_mods();
            recomp::mods::enable_mod(pack_id, true);
            recompui::update_mod_list(false);
            recompui::renderer::trigger_texture_pack_update(); // Picks the new files up if it was enabled already.
            return (int)textures.size();
        }

        void set_source(std::shared_ptr<const rush2::rom2049::Source> new_source) {
            std::unique_lock lock(mutex);
            if (new_source == source) {
                return;
            }
            source = new_source;
        }

    private:
        std::mutex mutex;
        std::condition_variable changed;
        std::thread worker;
        std::unordered_map<uint64_t, Seen> seen_hashes;
        std::unordered_map<uint64_t, Entry> entries;
        std::unordered_map<uint64_t, uint64_t> bases;         // Index key -> content key of the variant upscaled.
        std::unordered_set<uint64_t> ui_keys;
        std::deque<uint64_t> pending;
        std::vector<std::pair<uint64_t, uint64_t>> applies;   // (hash, key) for the worker to replace.
        std::vector<uint64_t> removals;                       // Hashes whose replacement goes.
        std::vector<uint64_t> new_ui_keys;                    // To append to the UI list.
        Mode mode = Mode::Off;
        std::string command;
        uint64_t generation = 0;
        std::atomic<bool> cancel_run = false;
        bool clear_requested = false;
        bool busy = false;
        std::string busy_message;
        std::string message;
        // A Dreamcast source, whose texture pack's textures (rush2::rom2049::dc::in_texture_pack) aren't upscaled, and
        // how many kept 3D textures that pack has replaced, for the log.
        std::shared_ptr<const rush2::rom2049::Source> source;
        size_t source_packed = 0, source_logged = 0;

        // Exact images (add_exact_image), the hashes found to be strips of one (image, first row), and hashes to give
        // theirs.
        struct ExactImage {
            std::vector<uint8_t> indices;
            Image image;
        };
        struct ExactStrip {
            size_t image;
            uint32_t row;
        };
        std::vector<ExactImage> exact_images;
        std::unordered_map<uint64_t, ExactStrip> exact_strips;
        std::vector<uint64_t> exact_applies;

    public:
        void add_exact(std::vector<uint8_t> indices, Image image) {
            std::unique_lock lock(mutex);
            exact_images.push_back({ std::move(indices), std::move(image) });
        }

    private:
        // Called with the mutex held, once per 2D CI8 hash.
        void find_exact(uint64_t hash, const uint8_t* tmem, const RT64::LoadTile& tile, uint16_t width, uint16_t height,
                        uint32_t tlut) {
            std::vector<uint8_t> indices;
            decode_tmem(tmem, tile.fmt, tile.siz, tile.tmem, tile.line, tile.palette, tlut, width, height, &indices);
            if (indices.empty() || std::all_of(indices.begin(), indices.end(), [&](uint8_t i) { return i == indices[0]; })) {
                return;
            }
            for (size_t i = 0; i < exact_images.size(); i++) {
                const ExactImage& e = exact_images[i];
                if (e.image.width != width || e.image.height < height) {
                    continue;
                }
                for (uint32_t row = 0; row + height <= e.image.height; row++) {
                    if (memcmp(&e.indices[size_t(row) * width], indices.data(), indices.size()) == 0) {
                        exact_strips[hash] = { i, row };
                        exact_applies.push_back(hash);
                        changed.notify_all();
                        return;
                    }
                }
            }
        }

        // Worker: gives each strip's hash the whole image, as the region of it the strip is (RT64::ReplacementRegion).
        // The image is the texture's size, so RT64 samples it exactly as it samples the game's texture.
        void apply_exact(const std::vector<uint64_t>& todo) {
            for (uint64_t hash : todo) {
                ExactStrip strip;
                Image image;
                {
                    std::unique_lock lock(mutex);
                    auto it = exact_strips.find(hash);
                    if (it == exact_strips.end()) {
                        continue;
                    }
                    strip = it->second;
                    image = exact_images[strip.image].image;
                }
                uint64_t content = content_key(image) ^ 0xE8AC71A6Eull;
                std::vector<uint8_t> bytes;
                if (!RT64::hasLiveReplacementContent(content)) {
                    bytes = make_dds(image);
                }
                RT64::ReplacementRegion region;
                region.y = strip.row;
                region.sourceWidth = image.width;
                region.sourceHeight = image.height;
                RT64::addLiveReplacement(hash, content, bytes, region);
            }
        }

        // Called with the mutex held.
        void mark_ui(uint64_t key) {
            auto it = entries.find(key);
            if (it == entries.end() || it->second.ui) {
                return;
            }
            it->second.ui = true;
            ui_keys.insert(key);
            new_ui_keys.push_back(key);
            removals.insert(removals.end(), it->second.hashes.begin(), it->second.hashes.end());
            changed.notify_all();
        }

        void load_ui_list() {
            std::ifstream file{ ui_list() };
            std::string line;
            while (std::getline(file, line)) {
                uint64_t key;
                if (parse_key_name(line, key)) {
                    ui_keys.insert(key);
                }
            }
        }

        std::map<uint64_t, std::set<uint64_t>> read_dump_index() {
            std::map<uint64_t, std::set<uint64_t>> index;
            std::ifstream file{ dump_dir() / "hashes.txt" };
            std::string line;
            while (std::getline(file, line)) {
                std::istringstream words{ line };
                std::string word;
                uint64_t key, hash;
                if (!(words >> word) || !parse_key_name(word, key)) {
                    continue;
                }
                while (words >> word) {
                    if (parse_key_name(word, hash)) {
                        index[key].insert(hash);
                    }
                }
            }
            return index;
        }

        void write_dump_index(const std::map<uint64_t, std::set<uint64_t>>& index) {
            std::string text = "# <image key> <RT64 hashes it replaces>. Written by Dump Textures, read by Install Upscaled.\n";
            for (const auto& [key, hashes] : index) {
                text += key_name(key);
                for (uint64_t hash : hashes) {
                    text += " " + key_name(hash);
                }
                text += "\n";
            }
            write_file(dump_dir() / "hashes.txt", std::vector<uint8_t>(text.begin(), text.end()));
        }

        void write_dump_readme() {
            std::string text =
                "Rush 2 textures, one PNG per texture, named by their contents. Only textures drawn in 3D are dumped;\n"
                "HUD, menu and font images are left out. Dump again after playing more tracks and cars to add theirs.\n"
                "\n"
                "1. Upscale these images with any program (Topaz Gigapixel, an image editor...).\n"
                "2. Save the results into the \"upscaled\" folder next to this one. Any size works. Keep the first\n"
                "   16 characters of each file name: extra text after them (\"_gigapixel-2x\") is fine. PNG keeps\n"
                "   transparency; when an image comes back without it, the original's is used.\n"
                "3. Press Install Upscaled in Settings > Graphics. The pack shows up in the mods menu as\n"
                "   \"Custom Upscaled Textures\", and texture_upscale/rush2_custom_textures.rtz is a copy to share.\n";
            write_file(dump_dir() / "README.txt", std::vector<uint8_t>(text.begin(), text.end()));
        }

        // The worker: removes and adds live replacements, recolors palette variants and upscales the rest.
        void run() {
            while (true) {
                std::vector<uint64_t> to_remove, to_append;
                std::vector<std::pair<uint64_t, uint64_t>> to_apply;
                std::vector<Job> recolors, upscales;
                bool clear = false;
                Mode batch_mode;
                std::string batch_command;
                uint64_t batch_generation;
                {
                    std::unique_lock lock(mutex);
                    changed.wait_for(lock, std::chrono::milliseconds(500), [this]() {
                        return clear_requested || !removals.empty() || !applies.empty() || !new_ui_keys.empty() ||
                               !exact_applies.empty() ||
                               (!pending.empty() && mode != Mode::Off);
                    });
                    clear = clear_requested;
                    clear_requested = false;
                    cancel_run = false;
                    to_remove.swap(removals);
                    to_apply.swap(applies);
                    to_append.swap(new_ui_keys);
                    batch_mode = mode;
                    batch_command = command;
                    batch_generation = generation;
                    take_jobs(recolors, upscales);
                    busy = !recolors.empty() || !upscales.empty();
                }

                if (clear) {
                    RT64::clearLiveReplacements();
                }
                for (uint64_t hash : to_remove) {
                    RT64::removeLiveReplacement(hash);
                }
                {
                    std::unique_lock lock(mutex);
                    if (source_packed != source_logged) {
                        source_logged = source_packed;
                        printf("[upscale] %zu Dreamcast textures drawn from its texture pack\n", source_packed);
                        fflush(stdout);
                    }
                }
                std::vector<uint64_t> to_exact;
                {
                    std::unique_lock lock(mutex);
                    to_exact.swap(exact_applies);
                }
                apply_exact(to_exact);
                if (!to_append.empty()) {
                    std::error_code ec;
                    std::filesystem::create_directories(root(), ec);
                    std::ofstream file{ ui_list(), std::ios::app };
                    for (uint64_t key : to_append) {
                        file << key_name(key) << "\n";
                    }
                }
                for (const auto& [hash, key] : to_apply) {
                    apply(key, { hash }, batch_mode, batch_generation);
                }
                for (const Job& job : recolors) {
                    recolor_job(job, batch_mode, batch_generation);
                }
                if (!upscales.empty()) {
                    process(upscales, batch_mode, batch_command, batch_generation);
                }
                std::unique_lock lock(mutex);
                busy = false;
                busy_message.clear();
            }
        }

        // Takes the next jobs off the queue (mutex held). Variants whose base is ready are recolored; those whose base
        // is still waiting wait with it; the rest are upscaled, one external run's worth at a time.
        void take_jobs(std::vector<Job>& recolors, std::vector<Job>& upscales) {
            while (!pending.empty() && mode != Mode::Off && upscales.size() < batch_size && recolors.size() < 256) {
                uint64_t key = pending.front();
                pending.pop_front();
                Entry& entry = entries[key];
                if (!entry.queued || entry.ui || entry.ready) {
                    continue;
                }
                if (entry.base != 0) {
                    auto base_it = entries.find(entry.base);
                    Entry* base = base_it != entries.end() ? &base_it->second : nullptr;
                    if (base != nullptr && base->ready) {
                        recolors.push_back({ key, entry, entry.base, base->image, base->edge_s, base->edge_t });
                        continue;
                    }
                    if (base != nullptr && !base->failed && !base->ui) {
                        if (!base->queued) {
                            base->queued = true;
                            pending.push_back(entry.base);
                        }
                        base->dependents.push_back(key); // Requeued when the base is done.
                        continue;
                    }
                    entry.base = 0; // The base can't be upscaled: upscale this one itself.
                }
                upscales.push_back({ key, entry });
            }
        }

        std::filesystem::path cache_path(Mode for_mode, uint64_t key) {
            return root() / "cache" / mode_folder(for_mode) / (key_name(key) + ".dds");
        }

        // Live replacement keys differ per mode, so a result that lands after a mode change can't stand in for the
        // new mode's image of the same texture.
        static uint64_t live_key(uint64_t key, Mode for_mode) {
            return key ^ (uint64_t(for_mode) * 0x9E3779B97F4A7C15ull);
        }

        // Replaces the given hashes of a texture with its cached upscale, if the mode is still the one it was made in.
        void apply(uint64_t key, const std::vector<uint64_t>& hashes, Mode for_mode, uint64_t for_generation) {
            {
                std::unique_lock lock(mutex);
                auto it = entries.find(key);
                if (for_generation != generation || it == entries.end() || it->second.ui) {
                    return;
                }
            }
            uint64_t content = live_key(key, for_mode);
            std::vector<uint8_t> bytes;
            if (!RT64::hasLiveReplacementContent(content)) {
                bytes = read_file(cache_path(for_mode, key));
                if (bytes.empty()) {
                    return;
                }
            }
            for (uint64_t hash : hashes) {
                RT64::addLiveReplacement(hash, content, bytes);
            }
        }

        void set_failed(uint64_t key, uint64_t for_generation) {
            std::unique_lock lock(mutex);
            if (for_generation != generation) {
                return;
            }
            Entry& entry = entries[key];
            entry.queued = false;
            entry.failed = true;
            // Variants waiting on it get upscaled themselves.
            for (uint64_t dependent : entry.dependents) {
                entries[dependent].base = 0;
                pending.push_back(dependent);
            }
            entry.dependents.clear();
            changed.notify_all();
        }

        void set_message(uint64_t for_generation, const std::string& text) {
            std::unique_lock lock(mutex);
            if (for_generation == generation) {
                message = text;
            }
        }

        void set_ready(uint64_t key, uint64_t for_generation, Mode for_mode) {
            std::vector<uint64_t> hashes;
            {
                std::unique_lock lock(mutex);
                if (for_generation != generation) {
                    return;
                }
                Entry& entry = entries[key];
                entry.queued = false;
                entry.ready = true;
                hashes = entry.hashes;
                for (uint64_t dependent : entry.dependents) {
                    pending.push_front(dependent); // Recolors are quick: ahead of other upscales.
                }
                entry.dependents.clear();
                changed.notify_all();
            }
            apply(key, hashes, for_mode, for_generation);
        }

        bool store(uint64_t key, Image upscaled, const Image& original, uint64_t for_generation, Mode for_mode) {
            restore_alpha(upscaled, original);
            std::error_code ec;
            std::filesystem::create_directories(cache_path(for_mode, 0).parent_path(), ec);
            if (!write_file(cache_path(for_mode, key), make_dds(upscaled))) {
                set_failed(key, for_generation);
                return false;
            }
            set_ready(key, for_generation, for_mode);
            return true;
        }

        void recolor_job(const Job& job, Mode for_mode, uint64_t for_generation) {
            std::error_code ec;
            if (std::filesystem::exists(cache_path(for_mode, job.key), ec)) {
                set_ready(job.key, for_generation, for_mode);
                return;
            }
            Image base_upscale;
            if (!read_dds(read_file(cache_path(for_mode, job.base_key)), base_upscale) ||
                job.base_image.width != job.entry.image.width || job.base_image.height != job.entry.image.height) {
                // The base's upscale is gone: upscale this variant itself.
                std::unique_lock lock(mutex);
                entries[job.key].base = 0;
                pending.push_back(job.key);
                changed.notify_all();
                return;
            }
            store(job.key, recolor(base_upscale, job.base_image, job.entry.image, job.base_edge_s, job.base_edge_t),
                  job.entry.image, for_generation, for_mode);
        }

        Image hq(const Entry& entry, uint32_t scale) {
            return unpad(hq_upscale(pad(entry.image, border, entry.edge_s, entry.edge_t), scale), entry.image, border);
        }

        void process(const std::vector<Job>& jobs, Mode for_mode, const std::string& for_command, uint64_t for_generation) {
            // Upscaled in an earlier session: just load them.
            std::vector<const Job*> todo;
            std::error_code ec;
            for (const Job& job : jobs) {
                if (std::filesystem::exists(cache_path(for_mode, job.key), ec)) {
                    set_ready(job.key, for_generation, for_mode);
                }
                else {
                    todo.push_back(&job);
                }
            }
            if (todo.empty()) {
                return;
            }

            const uint32_t scale = mode_scale(for_mode);
            if (for_mode == Mode::Hq2x || for_mode == Mode::Hq4x) {
                for (const Job* job : todo) {
                    store(job->key, hq(job->entry, scale), job->entry.image, for_generation, for_mode);
                }
                return;
            }

            // Custom: the user's command line on a folder of padded images.
            std::filesystem::path in = work_dir() / "in", out = work_dir() / "out";
            if (for_command.find("{input}") == std::string::npos || for_command.find("{output}") == std::string::npos) {
                set_message(for_generation, "The custom upscaler command needs {input} and {output} in it.");
                for (const Job* job : todo) {
                    set_failed(job->key, for_generation);
                }
                return;
            }
            std::string line = for_command;
            line.replace(line.find("{input}"), 7, quoted(in));
            line.replace(line.find("{output}"), 8, quoted(out));

            {
                std::unique_lock lock(mutex);
                busy_message = "Upscaling textures...";
            }
            std::filesystem::remove_all(work_dir(), ec);
            std::filesystem::create_directories(in, ec);
            std::filesystem::create_directories(out, ec);
            for (const Job* job : todo) {
                write_png(in / (key_name(job->key) + ".png"), pad(job->entry.image, border, job->entry.edge_s, job->entry.edge_t));
            }

            int result = run_command(line, &cancel_run);
            if (cancel_run) {
                std::filesystem::remove_all(work_dir(), ec);
                return; // The mode changed; the new mode requeued everything.
            }

            // Match outputs by the key their names start with: tools often add a suffix.
            std::unordered_map<uint64_t, std::filesystem::path> outputs;
            for (const auto& file : std::filesystem::directory_iterator(out, ec)) {
                uint64_t key;
                if (file.is_regular_file() && parse_key_name(path_utf8(file.path().filename()), key)) {
                    outputs.emplace(key, file.path());
                }
            }

            size_t missing = 0, replaced = 0;
            for (const Job* job : todo) {
                Image output, upscaled;
                auto it = outputs.find(job->key);
                if (it != outputs.end() && read_image(it->second, output)) {
                    upscaled = unpad(output, job->entry.image, border);
                }
                if (upscaled.empty()) {
                    missing++;
                    set_failed(job->key, for_generation);
                    continue;
                }
                if (!matches_original(upscaled, job->entry.image)) {
                    // Garbage from the upscaler: use HQ2x instead.
                    replaced++;
                    upscaled = hq(job->entry, 2);
                }
                store(job->key, std::move(upscaled), job->entry.image, for_generation, for_mode);
            }
            std::filesystem::remove_all(work_dir(), ec);

            if (missing == todo.size()) {
                set_message(for_generation, "The upscaler didn't produce any images (exit code " + std::to_string(result) + ").");
            }
            else if (replaced > 0) {
                set_message(for_generation, "Some images the upscaler made didn't look like the originals, so HQ2x "
                    "was used for them.");
            }
            std::this_thread::sleep_for(run_pause);
        }
    };

    // Never destroyed: its worker thread and RT64's display list thread can still be using it while the game exits.
    Upscaler& upscaler = *new Upscaler();
    std::once_flag started;

    void apply_config(const recomp::config::ConfigValueVariant& mode_value, const recomp::config::ConfigValueVariant& command_value) {
        std::call_once(started, []() { upscaler.start(); });
        Mode mode = static_cast<Mode>(std::get<uint32_t>(mode_value));
        const std::string* command = std::get_if<std::string>(&command_value);
        upscaler.set_mode(mode, command != nullptr ? *command : std::string());
    }
}

void add_options(recomp::config::Config& config) {
    config.add_enum_option(
        mode_option_id,
        "Texture Upscaling",
        "Redraws the game's textures at a higher resolution as they first appear, and keeps the results so each "
        "texture is only upscaled once. Car paint jobs reuse one upscale. Only textures on 3D surfaces are "
        "upscaled: the HUD, menus and text keep their original look. Texture packs you install take priority."
        "<br/><br/>"
        "<recomp-color primary>Off</recomp-color> uses the original textures. "
        "<recomp-color primary>HQ2x</recomp-color> and <recomp-color primary>HQ4x</recomp-color> smooth the pixels "
        "like emulators' texture enhancement. "
        "<recomp-color primary>Custom</recomp-color> runs the command below instead.",
        {
            { Mode::Off, "Off", "Off" },
            { Mode::Hq2x, "Hq2x", "HQ2x" },
            { Mode::Hq4x, "Hq4x", "HQ4x" },
            { Mode::Custom, "Custom", "Custom" },
        },
        Mode::Off
    );
    // Configs saved while the ESRGAN modes existed: their 2x and 4x become HQ2x and HQ4x.
    config.on_json_parse_option(mode_option_id, [](const nlohmann::json& value) -> recomp::config::ConfigValueVariant {
        std::string name = value.is_string() ? value.get<std::string>() : "";
        if (name == "Hq2x" || name == "Esrgan2x") return (uint32_t)Mode::Hq2x;
        if (name == "Hq4x" || name == "Esrgan4x") return (uint32_t)Mode::Hq4x;
        if (name == "Custom") return (uint32_t)Mode::Custom;
        return (uint32_t)Mode::Off;
    });

    config.add_string_option(
        command_option_id,
        "Custom Upscaler Command",
        "The command line Custom upscaling runs. {input} is replaced with a folder of PNG images and {output} with "
        "the folder to save the upscaled images in (any scale, file names starting like the input's). For example: "
        "\"C:\\path\\to\\upscaler.exe\" -i {input} -o {output} -s 2",
        ""
    );
    // Hidden while the mode is one of these: only Custom shows it.
    config.add_option_hidden_dependency(command_option_id, mode_option_id, Mode::Off, Mode::Hq2x, Mode::Hq4x);

    // The config can move as later tabs are added, so the callbacks look it up again.
    config.add_option_change_callback(mode_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            apply_config(cur_value, recompui::config::get_graphics_config().get_option_value(command_option_id));
        });
    config.add_option_change_callback(command_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            apply_config(recompui::config::get_graphics_config().get_option_value(mode_option_id), cur_value);
        });
}

void apply_loaded_options(recomp::config::Config& config) {
    apply_config(config.get_option_value(mode_option_id), config.get_option_value(command_option_id));
}

void set_texture_source(std::shared_ptr<const rush2::rom2049::Source> source) {
    upscaler.set_source(source != nullptr && source->is_dreamcast() ? source : nullptr);
}

void add_exact_image(std::vector<uint8_t> indices, Image image) {
    upscaler.add_exact(std::move(indices), std::move(image));
}

void add_buttons(rush2::ui::OptionsPage* page) {
    using namespace recompui;
    ContextId context = get_current_context();
    std::string note = "Dump the textures you have driven past, upscale them with any program (Topaz Gigapixel, "
                       "say), save the results in the upscaled folder next to the dump folder, then install them "
                       "as a texture pack.";
    std::string status = upscaler.status();
    if (!status.empty()) {
        note += " " + status;
    }
    page->add_heading("Your Own Upscales", note);

    // The buttons get a row of their own under the heading, so they keep their full size; it scrolls into view as
    // the controller reaches them.
    Element* row = page->add_row();

    Button* dump_button = context.create_element<Button>(row, "Dump Textures", ButtonStyle::Secondary);
    dump_button->add_pressed_callback([dump_button]() {
        size_t count = upscaler.dump();
        dump_button->set_text("Dumped " + std::to_string(count) + " Textures");
    });

    Button* folder_button = context.create_element<Button>(row, "Open Dump Folder", ButtonStyle::Secondary);
    folder_button->add_pressed_callback([]() {
        open_folder(dump_dir());
    });

    Button* install_button = context.create_element<Button>(row, "Install Upscaled", ButtonStyle::Secondary);
    install_button->add_pressed_callback([install_button]() {
        int count = upscaler.install();
        if (count > 0) {
            install_button->set_text("Installed " + std::to_string(count) + " Textures");
        }
        else {
            install_button->set_text(count == 0 ? "Nothing in the Upscaled Folder" : "Install Failed");
        }
    });
}

}
