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
// Dreamcast Textures (src/rush2049dc/rush2049_dc_pack.cpp): with a Dreamcast Rush 2049 disc as the source, the
// textures it scaled down to fit TMEM are drawn from its texture pack of the disc's images, so they aren't upscaled
// here. With the N64 ROM as the source, each kept Rush 2049 texture (RGBA or CI) is first offered to
// rush2::rom2049::dc::match; one drawn with a disc image instead (a live replacement) isn't upscaled either. When the
// option or the source changes (rush2::rom2049::dc::texture_generation), those textures go back to being upscaled
// or matched again.
//
// For upscalers without a command line (Topaz Gigapixel's app, say): Dump Textures writes the kept textures as
// <key>.png into texture_upscale/dump/<game> (rush2, sfrush, rush2049: the game whose data the texture was loaded
// from, rush2::origin) and every disc image into dump/rush2049dc, the user saves upscaled copies (any size, file names
// starting with the key) into texture_upscale/upscaled/<the same folder>, and Install Upscaled turns those into the
// texture pack mod "rush2_custom_textures" (a folder in the mods folder, enabled right away) plus a shareable .rtz of
// it. The disc images' upscales go to the Dreamcast Textures instead (rush2::rom2049::dc::add_upscale), so they're
// drawn wherever the disc's images are, and only while the option is on. Dumps from before the per-game folders
// (files at the top of dump and upscaled) still install.

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
#include "texture_origin.h"
#include "texture_upscale.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace rush2::upscale {

