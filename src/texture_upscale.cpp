// Texture upscaling (Settings > Graphics > Texture Upscaling). See docs/texture_upscaling.md.
//
// RT64 shows the recomp every texture a draw call samples (RT64::TextureObserver, added in lib/patches/rt64.patch).
// Textures drawn by 3D geometry with a perspective projection on a few separate frames are decoded from TMEM and
// kept; any texture also drawn in 2D (HUD, menus, fonts: rectangles and orthographic draws) is left alone, and if
// it was upscaled already its replacement is taken off again. Textures are identified by their decoded pixels
// (content_key), so the same image loaded under different RT64 hashes is upscaled and stored once.
//
// Automatic mode runs an upscaler on a background thread in batches and caches each result as a DDS with mipmaps in
// <app folder>/texture_upscale/cache/<upscaler>/<key>.dds, so nothing is upscaled twice, across sessions too. Results
// reach the renderer as RT64 live replacements (rt64_live_textures.h): no pack reload, and any installed texture pack
// that replaces the same texture wins. The default upscaler is Real-ESRGAN (realesr-animevideov3, ncnn/Vulkan),
// downloaded on first use; Custom runs any command line on a folder of images.
//
// For upscalers without a command line (Topaz Gigapixel's app, say): Dump Textures writes the kept textures as
// <key>.png into texture_upscale/dump, the user saves upscaled copies (any size, file names starting with the key)
// into texture_upscale/upscaled, and Install Upscaled turns those into the texture pack mod "rush2_custom_textures"
// (a folder in the mods folder, enabled right away) plus a shareable .rtz of it.

#include <atomic>
#include <condition_variable>
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
#include "texture_upscale.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace rush2::upscale {

const char* const mode_option_id = "texture_upscaling";
const char* const command_option_id = "texture_upscale_command";

namespace {
    enum class Mode : uint32_t { Off, Esrgan2x, Esrgan4x, Custom };

    constexpr uint32_t stable_frames = 3;  // Frames a texture is drawn in 3D before it's kept (skips one-frame textures).
    constexpr uint32_t border = 8;         // Texels of padding around images given to an automatic upscaler.
    constexpr size_t batch_size = 48;      // Images per upscaler run.
    const std::string pack_id = "rush2_custom_textures";

    // Real-ESRGAN ncnn/Vulkan builds, release v0.2.5.0 (BSD-3-Clause). The zips hold the program and its models.
#if defined(_WIN32)
    const char* esrgan_url = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-windows.zip";
    const char* esrgan_exe = "realesrgan-ncnn-vulkan.exe";
#elif defined(__APPLE__)
    const char* esrgan_url = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-macos.zip";
    const char* esrgan_exe = "realesrgan-ncnn-vulkan";
#else
    const char* esrgan_url = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.5.0/realesrgan-ncnn-vulkan-20220424-ubuntu.zip";
    const char* esrgan_exe = "realesrgan-ncnn-vulkan";
#endif

    std::string utf8(const std::filesystem::path& path) {
        auto text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    std::string quoted(const std::filesystem::path& path) {
        return "\"" + utf8(path) + "\"";
    }

    std::filesystem::path root() { return recompui::file::get_app_folder_path() / "texture_upscale"; }
    std::filesystem::path esrgan_dir() { return root() / "realesrgan"; }
    std::filesystem::path dump_dir() { return root() / "dump"; }
    std::filesystem::path upscaled_dir() { return root() / "upscaled"; }
    std::filesystem::path work_dir() { return root() / "work"; }
    std::filesystem::path ui_list() { return root() / "ui_textures.txt"; }

    const char* mode_folder(Mode mode) {
        switch (mode) {
            case Mode::Esrgan2x: return "esrgan_x2";
            case Mode::Esrgan4x: return "esrgan_x4";
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
        bool ui = false;            // Also drawn in 2D: never replaced.
        bool queued = false;        // Waiting for the worker in the current mode.
        bool ready = false;         // Upscaled in the current mode; its hashes get the replacement as they show up.
        bool failed = false;        // The upscaler gave nothing usable for it in the current mode.
    };

    // What the observer knows of an RT64 hash.
    struct Seen {
        uint64_t key = 0;
        uint64_t last_frame = UINT64_MAX;
        uint32_t frames = 0;
        bool ui = false;
        bool kept = false;          // Decoded already (key is 0 when it wasn't worth keeping).
    };

    class Upscaler : public RT64::TextureObserver {
    public:
        void start() {
            load_ui_list();
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
            clear_requested = true;
            failed_message.clear();
            pending.clear();
            for (auto& [key, entry] : entries) {
                entry.ready = false;
                entry.failed = false;
                entry.queued = (mode != Mode::Off) && !entry.ui;
                if (entry.queued) {
                    pending.push_back(key);
                }
            }
            changed.notify_all();
        }

        std::string status() {
            std::unique_lock lock(mutex);
            if (!failed_message.empty()) {
                return failed_message;
            }
            if (mode == Mode::Off) {
                return "";
            }
            if (!pending.empty() || busy) {
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
            Image image = decode_tmem(tmem, tile.fmt, tile.siz, tile.tmem, tile.line, tile.palette, tlut, width, height);
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
                if (!file.is_regular_file() || !parse_key_name(utf8(file.path().filename()), key)) {
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

    private:
        std::mutex mutex;
        std::condition_variable changed;
        std::thread worker;
        std::unordered_map<uint64_t, Seen> seen_hashes;
        std::unordered_map<uint64_t, Entry> entries;
        std::unordered_set<uint64_t> ui_keys;
        std::deque<uint64_t> pending;
        std::vector<std::pair<uint64_t, uint64_t>> applies;   // (hash, key) for the worker to replace.
        std::vector<uint64_t> removals;                       // Hashes whose replacement goes.
        std::vector<uint64_t> new_ui_keys;                    // To append to the UI list.
        Mode mode = Mode::Off;
        std::string command;
        uint64_t generation = 0;
        bool clear_requested = false;
        bool busy = false;
        std::string busy_message;
        std::string failed_message;

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
                "1. Upscale these images with any program (Topaz Gigapixel, Real-ESRGAN, an image editor...).\n"
                "2. Save the results into the \"upscaled\" folder next to this one. Any size works. Keep the first\n"
                "   16 characters of each file name: extra text after them (\"_gigapixel-2x\") is fine. PNG keeps\n"
                "   transparency; when an image comes back without it, the original's is used.\n"
                "3. Press Install Upscaled in Settings > Graphics. The pack shows up in the mods menu as\n"
                "   \"Custom Upscaled Textures\", and texture_upscale/rush2_custom_textures.rtz is a copy to share.\n";
            write_file(dump_dir() / "README.txt", std::vector<uint8_t>(text.begin(), text.end()));
        }

        // The worker: removes and adds live replacements and runs the upscaler on batches of pending textures.
        void run() {
            while (true) {
                std::vector<uint64_t> to_remove, to_append;
                std::vector<std::pair<uint64_t, uint64_t>> to_apply;
                std::vector<uint64_t> batch;
                std::vector<Entry> batch_entries;
                bool clear = false;
                Mode batch_mode;
                std::string batch_command;
                uint64_t batch_generation;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [this]() {
                        return clear_requested || !removals.empty() || !applies.empty() || !new_ui_keys.empty() ||
                               (!pending.empty() && mode != Mode::Off);
                    });
                    clear = clear_requested;
                    clear_requested = false;
                    to_remove.swap(removals);
                    to_apply.swap(applies);
                    to_append.swap(new_ui_keys);
                    batch_mode = mode;
                    batch_command = command;
                    batch_generation = generation;
                    while (!pending.empty() && batch.size() < batch_size && mode != Mode::Off) {
                        uint64_t key = pending.front();
                        pending.pop_front();
                        Entry& entry = entries[key];
                        if (!entry.queued || entry.ui) {
                            continue;
                        }
                        batch.push_back(key);
                        batch_entries.push_back(entry);
                    }
                    busy = !batch.empty();
                }

                if (clear) {
                    RT64::clearLiveReplacements();
                }
                for (uint64_t hash : to_remove) {
                    RT64::removeLiveReplacement(hash);
                }
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
                if (!batch.empty()) {
                    process(batch, batch_entries, batch_mode, batch_command, batch_generation);
                    std::unique_lock lock(mutex);
                    busy = false;
                    busy_message.clear();
                }
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

        void set_failed(const std::vector<uint64_t>& keys, uint64_t for_generation, const std::string& message) {
            std::unique_lock lock(mutex);
            if (for_generation != generation) {
                return;
            }
            for (uint64_t key : keys) {
                Entry& entry = entries[key];
                entry.queued = false;
                entry.failed = true;
            }
            if (!message.empty()) {
                failed_message = message;
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
            }
            apply(key, hashes, for_mode, for_generation);
        }

        bool ensure_esrgan(uint64_t for_generation) {
            std::filesystem::path exe = esrgan_dir() / esrgan_exe;
            std::error_code ec;
            if (std::filesystem::exists(exe, ec)) {
                return true;
            }
            {
                std::unique_lock lock(mutex);
                busy_message = "Downloading Real-ESRGAN (45 MB)...";
            }
            std::filesystem::create_directories(esrgan_dir(), ec);
            std::filesystem::path zip = root() / "realesrgan.zip";
#ifdef _WIN32
            std::string curl = "curl.exe";
#else
            std::string curl = "curl";
#endif
            int result = run_command(curl + " -L --fail -s -o " + quoted(zip) + " " + esrgan_url);
            bool ok = result == 0 && extract_zip(zip, esrgan_dir()) && std::filesystem::exists(exe, ec);
            std::filesystem::remove(zip, ec);
#ifndef _WIN32
            if (ok) {
                std::filesystem::permissions(exe, std::filesystem::perms::owner_exec | std::filesystem::perms::group_exec |
                    std::filesystem::perms::others_exec, std::filesystem::perm_options::add, ec);
            }
#endif
            if (!ok) {
                std::unique_lock lock(mutex);
                if (for_generation == generation) {
                    failed_message = "Couldn't download Real-ESRGAN. Check the internet connection, or extract " +
                        std::string(esrgan_url) + " into " + utf8(esrgan_dir()) + " and pick the setting again.";
                }
            }
            return ok;
        }

        void process(const std::vector<uint64_t>& keys, const std::vector<Entry>& batch_entries, Mode for_mode,
                     const std::string& for_command, uint64_t for_generation) {
            // Upscaled in an earlier session: just load them.
            std::vector<size_t> todo;
            std::error_code ec;
            for (size_t i = 0; i < keys.size(); i++) {
                if (std::filesystem::exists(cache_path(for_mode, keys[i]), ec)) {
                    set_ready(keys[i], for_generation, for_mode);
                }
                else {
                    todo.push_back(i);
                }
            }
            if (todo.empty()) {
                return;
            }

            std::vector<uint64_t> todo_keys;
            for (size_t i : todo) {
                todo_keys.push_back(keys[i]);
            }

            std::string line;
            std::filesystem::path in = work_dir() / "in", out = work_dir() / "out";
            if (for_mode == Mode::Custom) {
                if (for_command.find("{input}") == std::string::npos || for_command.find("{output}") == std::string::npos) {
                    set_failed(todo_keys, for_generation, "The custom upscaler command needs {input} and {output} in it.");
                    return;
                }
                line = for_command;
                line.replace(line.find("{input}"), 7, quoted(in));
                line.replace(line.find("{output}"), 8, quoted(out));
            }
            else {
                if (!ensure_esrgan(for_generation)) {
                    set_failed(todo_keys, for_generation, "");
                    return;
                }
                line = quoted(esrgan_dir() / esrgan_exe) + " -i " + quoted(in) + " -o " + quoted(out) +
                       " -s " + (for_mode == Mode::Esrgan4x ? "4" : "2") + " -n realesr-animevideov3 -m " +
                       quoted(esrgan_dir() / "models") + " -f png";
            }

            {
                std::unique_lock lock(mutex);
                busy_message = "Upscaling textures...";
            }
            std::filesystem::remove_all(work_dir(), ec);
            std::filesystem::create_directories(in, ec);
            std::filesystem::create_directories(out, ec);
            for (size_t i : todo) {
                const Entry& entry = batch_entries[i];
                write_png(in / (key_name(keys[i]) + ".png"), pad(entry.image, border, entry.edge_s, entry.edge_t));
            }

            int result = run_command(line);

            // Match outputs by the key their names start with: tools often add a suffix.
            std::unordered_map<uint64_t, std::filesystem::path> outputs;
            for (const auto& file : std::filesystem::directory_iterator(out, ec)) {
                uint64_t key;
                if (file.is_regular_file() && parse_key_name(utf8(file.path().filename()), key)) {
                    outputs.emplace(key, file.path());
                }
            }

            std::vector<uint64_t> missing;
            std::filesystem::create_directories(cache_path(for_mode, 0).parent_path(), ec);
            for (size_t i : todo) {
                uint64_t key = keys[i];
                const Entry& entry = batch_entries[i];
                Image output;
                auto it = outputs.find(key);
                if (it == outputs.end() || !read_image(it->second, output)) {
                    missing.push_back(key);
                    continue;
                }
                Image upscaled = unpad(output, entry.image, border);
                if (upscaled.empty()) {
                    missing.push_back(key);
                    continue;
                }
                restore_alpha(upscaled, entry.image);
                if (write_file(cache_path(for_mode, key), make_dds(upscaled))) {
                    set_ready(key, for_generation, for_mode);
                }
                else {
                    missing.push_back(key);
                }
            }
            std::filesystem::remove_all(work_dir(), ec);

            if (!missing.empty()) {
                set_failed(missing, for_generation, missing.size() == todo.size()
                    ? "The upscaler didn't produce any images (exit code " + std::to_string(result) + ")."
                    : "");
            }
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
        "Redraws the game's textures at a higher resolution with an AI upscaler, a few at a time in the background as "
        "they first appear, and keeps the results so each texture is only upscaled once. "
        "Only textures on 3D surfaces are upscaled: the HUD, menus and text keep their original look. "
        "Texture packs you install take priority.<br/><br/>"
        "<recomp-color primary>Off</recomp-color> uses the original textures. "
        "<recomp-color primary>ESRGAN 2x</recomp-color> and <recomp-color primary>ESRGAN 4x</recomp-color> use "
        "Real-ESRGAN, downloaded the first time (45 MB). "
        "<recomp-color primary>Custom</recomp-color> runs the command below instead.",
        {
            { Mode::Off, "Off", "Off" },
            { Mode::Esrgan2x, "Esrgan2x", "ESRGAN 2x" },
            { Mode::Esrgan4x, "Esrgan4x", "ESRGAN 4x" },
            { Mode::Custom, "Custom", "Custom" },
        },
        Mode::Off
    );

    config.add_string_option(
        command_option_id,
        "Custom Upscaler Command",
        "The command line Custom upscaling runs. {input} is replaced with a folder of PNG images and {output} with "
        "the folder to save the upscaled images in (any scale, file names starting like the input's). For example: "
        "\"C:\\path\\to\\upscaler.exe\" -i {input} -o {output} -s 2",
        ""
    );
    // Hidden while the mode is one of these: only Custom shows it.
    config.add_option_hidden_dependency(command_option_id, mode_option_id, Mode::Off, Mode::Esrgan2x, Mode::Esrgan4x);

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

void add_buttons(rush2::ui::OptionsPage* page) {
    using namespace recompui;
    ContextId context = get_current_context();
    std::string note = "Dump the textures you have driven past, upscale them with any program, then install the "
                       "results as a texture pack.";
    std::string status = upscaler.status();
    if (!status.empty()) {
        note += " " + status;
    }
    auto heading = page->add_heading("Your Own Upscales", note);

    Button* dump_button = context.create_element<Button>(heading.row, "Dump Textures", ButtonStyle::Secondary);
    dump_button->add_pressed_callback([dump_button]() {
        size_t count = upscaler.dump();
        dump_button->set_text("Dumped " + std::to_string(count));
        open_folder(dump_dir());
    });

    Button* install_button = context.create_element<Button>(heading.row, "Install Upscaled", ButtonStyle::Secondary);
    install_button->add_pressed_callback([install_button]() {
        int count = upscaler.install();
        if (count > 0) {
            install_button->set_text("Installed " + std::to_string(count));
        }
        else {
            install_button->set_text(count == 0 ? "Nothing to Install" : "Install Failed");
            std::error_code ec;
            std::filesystem::create_directories(upscaled_dir(), ec);
            open_folder(upscaled_dir());
        }
    });

    Button* folder_button = context.create_element<Button>(heading.row, "Open Folder", ButtonStyle::Secondary);
    folder_button->add_pressed_callback([]() {
        std::error_code ec;
        std::filesystem::create_directories(root(), ec);
        open_folder(root());
    });
}

}