const char* const mode_option_id = "texture_upscaling";
const char* const command_option_id = "texture_upscale_command";
const char* const dreamcast_option_id = "dreamcast_textures";

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
    constexpr const char* dc_folder = "rush2049dc";
    // The dump's folders: the top (dumps from before the per-game folders) and one per game.
    const std::vector<std::string>& dump_folders() {
        static const std::vector<std::string> folders = { "", "rush2", "sfrush", "rush2049", dc_folder };
        return folders;
    }

    using rush2::rom2049::dc::TextureMode;
    enum class DcState : uint8_t { Unknown, Pending, None, Matched };

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
        rush2::origin::Game origin = rush2::origin::Game::Rush2; // Where it was first loaded from.
        uint8_t fmt = 0;            // The tile's G_IM_FMT.
        DcState dc = DcState::Unknown; // N64 Dreamcast Textures: whether a disc image is drawn in its place.
        uint64_t dc_image = 0;
        bool disc_upscale = false;  // Upscales a matched disc image for the N64 textures in hashes.
    };

    // Whether an entry is offered to the N64 Dreamcast Textures match: a Rush 2049 texture.
    bool dc_candidate(const Entry& entry) {
        return entry.origin == rush2::origin::Game::Rush2049;
    }

    // An I or IA texture, which the draw tints.
    bool intensity(const Entry& entry) {
        return entry.fmt == 3 || entry.fmt == 4;
    }

    // The key of the entry that upscales a disc image matched to an N64 texture of its size.
    uint64_t dc_upscale_key(uint64_t image) {
        return image ^ 0xD15C0000D15C0000ull;
    }

    // An entry its disc image or a pending match keeps from being upscaled.
    bool dc_held(const Entry& entry) {
        return entry.dc == DcState::Pending || entry.dc == DcState::Matched;
    }

    // What the observer knows of an RT64 hash.
    struct Seen {
        uint64_t key = 0;
        uint64_t last_frame = UINT64_MAX;
        uint32_t frames = 0;
        bool ui = false;
        bool kept = false;          // Decoded already (key is 0 when it wasn't worth keeping).
        bool dc_packed = false;     // Left to the Dreamcast Textures' pack; seen again when it changes.
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
            // For tests: RUSH2_TEXTURE_DUMP=<seconds> presses Dump Textures that long after boot.
            if (const char* after = getenv("RUSH2_TEXTURE_DUMP")) {
                int seconds = atoi(after);
                std::thread([this, seconds]() {
                    std::this_thread::sleep_for(std::chrono::seconds(seconds));
                    start_dump();
                    while (dumping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    printf("[upscale] dumped %zu textures\n", dump_count.load());
                    fflush(stdout);
                }).detach();
            }
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
                entry.queued = (mode != Mode::Off) && !entry.ui && !dc_held(entry);
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
            if (dumping) {
                return "Dumping textures... " + std::to_string(dump_done.load());
            }
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
                          uint32_t tlut, bool world, uint64_t frame, uint32_t address) override {
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
            if (dc_mode == TextureMode::Disc && rush2::rom2049::dc::in_texture_pack(hash)) {
                seen.dc_packed = true;
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
                entry.origin = rush2::origin::at(address);
                entry.fmt = tile.fmt;
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
            if (!entry.ui && dc_mode == TextureMode::N64 && dc_candidate(entry)) {
                if (entry.dc == DcState::Unknown) {
                    entry.dc = DcState::Pending;
                    dc_pending.push_back(key);
                    dc_new = true;
                    changed.notify_all();
                }
                if (entry.dc == DcState::Matched) {
                    draw_dc(hash, entry.dc_image);
                    changed.notify_all();
                }
                if (dc_held(entry)) {
                    return;
                }
            }
            if (entry.ui || mode == Mode::Off || entry.failed || dc_held(entry)) {
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

        // Starts writing every kept 3D texture to its game's dump folder and every disc image to rush2049dc (skipping
        // ones already there), in the background: dumping() while it runs, then dumped() has how many there are.
        void start_dump() {
            if (dumping.exchange(true)) {
                return;
            }
            dump_done = 0;
            std::thread([this]() {
                size_t count = dump_kept();
                // The disc images in use: drawn from its pack, drawn at the disc's size already, or matched.
                std::vector<uint64_t> disc_hashes, disc_images;
                {
                    std::unique_lock lock(mutex);
                    for (const auto& [hash, seen] : seen_hashes) {
                        if (seen.dc_packed) disc_hashes.push_back(hash);
                    }
                    for (const auto& [key, entry] : entries) {
                        if (entry.origin == rush2::origin::Game::Rush2049DC && !entry.disc_upscale) {
                            disc_hashes.insert(disc_hashes.end(), entry.hashes.begin(), entry.hashes.end());
                        }
                        if (entry.dc == DcState::Matched) disc_images.push_back(entry.dc_image);
                    }
                }
                count += rush2::rom2049::dc::dump_images(dump_dir() / dc_folder, disc_hashes, disc_images, &dump_done);
                write_dump_readme();
                dump_count = count;
                dumping = false;
            }).detach();
        }

        bool is_dumping() const { return dumping; }
        size_t dumped() const { return dump_count; }

        size_t dump_kept() {
            std::vector<std::pair<uint64_t, Entry>> snapshot;
            {
                std::unique_lock lock(mutex);
                for (const auto& [key, entry] : entries) {
                    // A disc's own textures are dumped at their full size from its images.
                    if (!entry.ui && entry.origin != rush2::origin::Game::Rush2049DC) {
                        snapshot.emplace_back(key, entry);
                    }
                }
            }

            std::error_code ec;
            for (const std::string& folder : dump_folders()) {
                if (!folder.empty()) {
                    std::filesystem::create_directories(dump_dir() / folder, ec);
                    std::filesystem::create_directories(upscaled_dir() / folder, ec);
                }
            }
            size_t count = 0;
            for (int g = 0; g < rush2::origin::game_count; g++) {
                auto game = static_cast<rush2::origin::Game>(g);
                if (game == rush2::origin::Game::Rush2049DC) {
                    continue;
                }
                std::filesystem::path dir = dump_dir() / rush2::origin::folder(game);
                std::map<uint64_t, std::set<uint64_t>> index = read_dump_index(dir);
                for (const auto& [key, entry] : snapshot) {
                    if (entry.origin != game) {
                        continue;
                    }
                    std::filesystem::path path = dir / (key_name(key) + ".png");
                    if (!std::filesystem::exists(path, ec)) {
                        write_png(path, entry.image);
                    }
                    index[key].insert(entry.hashes.begin(), entry.hashes.end());
                    dump_done++;
                }
                if (!index.empty()) {
                    write_dump_index(dir, index);
                }
                count += index.size();
            }
            return count;
        }

        // Builds the texture pack mod from the upscaled folder. Returns how many textures it has, or -1 on failure.
        int install() {
            std::filesystem::path pack_dir = recomp::mods::get_mods_directory() / pack_id;
            std::filesystem::path texture_dir = pack_dir / "textures";
            std::error_code ec;
            std::filesystem::create_directories(texture_dir, ec);
            for (const auto& file : std::filesystem::directory_iterator(texture_dir, ec)) {
                std::filesystem::remove(file.path(), ec); // Textures no longer in the upscaled folder.
            }

            std::vector<PackTexture> textures;
            std::set<uint64_t> done;
            // The disc images' upscales go to the Dreamcast Textures, while they can tell their images.
            bool disc_images = rush2::rom2049::dc::texture_mode() != TextureMode::Off;
            std::vector<std::pair<uint64_t, std::filesystem::path>> disc_upscales;
            for (const std::string& folder : dump_folders()) {
                std::map<uint64_t, std::set<uint64_t>> index = read_dump_index(dump_dir() / folder);
                for (const auto& file : std::filesystem::directory_iterator(upscaled_dir() / folder, ec)) {
                    uint64_t key;
                    if (!file.is_regular_file() || !parse_key_name(path_utf8(file.path().filename()), key)) {
                        continue;
                    }
                    if (folder == dc_folder && disc_images && rush2::rom2049::dc::is_image(key)) {
                        disc_upscales.emplace_back(key, file.path());
                        continue;
                    }
                    auto it = index.find(key);
                    Image upscaled, original;
                    if (it == index.end() || done.count(key) || !read_image(file.path(), upscaled) ||
                        !read_image(dump_dir() / folder / (key_name(key) + ".png"), original)) {
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
            }
            int disc_count = 0;
            if (disc_images) {
                rush2::rom2049::dc::begin_upscales();
                for (const auto& [key, path] : disc_upscales) {
                    Image upscaled, original;
                    if (!read_image(path, upscaled) || !rush2::rom2049::dc::original_image(key, original)) {
                        continue;
                    }
                    restore_alpha(upscaled, original);
                    disc_count += rush2::rom2049::dc::add_upscale(key, upscaled) ? 1 : 0;
                }
                rush2::rom2049::dc::end_upscales();
            }
            if (textures.empty()) {
                return disc_count;
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
            return (int)textures.size() + disc_count;
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
        // Dreamcast Textures: the mode and generation last seen, how many kept 3D textures its pack has replaced (for
        // the log), entries waiting for match (dc_new: some since the worker last looked) and (hash, image) to draw.
        TextureMode dc_mode = TextureMode::Off;
        uint64_t dc_generation = 0;
        size_t source_packed = 0, source_logged = 0;
        std::deque<uint64_t> dc_pending;
        bool dc_new = false;
        std::vector<std::pair<uint64_t, uint64_t>> dc_applies;
        // Dump Textures, running in the background.
        std::atomic<bool> dumping = false;
        std::atomic<size_t> dump_done = 0, dump_count = 0;

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

        // Gives an entry back to the upscaler (mutex held): its upscale drawn again, or queued.
        void restore_upscale(uint64_t key, Entry& entry) {
            if (mode == Mode::Off || entry.ui || entry.failed) {
                return;
            }
            if (entry.ready) {
                for (uint64_t hash : entry.hashes) {
                    applies.emplace_back(hash, key);
                }
            }
            else if (!entry.queued) {
                entry.queued = true;
                pending.push_back(key);
            }
            changed.notify_all();
        }

        // The Dreamcast Textures changed (mutex held): disc images drawn so far come off, textures left to its pack
        // are seen again, and every texture is matched again or upscaled.
        void dc_reset(TextureMode new_mode) {
            dc_mode = new_mode;
            dc_pending.clear();
            dc_applies.clear();
            // The disc images' upscales are made again for whatever matches now.
            for (auto& [key, entry] : entries) {
                if (entry.disc_upscale) {
                    entry.hashes.clear();
                    entry.queued = false;
                }
            }
            for (auto& [key, entry] : entries) {
                bool held = dc_held(entry);
                if (entry.dc == DcState::Matched) {
                    removals.insert(removals.end(), entry.hashes.begin(), entry.hashes.end());
                }
                entry.dc = DcState::Unknown;
                entry.dc_image = 0;
                if (entry.ui) {
                    continue;
                }
                if (dc_mode == TextureMode::N64 && dc_candidate(entry)) {
                    entry.dc = DcState::Pending;
                    dc_pending.push_back(key);
                    dc_new = true;
                }
                else if (held) {
                    restore_upscale(key, entry);
                }
            }
            for (auto it = seen_hashes.begin(); it != seen_hashes.end();) {
                it = it->second.dc_packed ? seen_hashes.erase(it) : std::next(it);
            }
        }

        // Worker: asks rush2::rom2049::dc::match about the textures waiting for it.
        void run_matches() {
            std::vector<std::pair<uint64_t, Image>> todo;
            std::vector<bool> todo_intensity;
            uint64_t for_generation;
            {
                std::unique_lock lock(mutex);
                dc_new = false;
                for_generation = dc_generation;
                while (!dc_pending.empty() && todo.size() < 64) {
                    uint64_t key = dc_pending.front();
                    dc_pending.pop_front();
                    auto it = entries.find(key);
                    if (it != entries.end() && it->second.dc == DcState::Pending) {
                        todo.emplace_back(key, it->second.image);
                    todo_intensity.push_back(intensity(it->second));
                    }
                }
            }
            for (size_t i = 0; i < todo.size(); i++) {
                uint64_t image = 0;
                auto result = rush2::rom2049::dc::match(todo[i].second, todo[i].first, todo_intensity[i], image);
                // A disc image no larger than the texture is upscaled like it would have been (unless the player
                // upscaled it themselves): its own entry, made here, off the lock.
                Image disc;
                bool upscale_disc = result == rush2::rom2049::dc::Match::Found &&
                                    !rush2::rom2049::dc::upscaled_by_player(image) &&
                                    rush2::rom2049::dc::original_image(image, disc) &&
                                    disc.width <= todo[i].second.width && disc.height <= todo[i].second.height;
                std::unique_lock lock(mutex);
                if (for_generation != dc_generation) {
                    return; // dc_reset queued them all again.
                }
                if (result == rush2::rom2049::dc::Match::NotReady) {
                    // The disc's images are still being built: these wait for the next round.
                    for (size_t j = todo.size(); j-- > i;) {
                        dc_pending.push_front(todo[j].first);
                    }
                    return;
                }
                Entry& entry = entries[todo[i].first];
                if (entry.dc != DcState::Pending) {
                    continue;
                }
                if (result == rush2::rom2049::dc::Match::Found) {
                    entry.dc = DcState::Matched;
                    entry.dc_image = image;
                    if (upscale_disc) {
                        auto [it, inserted] = entries.try_emplace(dc_upscale_key(image));
                        Entry& up = it->second;
                        if (inserted) {
                            up.image = std::move(disc);
                            up.edge_s = entry.edge_s;
                            up.edge_t = entry.edge_t;
                            up.origin = rush2::origin::Game::Rush2049DC; // dumped from the disc's images instead
                            up.dc = DcState::None;
                            up.disc_upscale = true;
                        }
                        Entry& matched = entries[todo[i].first];
                        up.hashes.insert(up.hashes.end(), matched.hashes.begin(), matched.hashes.end());
                        restore_upscale(dc_upscale_key(image), up);
                    }
                    Entry& matched = entries[todo[i].first];
                    for (uint64_t hash : matched.hashes) {
                        draw_dc(hash, image);
                    }
                }
                else {
                    entry.dc = DcState::None;
                    restore_upscale(todo[i].first, entry);
                }
            }
        }

        // Draws hash with its disc image (mutex held): the upscale of it once there is one, else the image.
        void draw_dc(uint64_t hash, uint64_t image) {
            auto up = entries.find(dc_upscale_key(image));
            if (up != entries.end()) {
                if (std::find(up->second.hashes.begin(), up->second.hashes.end(), hash) == up->second.hashes.end()) {
                    up->second.hashes.push_back(hash);
                }
                if (mode != Mode::Off && up->second.ready) {
                    applies.emplace_back(hash, up->first);
                    return;
                }
            }
            dc_applies.emplace_back(hash, image);
        }

        // Worker: draws the textures with their disc images.
        void apply_dc(const std::vector<std::pair<uint64_t, uint64_t>>& todo) {
            for (const auto& [hash, image] : todo) {
                uint64_t content = rush2::rom2049::dc::image_content(image);
                std::vector<uint8_t> bytes;
                if (content == 0 ||
                    (!RT64::hasLiveReplacementContent(content) && !rush2::rom2049::dc::image_file(image, bytes, content))) {
                    continue;
                }
                RT64::addLiveReplacement(hash, content, bytes);
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

        std::map<uint64_t, std::set<uint64_t>> read_dump_index(const std::filesystem::path& dir) {
            std::map<uint64_t, std::set<uint64_t>> index;
            std::ifstream file{ dir / "hashes.txt" };
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

        void write_dump_index(const std::filesystem::path& dir, const std::map<uint64_t, std::set<uint64_t>>& index) {
            std::string text = "# <image key> <RT64 hashes it replaces>. Written by Dump Textures, read by Install Upscaled.\n";
            for (const auto& [key, hashes] : index) {
                text += key_name(key);
                for (uint64_t hash : hashes) {
                    text += " " + key_name(hash);
                }
                text += "\n";
            }
            write_file(dir / "hashes.txt", std::vector<uint8_t>(text.begin(), text.end()));
        }

        void write_dump_readme() {
            std::string text =
                "The game's textures, one PNG per texture, named by their contents, in a folder per game:\n"
                "  rush2       Rush 2's own textures\n"
                "  sfrush      the San Francisco Rush tracks'\n"
                "  rush2049    the Rush 2049 tracks' and cars' (from the N64 ROM)\n"
                "  rush2049dc  every texture of the Rush 2049 Dreamcast disc at its full size (with a disc installed\n"
                "              and Dreamcast Textures on)\n"
                "Only textures drawn in 3D are dumped; HUD, menu and font images are left out. Dump again after playing\n"
                "more tracks and cars to add theirs.\n"
                "\n"
                "1. Upscale these images with any program (Topaz Gigapixel, an image editor...).\n"
                "2. Save the results into the same folder under \"upscaled\" next to this one (upscaled/rush2049dc for\n"
                "   rush2049dc...). Any size works. Keep the first 16 characters of each file name: extra text after\n"
                "   them (\"_gigapixel-2x\") is fine. PNG keeps transparency; when an image comes back without it, the\n"
                "   original's is used.\n"
                "3. Press Install Upscaled in Settings > Graphics. The pack shows up in the mods menu as\n"
                "   \"Custom Upscaled Textures\", and texture_upscale/rush2_custom_textures.rtz is a copy to share.\n"
                "   The Dreamcast images' upscales aren't in it: they're drawn wherever the disc's images are (with the\n"
                "   disc as the source, or matching N64 textures), while Dreamcast Textures is on.\n";
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
                               !exact_applies.empty() || dc_new || !dc_applies.empty() ||
                               dc_generation != rush2::rom2049::dc::texture_generation() ||
                               (!pending.empty() && mode != Mode::Off);
                    });
                    uint64_t dc_now = rush2::rom2049::dc::texture_generation();
                    if (dc_now != dc_generation) {
                        dc_generation = dc_now;
                        dc_reset(rush2::rom2049::dc::texture_mode());
                    }
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
                    // That took the disc images off too.
                    std::unique_lock lock(mutex);
                    for (const auto& [key, entry] : entries) {
                        if (entry.dc == DcState::Matched) {
                            for (uint64_t hash : entry.hashes) {
                                draw_dc(hash, entry.dc_image);
                            }
                        }
                    }
                }
                for (uint64_t hash : to_remove) {
                    RT64::removeLiveReplacement(hash);
                }
                run_matches();
                std::vector<std::pair<uint64_t, uint64_t>> to_dc;
                {
                    std::unique_lock lock(mutex);
                    to_dc.swap(dc_applies);
                }
                apply_dc(to_dc);
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
                if (!entry.queued || entry.ui || entry.ready || dc_held(entry)) {
                    entry.queued = false;
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
                if (for_generation != generation || it == entries.end() || it->second.ui ||
                    it->second.dc == DcState::Matched) {
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

namespace {
    // Whether Dreamcast Textures is grayed out (set_dreamcast_option_state), and the config to show it in, once the
    // tabs are made.
    std::mutex dreamcast_state_mutex;
    bool dreamcast_disabled = true;
    recomp::config::Config* dreamcast_config = nullptr;
}

void add_options(recomp::config::Config& config) {
    config.add_bool_option(
        dreamcast_option_id,
        "Dreamcast Textures",
        "With the N64 ROM as the Rush 2049 source and a Rush 2049 Dreamcast disc installed too (Games tab), draws each "
        "N64 texture that is the same picture as a disc texture with the disc's image, up to four times the size and in "
        "full color; the rest stay as they are. Texture packs you install take priority, and upscaling handles "
        "everything the disc doesn't replace. Upscales of the disc's images (Your Own Upscales, rush2049dc) are drawn "
        "in their place.<br/><br/>Unavailable without a disc, and with the disc as the source, which always draws its "
        "own textures at full size. The first time, the disc's images take a minute or so to prepare in the "
        "background.",
        true
    );
    config.add_option_change_callback(dreamcast_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext) {
            rush2::rom2049::dc::set_textures_enabled(std::get<bool>(cur_value));
        });

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

void set_dreamcast_option_state(bool disc_source, bool disc_stored) {
    {
        std::lock_guard lock{ dreamcast_state_mutex };
        dreamcast_disabled = disc_source || !disc_stored;
    }
    if (dreamcast_config != nullptr) {
        dreamcast_config->update_option_disabled(dreamcast_option_id, dreamcast_disabled);
    }
}

void apply_loaded_options(recomp::config::Config& config) {
    dreamcast_config = &config;
    {
        std::lock_guard lock{ dreamcast_state_mutex };
        config.update_option_disabled(dreamcast_option_id, dreamcast_disabled);
    }
    rush2::rom2049::dc::set_textures_enabled(std::get<bool>(config.get_option_value(dreamcast_option_id)));
    apply_config(config.get_option_value(mode_option_id), config.get_option_value(command_option_id));
}

void add_exact_image(std::vector<uint8_t> indices, Image image) {
    upscaler.add_exact(std::move(indices), std::move(image));
}

void add_buttons(rush2::ui::OptionsPage* page) {
    using namespace recompui;
    ContextId context = get_current_context();
    const std::string note = "Dump the textures you have driven past (in a folder per game, and every Rush 2049 "
                             "Dreamcast texture), upscale them with any program (Topaz Gigapixel, say), save the "
                             "results in the same folder under upscaled, then install them.";
    auto heading = page->add_heading("Your Own Upscales", note + " " + upscaler.status());
    // The note follows the status (dumping, upscaling) while the page is open.
    auto shown = std::make_shared<std::string>(upscaler.status());
    Label* note_label = heading.note;

    // The buttons get a row of their own under the heading, so they keep their full size; it scrolls into view as
    // the controller reaches them.
    Element* row = page->add_row();

    Button* dump_button = context.create_element<Button>(row, "Dump Textures", ButtonStyle::Secondary);
    auto dump_started = std::make_shared<bool>(false);
    dump_button->add_pressed_callback([dump_button, dump_started]() {
        *dump_started = true;
        upscaler.start_dump();
        dump_button->set_text("Dumping...");
    });
    page->add_update_callback([dump_button, dump_started, shown, note_label, note]() {
        if (*dump_started && !upscaler.is_dumping()) {
            *dump_started = false;
            dump_button->set_text("Dumped " + std::to_string(upscaler.dumped()) + " Textures");
        }
        std::string status = upscaler.status();
        if (status != *shown && note_label != nullptr) {
            *shown = status;
            note_label->set_text(note + " " + status);
        }
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
