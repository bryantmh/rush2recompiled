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
// <app folder>/asset_dump/generated_texture_cache/<mode>/<key>.dds, so nothing is upscaled twice, across sessions too. Results
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
// For upscalers without a command line (Topaz Gigapixel's app, say): Dump Textures writes every texture of every
// model the game can draw, read from the ROMs and named after its model file and model (rush2::catalog,
// docs/asset_catalog.md), as asset_dump/textures/<game>/<file path>/<name>.png (rush2, sfrush, rush2049: the game
// whose data the texture comes from), the session's other 3D textures beside them (car paint jobs as
// <name>~<key>.png, the rest as unnamed/<key>.png), and the disc images in use into dump/rush2049dc by disc file and
// texture name (Rush 2049's N64 textures take the name of the disc image they match). names.txt maps
// each image to its content key, hashes.txt each key to the RT64 hashes it replaces (the catalog's, computed from the
// ROM, and the ones drawn). The user saves upscaled copies (any size, file names starting with the image's) into
// asset_dump/user_texture_pack/<the same path>, and Install Upscaled Textures turns those into the texture pack mod
// "rush2_custom_textures" (a folder in the mods folder, enabled right away) plus a shareable .rtz of it; a paint job
// without an upscale of its own gets its texture's, recolored. The disc images' upscales go to the Dreamcast Textures
// instead (rush2::rom2049::dc::add_upscale), so they're drawn wherever the disc's images are, and only while the
// option is on. At startup, on the worker thread, migrate_folders moves the older texture_upscale/ layout into
// asset_dump/ (Dump New Textures' originals back into the dump), and upscales named by key move to their names, in
// whichever game folder names them.

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
#include "librecomp/game.hpp"
#include "util/file.h"

#include "asset_catalog.h"
#include "options_page.h"
#include "rush2049_dc.h"
#include "texture_census.h"
#include "body_sheet.h"
#include "car1_stripes.h"
#include "texture_derive.h"
#include "texture_origin.h"
#include "texture_upscale.h"
#include "track_cache.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace rush2::upscale {

const char* const mode_option_id = "texture_upscaling";
const char* const command_option_id = "texture_upscale_command";
const char* const dreamcast_option_id = "dreamcast_textures";
const char* const interface_option_id = "upscale_interface";
const char* const car_colors_option_id = "keep_car_colors";

namespace {
    // Saved by name ("Hq2x"...), so the values only matter inside this file.
    enum class Mode : uint32_t { Off, Hq2x, Hq4x, Custom };

    constexpr uint32_t stable_frames = 3;  // Frames a texture is drawn in 3D before it's kept (skips one-frame textures).
    constexpr uint32_t border = 8;         // Texels of padding around images given to an upscaler.
    constexpr size_t batch_size = 16;      // Images per batch (one custom upscaler run).
    constexpr auto run_pause = std::chrono::milliseconds(250); // Between custom upscaler runs, to give the game room.
    const std::string pack_id = "rush2_custom_textures";
    // The installed pack, an .rtz in the mods folder (built from asset_dump/user_texture_pack).
    std::filesystem::path pack_path() { return recomp::mods::get_mods_directory() / (pack_id + ".rtz"); }

    std::string quoted(const std::filesystem::path& path) {
        return "\"" + path_utf8(path) + "\"";
    }

    // <app folder>/asset_dump (include/texture_upscale.h): the texture dump in textures/, the player's upscales in
    // user_texture_pack/, the upscaler's results in generated_texture_cache/.
    std::filesystem::path root() { return asset_dump_dir(); }
    std::filesystem::path dump_dir() { return root() / "textures"; }
    std::filesystem::path upscaled_dir() { return root() / "user_texture_pack"; }
    // Tiles a half-body sheet gives back all of (rush2::sheet::covers): out of the folders to upscale, kept for Install.
    std::filesystem::path covered_dir() { return root() / "sheet_tiles"; }
    std::filesystem::path work_dir() { return generated_cache_dir() / "work"; }
    std::filesystem::path ui_list() { return root() / "ui_textures.txt"; }
    constexpr const char* dc_folder = "rush2049dc";
    // The dump's folders: the top (dumps from before the per-game folders) and one per game.
    const std::vector<std::string>& dump_folders() {
        static const std::vector<std::string> folders = { "", "rush2", "sfrush", "rush2049", dc_folder };
        return folders;
    }

    using rush2::rom2049::dc::TextureMode;
    enum class DcState : uint8_t { Unknown, Pending, None, Matched };

    std::atomic<uint32_t> builtin_car_scale = 0; // builtin_scale(): set_mode.

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
        bool derived = false;       // A 3D draw handed to rush2::derive (at first sight).
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
            rush2::derive::start();
            // The layout migrations run on the worker before it upscales anything, never holding up the window (a
            // player doesn't know they are there). Dump and Install wait for them (wait_migrated).
            worker = std::thread([this]() {
                migrate_folders();
                migrate_names_on_disk();
                {
                    std::unique_lock lock(mutex);
                    load_ui_list();
                }
                // Left by the ESRGAN modes, which are gone: their caches.
                std::error_code ec;
                for (const char* folder : { "esrgan_x2", "esrgan_x4", "esrgan_x2_from_x4" }) {
                    std::filesystem::remove_all(generated_cache_dir() / folder, ec);
                }
                migrated = true;
                run();
            });
            worker.detach(); // Lives as long as the game; the process ends with it.
            RT64::ActiveTextureObserver = this;
            // For tests: RUSH2_TEXTURE_DUMP=<seconds> presses Dump Textures that long after boot, then Install
            // Upscaled Textures with RUSH2_TEXTURE_INSTALL=1. With RUSH2_TEXTURE_CENSUS it writes the census instead.
            if (const char* after = getenv("RUSH2_TEXTURE_DUMP")) {
                int seconds = atoi(after);
                bool then_install = getenv("RUSH2_TEXTURE_INSTALL") != nullptr;
                std::thread([this, seconds, then_install]() {
                    std::this_thread::sleep_for(std::chrono::seconds(seconds));
                    if (rush2::census::enabled()) {
                        rush2::census::write(); // A census run: no dump (it would be cut short when the test ends).
                        return;
                    }
                    start_dump();
                    while (dumping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    printf("[upscale] dumped %zu textures\n", dump_count.load());
                    if (then_install) {
                        start_install();
                        while (installing_flag) std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        printf("[upscale] installed %d textures\n", install_count.load());
                    }
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
            uint32_t scale = (mode == Mode::Hq2x || mode == Mode::Hq4x) ? mode_scale(mode) : 0;
            if (builtin_car_scale.exchange(scale) != scale) {
                rush2::derive::packs_changed(); // The cars' upscales come from here now, or no longer.
            }
            command = new_command;
            command_key = rush2::track_cache::hash(reinterpret_cast<const uint8_t*>(command.data()), command.size());
            if (mode == Mode::Custom) {
                // Builds before the per-command folders kept Custom results loose in custom/: they're most likely
                // from this command, so they move into its folder rather than being upscaled again.
                std::error_code ec;
                std::filesystem::path loose = generated_cache_dir() / mode_folder(Mode::Custom);
                std::filesystem::path folder = cache_folder(Mode::Custom);
                for (const auto& file : std::filesystem::directory_iterator(loose, ec)) {
                    if (file.is_regular_file() && file.path().extension() == ".dds") {
                        std::filesystem::create_directories(folder, ec);
                        std::filesystem::rename(file.path(), folder / file.path().filename(), ec);
                    }
                }
            }
            requeue_all();
        }

        // Upscale Interface: interface textures (drawn in 2D, or only in the catalog's interface containers) are
        // upscaled and dumped only with it on.
        void set_ui(bool enabled) {
            std::unique_lock lock(mutex);
            if (ui_upscaling == enabled) {
                return;
            }
            ui_upscaling = enabled;
            generation++;
            cancel_run = true;
            clear_requested = true;
            pending.clear();
            for (auto& [key, entry] : entries) {
                entry.queued = (mode != Mode::Off) && !skip_ui(entry) && !entry.ready && !entry.failed && !dc_held(entry);
                if (entry.queued) {
                    pending.push_back(key);
                }
                if (entry.ready && !skip_ui(entry)) {
                    for (uint64_t hash : entry.hashes) {
                        applies.emplace_back(hash, key);
                    }
                }
            }
            for (const auto& [hash, strip] : exact_strips) {
                exact_applies.push_back(hash);
            }
            changed.notify_all();
        }

        // Every texture upscaled anew for a new mode (mutex held).
        void requeue_all() {
            generation++;
            cancel_run = true; // Stops an upscaler that's running for the old mode.
            clear_requested = true;
            message.clear();
            pending.clear();
            for (auto& [key, entry] : entries) {
                entry.ready = false;
                entry.failed = false;
                entry.dependents.clear();
                entry.queued = (mode != Mode::Off) && !skip_ui(entry) && !dc_held(entry);
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

        // What Dump Textures is on (shown on its button), empty when it isn't running.
        std::string dump_status() {
            std::unique_lock lock(mutex);
            if (!dumping) {
                return "";
            }
            return !dump_phase.empty() ? dump_phase : "Dumping... " + std::to_string(dump_done.load());
        }

        // What the upscaling mode is doing, or its last problem (shown on the Texture Upscaling option), kept to one
        // short line.
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
                          uint32_t tlut, bool world, uint64_t frame, uint32_t address) override {
            std::unique_lock lock(mutex);
            Seen& seen = seen_hashes[hash];
            if (!world) {
                if (!seen.ui) {
                    seen.ui = true;
                    if (!exact_images.empty() && tlut != 0 && tile.siz == 1) {
                        find_exact(hash, tmem, tile, width, height, tlut);
                    }
                    // Packs replace 2D textures too; ones the game makes at run time (strips of a big image, other
                    // palettes) are derived from the catalog's (the upscaling modes leave 2D alone).
                    if (width <= 1024 && height <= 1024) {
                        std::vector<uint8_t> indices;
                        Image image = decode_tmem(tmem, tile.fmt, tile.siz, tile.tmem, tile.line, tile.palette, tlut,
                                                  width, height, tlut != 0 ? &indices : nullptr);
                        uint64_t drawn_index_key = tlut != 0 ? index_key(indices, width, height, tile.siz) : 0;
                        uint64_t key = content_key(image);
                        if (rush2::census::enabled()) {
                            rush2::census::record(hash, key, drawn_index_key, address, tile.fmt, tile.siz, tile.line,
                                                  tile.tmem, width, height, tlut, image);
                        }
                        rush2::derive::drawn(hash, key, drawn_index_key, image, std::move(indices), tile.siz, false);
                    }
                    if (seen.key != 0) {
                        mark_ui(seen.key);
                    }
                }
                if (!ui_upscaling) {
                    return;
                }
            }
            // The derived replacements (car paints, stamped stripes...) get a 3D texture the first time it's drawn,
            // not after stable_frames: waiting three frames for a repaint showed the native texture that long.
            if (world && !seen.derived) {
                seen.derived = true;
                if (width <= 1024 && height <= 1024 &&
                    !(dc_mode == TextureMode::Disc && rush2::rom2049::dc::in_texture_pack(hash))) {
                    std::vector<uint8_t> indices;
                    Image image = decode_tmem(tmem, tile.fmt, tile.siz, tile.tmem, tile.line, tile.palette, tlut,
                                              width, height, tlut != 0 ? &indices : nullptr);
                    uint64_t key = content_key(image);
                    const uint64_t drawn_index_key = tlut != 0 ? index_key(indices, width, height, tile.siz) : 0;
                    if (rush2::census::enabled()) {
                        rush2::census::record(hash, key, drawn_index_key, address, tile.fmt, tile.siz, tile.line,
                                              tile.tmem, width, height, tlut, image);
                    }
                    rush2::derive::drawn(hash, key, drawn_index_key, image, std::move(indices), tile.siz, true);
                }
            }
            if ((seen.ui && !ui_upscaling) || seen.kept || frame == seen.last_frame) {
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
            uint64_t key = content_key(image);
            if (!worth_upscaling(image)) {
                return;
            }
            seen.key = key;
            auto [it, inserted] = entries.try_emplace(key);
            Entry& entry = it->second;
            if (inserted) {
                entry.image = std::move(image);
                entry.edge_s = edge_of(tile.cms, tile.masks);
                entry.edge_t = edge_of(tile.cmt, tile.maskt);
                entry.ui = ui_keys.count(key) != 0 || !world;
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
            if (!world && !entry.ui) {
                mark_ui(key);
            }
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
            if (skip_ui(entry) || mode == Mode::Off || entry.failed || dc_held(entry)) {
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

        // Starts Dump Textures in the background: every texture of the catalog (the ROMs' own, rush2::catalog) into
        // its game's folder and every Dreamcast disc image into rush2049dc. dumping() while it runs, then dumped() has
        // how many there are. textures/ is made afresh each time: it only ever holds what the ROMs have.
        std::atomic<bool> migrated = false; // The startup migrations are done (start).
        void wait_migrated() {
            while (!migrated) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        void start_dump() {
            if (installing_flag || dumping.exchange(true)) {
                return;
            }
            dump_done = 0;
            std::thread([this]() {
                wait_migrated();
                size_t count = dump_named();
                std::vector<std::pair<std::string, uint64_t>> disc_named;
                bool interface;
                {
                    std::unique_lock lock(mutex);
                    interface = ui_upscaling;
                }
                count += rush2::rom2049::dc::dump_images(dump_dir() / dc_folder, interface, &dump_done, &disc_named);
                // The disc images are named by the disc's texture names; installs find them through names.txt.
                std::map<std::string, DumpName> names;
                for (const auto& [path, key] : disc_named) names[path] = { path, key };
                if (!names.empty()) {
                    migrate_key_names(dc_folder, { { dc_folder, names } });
                    write_dump_names(dump_dir() / dc_folder, names);
                }
                write_dump_readme();
                dump_count = count;
                dumping = false;
            }).detach();
        }

        bool is_dumping() const { return dumping; }
        size_t dumped() const { return dump_count; }

        // A named image of the dump (names.txt in each game folder): its path in the folder without ".png" and its
        // content key.
        struct DumpName {
            std::string path;
            uint64_t key = 0;
        };

        static std::vector<DumpName> read_dump_names(const std::filesystem::path& dir) {
            std::vector<DumpName> names;
            std::ifstream file{ dir / "names.txt" };
            std::string line;
            while (std::getline(file, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream words{ line };
                DumpName n;
                std::string key;
                if (!(words >> n.path >> key) || !parse_key_name(key, n.key)) continue;
                names.push_back(std::move(n));
            }
            return names;
        }

        static void write_dump_names(const std::filesystem::path& dir, const std::map<std::string, DumpName>& names) {
            std::string text = "# <image path> <image key>. Written by Dump Textures, read by Install Upscaled.\n";
            for (const auto& [path, n] : names) {
                text += n.path + " " + key_name(n.key) + "\n";
            }
            write_file(dir / "names.txt", std::vector<uint8_t>(text.begin(), text.end()));
        }

        // The image path (relative to `root`, '/' separated, no extension) of a file.
        static std::string relative_stem(const std::filesystem::path& file, const std::filesystem::path& root) {
            std::filesystem::path rel = file.lexically_relative(root);
            rel.replace_extension();
            std::u8string s = rel.generic_u8string();
            return std::string(s.begin(), s.end());
        }

        // Writes every texture of every model the game can draw (rush2::catalog, named after the models, the file
        // names unique in a game folder) and the RT64 hash the catalog gives each. Textures the game makes from these
        // at run time (other palettes, stamped stripes, copies) aren't dumped: their replacements are made from these
        // ones' upscales (rush2::derive). Upscales of the pushed builds' dumps, named by key, move to their names.
        // Where the dump puts a texture: its game folder and its path in it.
        struct Named {
            std::string folder, path;
        };
        // A 2049 car's container read with its triangles, and its textures' content keys.
        struct SheetJob {
            std::string path;
            rush2::catalog::Container container;
            std::vector<uint64_t> keys;
        };

        // The cars' half-body sheets (rush2::sheet), Rush 2's and Rush 2049's: each car's <path>/<CAR>_BODY.png, drawn
        // from the dumped images of its body's tiles, and each game folder's body_sheets.txt with their layouts, which
        // Install reads. A tile the car draws only where its sheet gives it back moves to sheet_tiles: upscaling it
        // alone would do nothing.
        void write_sheets(std::vector<SheetJob>& jobs, const std::unordered_map<uint64_t, Named>& named,
                          const std::unordered_map<uint64_t, Image>& images,
                          std::map<std::string, std::set<std::string>>& file_names) {
            std::map<std::string, std::vector<rush2::sheet::Sheet>> by_folder;
            std::map<std::pair<std::string, std::string>, uint64_t> any_key; // (Folder, path) -> key, for the _D1 tiles.
            for (const auto& [key, n] : named) any_key[{ n.folder, n.path }] = key;
            auto write = [&](const std::string& folder, const rush2::sheet::Sheet& sheet) {
                Image image = rush2::sheet::render(sheet, [&](const std::string& tile) -> const Image* {
                    auto k = any_key.find({ folder, tile });
                    if (k == any_key.end()) return nullptr;
                    auto im = images.find(k->second);
                    return im == images.end() ? nullptr : &im->second;
                });
                std::error_code ec;
                std::filesystem::path out = dump_dir() / folder / std::filesystem::u8path(sheet.path + ".png");
                std::filesystem::create_directories(out.parent_path(), ec);
                write_png(out, image);
                printf("[upscale] dump: %s, %zu pieces of %zu tiles, %ux%u\n", sheet.path.c_str(), sheet.pieces.size(),
                       rush2::sheet::tiles(sheet).size(), sheet.width, sheet.height);
            };
            size_t tile_count = 0, covered_count = 0;
            auto hide_covered = [&](const std::string& folder, const rush2::sheet::Sheet& sheet,
                                    const std::map<std::string, std::vector<uint8_t>>& drawn) {
                for (const std::string& tile : rush2::sheet::tiles(sheet)) {
                    tile_count++;
                    auto d = drawn.find(tile);
                    if (d == drawn.end() || !rush2::sheet::covers(sheet, tile, d->second)) continue;
                    std::error_code ec;
                    std::filesystem::path from = dump_dir() / folder / std::filesystem::u8path(tile + ".png");
                    std::filesystem::path to = covered_dir() / folder / std::filesystem::u8path(tile + ".png");
                    std::filesystem::create_directories(to.parent_path(), ec);
                    std::filesystem::rename(from, to, ec);
                    if (!ec) covered_count++;
                }
            };
            for (SheetJob& job : jobs) {
                std::vector<std::string> paths(job.keys.size());
                std::string folder;
                for (size_t i = 0; i < job.keys.size(); i++) {
                    auto it = named.find(job.keys[i]);
                    if (it == named.end()) continue;
                    if (folder.empty()) folder = it->second.folder;
                    if (it->second.folder != folder) continue; // The sheet's tiles are all in its game folder.
                    paths[i] = it->second.path;
                }
                if (folder.empty()) continue;
                std::string car = job.path.substr(job.path.rfind('/') + 1);
                std::string file = rush2::catalog::file_name(car) + "_BODY";
                std::set<std::string>& used = file_names[folder];
                for (int k = 2; used.count(file); k++) file = rush2::catalog::file_name(car) + "_BODY~" + std::to_string(k);
                rush2::sheet::Sheet sheet;
                std::map<std::string, std::vector<uint8_t>> drawn; // Tile -> the texels the car draws of it.
                if (!rush2::sheet::build(job.container, paths, job.path + "/" + file, sheet, &drawn)) continue;
                used.insert(file);
                write(folder, sheet);
                hide_covered(folder, sheet, drawn);
                // The damaged body (drawn once the car is banged up: Rush 2049's <tile>_D1, Rush 2's ...D1_n for
                // ...D0_n) has the same layout: its own sheet, <CAR>_BODY_D1, with the pieces whose tile has one
                // (glass and the like don't).
                auto damaged_tile = [&](const std::string& tile) -> std::string {
                    if (any_key.count({ folder, tile + "_D1" })) return tile + "_D1";
                    size_t at = tile.rfind("D0_");
                    if (at != std::string::npos && at > tile.rfind('/')) {
                        std::string d1 = tile.substr(0, at) + "D1_" + tile.substr(at + 3);
                        if (any_key.count({ folder, d1 })) return d1;
                    }
                    return "";
                };
                // Another version of the tiles laid out as `from` (version_of: a tile's version, "" for none): the sheet
                // <from's path><suffix> of the pieces whose tile has one. A version the car doesn't draw itself is
                // swapped in where its tile is drawn, so it draws what the tile does. Returns its drawn texels.
                auto variant = [&](const rush2::sheet::Sheet& from, const std::string& suffix,
                                   const std::function<std::string(const std::string&)>& version_of,
                                   const std::map<std::string, std::vector<uint8_t>>& drawn_from) {
                    rush2::sheet::Sheet v = from;
                    v.path += suffix;
                    v.pieces.clear();
                    std::map<std::string, std::vector<uint8_t>> drawn_v;
                    for (rush2::sheet::Piece piece : from.pieces) {
                        std::string base = piece.tile;
                        piece.tile = version_of(piece.tile);
                        if (piece.tile.empty()) continue;
                        const std::vector<uint8_t>* mask = nullptr;
                        if (auto d = drawn.find(piece.tile); d != drawn.end()) mask = &d->second;
                        else if (auto f = drawn_from.find(base); f != drawn_from.end()) mask = &f->second;
                        if (mask != nullptr) drawn_v.emplace(piece.tile, *mask);
                        v.pieces.push_back(std::move(piece));
                    }
                    std::string name = v.path.substr(v.path.rfind('/') + 1);
                    if (!v.pieces.empty() && !used.count(name)) {
                        used.insert(name);
                        write(folder, v);
                        hide_covered(folder, v, drawn_v);
                        by_folder[folder].push_back(std::move(v));
                    }
                    return drawn_v;
                };
                // Rush 2's damaged parts (D2_) draw their tiles themselves; Rush 2049 swaps <tile>_D1 in.
                variant(sheet, "_D1", damaged_tile, drawn);
                // The SF Rush decals alone (<tile>_SFRUSH, rush2::car1stripes::decal_textures), laid out as the body:
                // <CAR>_BODY_SFRUSH. The damaged panels draw the same decal.
                auto decal_tile = [&](const std::string& tile) -> std::string {
                    return any_key.count({ folder, tile + "_SFRUSH" }) ? tile + "_SFRUSH" : "";
                };
                variant(sheet, "_SFRUSH", decal_tile, drawn);
                by_folder[folder].push_back(std::move(sheet));
            }
            printf("[upscale] dump: %zu of %zu car tiles given back whole by their sheets, moved to sheet_tiles\n",
                   covered_count, tile_count);
            for (const auto& [folder, sheets] : by_folder) {
                std::string text = rush2::sheet::write_layout(sheets);
                write_file(dump_dir() / folder / "body_sheets.txt", std::vector<uint8_t>(text.begin(), text.end()));
            }
        }

        size_t dump_named() {
            std::error_code ec;
            std::filesystem::remove_all(dump_dir(), ec);
            std::filesystem::remove_all(covered_dir(), ec);
            std::unordered_map<uint64_t, Named> named;       // By content key.
            std::map<std::string, std::map<uint64_t, std::set<uint64_t>>> hashes; // Folder -> key -> RT64 hashes.
            std::map<std::string, std::map<std::string, DumpName>> names;          // Folder -> path -> name.
            std::unordered_map<uint64_t, Image> images;      // Catalog images to write.
            std::map<std::string, std::set<std::string>> file_names; // Folder -> file names given.

            std::vector<rush2::catalog::Entry> containers = rush2::catalog::containers([this](const std::string& s) {
                std::unique_lock lock(mutex);
                dump_phase = s;
            });
            bool interface;
            {
                std::unique_lock lock(mutex);
                interface = ui_upscaling;
            }
            {
                std::unique_lock lock(mutex);
                dump_phase = "Naming textures...";
            }
            // With a Dreamcast disc stored and Dreamcast Textures on, a Rush 2049 N64 texture matched to a disc image
            // takes the disc's texture name, which every texture has (docs/asset_catalog.md).
            const bool dc_names = rush2::rom2049::dc::texture_mode() == TextureMode::N64;
            if (dc_names || rush2::rom2049::dc::texture_mode() == TextureMode::Disc) {
                // The names (and the disc's folder) mustn't depend on how far the disc's images are built: a dump
                // made before they are ready would name textures otherwise, and upscales of it wouldn't install.
                {
                    std::unique_lock lock(mutex);
                    dump_phase = "Waiting for the Dreamcast images...";
                }
                auto until = std::chrono::steady_clock::now() + std::chrono::minutes(10);
                while (!rush2::rom2049::dc::images_ready() && std::chrono::steady_clock::now() < until) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
                if (!rush2::rom2049::dc::images_ready()) {
                    printf("[upscale] dump: the Dreamcast disc's images aren't ready; dumping without them\n");
                }
                {
                    std::unique_lock lock(mutex);
                    dump_phase = "Naming textures...";
                }
            }
            size_t matched = 0;
            // The cars' containers (with their triangles) and their textures' keys, for the half-body sheets.
            std::vector<SheetJob> sheet_jobs;
            for (const rush2::catalog::Entry& e : containers) {
                if (!interface && rush2::catalog::ui_path(e.path)) {
                    continue; // Interface art (Upscale Interface off); a texture also on a model elsewhere is named there.
                }
                rush2::catalog::Container c;
                std::string error;
                // Every car's triangles, for its sheet (Rush 2's and Rush 2049's; the paint and parts files aren't cars).
                const bool car = e.path.rfind("cars/", 0) == 0 && e.path != "cars/stripes" && e.path != "cars/parts";
                if (!rush2::catalog::read_container(*e.data, c, error, car, false, &e.copies)) {
                    continue;
                }
                std::vector<uint64_t> keys;
                for (const rush2::catalog::Texture& t : c.textures) keys.push_back(t.key);
                // Car paint bases are written in color (the paints violet, yellow and cyan), not as the catalog's gray,
                // so an upscale shows where each paint ends (rush2::catalog::paint_preview).
                const std::vector<uint8_t> paint_palette = e.path.rfind("cars/", 0) == 0
                    ? rush2::catalog::paint_preview_palette(containers, e) : std::vector<uint8_t>{};
                std::map<std::string, std::string> renamed; // Catalog name -> file name given, for the mipmap levels.
                for (rush2::catalog::Texture& t : c.textures) {
                    rush2::origin::Game game = e.game_at(t.texels);
                    if (game == rush2::origin::Game::Rush2049DC || !worth_upscaling(t.image) || named.count(t.key)) {
                        continue; // The disc's textures go to rush2049dc at their full size (dump_images).
                    }
                    const std::string folder = rush2::origin::folder(game);
                    std::string file;
                    size_t mip = t.name.find(".mip");
                    if (auto it = mip == std::string::npos ? renamed.end() : renamed.find(t.name.substr(0, mip));
                        it != renamed.end()) {
                        file = it->second + rush2::catalog::file_name(t.name.substr(mip)); // Follows its texture.
                    }
                    else {
                        std::string name = t.name;
                        if (dc_names && game == rush2::origin::Game::Rush2049 && mip == std::string::npos) {
                            uint64_t image_key = 0;
                            bool intensity = t.fmt == 3 || t.fmt == 4;
                            if (rush2::rom2049::dc::match(t.image, t.key, intensity, image_key) == rush2::rom2049::dc::Match::Found) {
                                std::string disc = rush2::rom2049::dc::image_name(image_key);
                                if (!disc.empty()) {
                                    name = disc;
                                    matched++;
                                }
                            }
                        }
                        // File names are unique in a game folder, so upscales still install after a tool such as
                        // Gigapixel writes them all into one folder: a name another model file already has for a
                        // different image takes this file's name ("SHEEN0~DM2"), one used twice in a file a number.
                        file = rush2::catalog::file_name(name);
                        std::set<std::string>& used = file_names[folder];
                        if (used.count(file) && !names[folder].count(e.path + "/" + file)) {
                            std::string container = e.path.substr(e.path.rfind('/') + 1);
                            file += "~" + rush2::catalog::file_name(container);
                        }
                        const std::string first = file;
                        for (int k = 2; used.count(file); k++) {
                            file = first + "~" + std::to_string(k);
                        }
                        if (mip == std::string::npos) renamed[t.name] = file;
                    }
                    file_names[folder].insert(file);
                    std::string path = e.path + "/" + file;
                    Named n{ folder, path };
                    auto& h = hashes[n.folder][t.key];
                    if (t.hash != 0) h.insert(t.hash);
                    names[n.folder][n.path] = { n.path, t.key };
                    named.emplace(t.key, std::move(n));
                    Image preview;
                    if (rush2::catalog::paint_base(e, t)) preview = rush2::catalog::paint_preview(t.indices, t.width, t.height, paint_palette);
                    images.emplace(t.key, preview.empty() ? std::move(t.image) : std::move(preview));
                }
                // The SF Rush decals: written into the panels at run time, so in no file. Each goes beside its panel
                // as <panel>_SFRUSH, the decal alone (transparent off it), as the stripe tiles are; the damaged
                // panel's is the same image.
                if (car) {
                    for (rush2::car1stripes::DecalTexture& dt :
                         rush2::car1stripes::decal_textures(e, c, containers, paint_palette)) {
                        auto it = named.find(c.textures[dt.texture].key);
                        if (it == named.end() || named.count(dt.key)) continue;
                        Named n{ it->second.folder, it->second.path + "_SFRUSH" };
                        file_names[n.folder].insert(n.path.substr(n.path.rfind('/') + 1));
                        names[n.folder][n.path] = { n.path, dt.key };
                        images.emplace(dt.key, std::move(dt.image));
                        named.emplace(dt.key, std::move(n));
                    }
                }
                if (car) sheet_jobs.push_back({ e.path, std::move(c), std::move(keys) });
            }
            {
                std::unique_lock lock(mutex);
                dump_phase.clear();
            }
            if (dc_names) {
                printf("[upscale] catalog: %zu Rush 2049 textures named from the Dreamcast disc\n", matched);
            }
            for (const auto& [key, n] : named) {
                std::filesystem::path file = dump_dir() / n.folder / std::filesystem::u8path(n.path + ".png");
                std::filesystem::create_directories(file.parent_path(), ec);
                write_png(file, images[key]);
                dump_done++;
            }
            printf("[upscale] catalog: %zu textures dumped\n", named.size());
            write_sheets(sheet_jobs, named, images, file_names);
            size_t count = 0;
            for (const std::string& folder : dump_folders()) {
                if (folder.empty() || folder == dc_folder) continue;
                std::filesystem::path dir = dump_dir() / folder;
                std::filesystem::create_directories(dir, ec);
                std::filesystem::create_directories(upscaled_dir() / folder, ec);
                write_dump_names(dir, names[folder]);
                write_dump_index(dir, hashes[folder]);
                count += names[folder].size();
            }
            // Upscales named by key (the pushed builds' dumps), in any game folder or at the top, move to their names.
            for (const std::string& folder : dump_folders()) {
                migrate_key_names(folder, names);
            }
            return count;
        }

        // Upscales named by key (the pushed builds' dumps: "<key><anything>.<ext>" at the top of a game folder of
        // user_texture_pack) move to their names (the rest of the file name after the key stays), in the game folder
        // of `all` (folder -> its names) that names the key, this folder first. An upscale identical to one already
        // there goes; one with no name in the catalog stays where it is.
        void migrate_key_names(const std::string& folder, const std::map<std::string, std::map<std::string, DumpName>>& all) {
            std::unordered_map<uint64_t, std::pair<std::string, std::string>> by_key; // key -> folder, path
            auto add = [&](const std::string& f, const std::map<std::string, DumpName>& names) {
                for (const auto& [path, n] : names) by_key.emplace(n.key, std::make_pair(f, n.path));
            };
            if (auto it = all.find(folder); it != all.end()) add(folder, it->second);
            for (const auto& [f, names] : all) add(f, names);
            std::error_code ec;
            std::filesystem::path dir = upscaled_dir() / folder;
            std::vector<std::filesystem::path> files;
            for (const auto& file : std::filesystem::directory_iterator(dir, ec)) {
                if (file.is_regular_file()) files.push_back(file.path());
            }
            for (const auto& file : files) {
                std::string name = path_utf8(file.filename());
                uint64_t key;
                if (!parse_key_name(name, key)) continue;
                auto it = by_key.find(key);
                if (it == by_key.end()) continue;
                std::filesystem::path target = upscaled_dir() / it->second.first /
                                               std::filesystem::u8path(it->second.second + name.substr(16));
                std::filesystem::create_directories(target.parent_path(), ec);
                if (!std::filesystem::exists(target, ec)) {
                    std::filesystem::rename(file, target, ec);
                }
                else if (read_file(file) == read_file(target)) {
                    std::filesystem::remove(file, ec); // Saved twice (the pushed builds filed some under two games).
                }
            }
        }

        // At startup: upscales named by key move to the names the last dump's names.txt gives them.
        void migrate_names_on_disk() {
            std::map<std::string, std::map<std::string, DumpName>> all;
            for (const std::string& folder : dump_folders()) {
                if (folder.empty()) continue;
                for (DumpName& n : read_dump_names(dump_dir() / folder)) all[folder].emplace(n.path, std::move(n));
            }
            if (all.empty()) {
                return;
            }
            for (const std::string& folder : dump_folders()) {
                migrate_key_names(folder, all);
            }
        }

        // The dumped original of an image (the dump holds every one).
        bool read_original(const std::string& folder, const std::string& path, Image& original) {
            // A tile its car's sheet gives back whole is in sheet_tiles (write_sheets).
            return read_image(dump_dir() / folder / std::filesystem::u8path(path + ".png"), original) ||
                   read_image(covered_dir() / folder / std::filesystem::u8path(path + ".png"), original);
        }

        // Runs Install Upscaled in the background (decoding every upscale and writing the pack takes seconds), to the
        // end whether or not the settings page stays open: installing() while it runs, then installed() has how many
        // textures the pack has (-1 on failure).
        void start_install() {
            if (dumping || installing_flag.exchange(true)) {
                return;
            }
            std::thread([this]() {
                wait_migrated();
                install_error.clear();
                install_count = install();
                installing_flag = false;
            }).detach();
        }

        bool installing() const { return installing_flag; }
        int installed() const { return install_count; }
        // Why the last install failed, for the button ("" when it didn't).
        const std::string& install_failure() const { return install_error; }

    private:
        // Puts new_file in place of the installed pack (or, with new_file empty, deletes it). Both the mod context and
        // RT64 (which reads replacements from the .rtz while it's enabled) keep it open, and Windows won't replace an
        // open file: RT64 is told to drop it (secondary_disable_texture_pack, as the mods menu's toggle does; disabling
        // the mod only takes effect at the next game start) and does so on the render thread, the mods are closed, and
        // the swap is retried for a few seconds while RT64 lets go. Then RT64 may take it back (the caller rescans the
        // mods and enables it).
        bool replace_pack(const std::filesystem::path& new_file, const std::filesystem::path& pack_file) {
            std::error_code ec;
            recompui::renderer::secondary_disable_texture_pack(pack_id);
            recomp::mods::close_mods();
            // The derived replacements read car upscales out of it while they're made.
            std::unique_lock<std::mutex> hold = rush2::derive::hold_packs();
            // The loose folder older builds installed (the same mod id): the .rtz takes its place.
            std::filesystem::path loose_pack = recomp::mods::get_mods_directory() / pack_id;
            if (std::filesystem::exists(loose_pack / "mod.json", ec)) {
                std::filesystem::remove_all(loose_pack, ec);
            }
            bool ok = false;
            for (int attempt = 0; attempt < 200 && !ok; attempt++) {
                ec.clear();
                if (new_file.empty()) std::filesystem::remove(pack_file, ec);
                else std::filesystem::rename(new_file, pack_file, ec);
                ok = !ec;
                if (!ok) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!ok) {
                printf("[upscale] Couldn't replace %s: %s\n", path_utf8(pack_file).c_str(), ec.message().c_str());
                install_error = "the old pack is in use";
            }
            recompui::renderer::secondary_enable_texture_pack(pack_id);
            return ok;
        }

        std::atomic<bool> installing_flag = false;
        std::atomic<int> install_count = 0;
        std::string install_error; // Written by the install thread before installing_flag clears.

        // Install's half-body sheets for one game folder (its body_sheets.txt): each upscale of a sheet among files
        // (named after the sheet's path or file name, as any upscale) is cut into its tiles (add: install one), with the
        // tile's own upscale, if any, filling what the sheet doesn't give back. Returns the sheet upscales found.
        template <typename NameOf, typename Add>
        std::set<std::filesystem::path> install_sheets(const std::string& folder, const std::filesystem::path& root,
                                                       const std::vector<std::filesystem::path>& files,
                                                       const std::map<std::string, DumpName>& names, NameOf& name_of,
                                                       const std::set<uint64_t>& done, Add& add) {
            std::set<std::filesystem::path> found;
            std::vector<uint8_t> text = read_file(dump_dir() / folder / "body_sheets.txt");
            if (text.empty()) return found;
            std::vector<rush2::sheet::Sheet> sheets = rush2::sheet::read_layout(std::string(text.begin(), text.end()));
            auto sheet_of = [&](const std::string& stem) -> const rush2::sheet::Sheet* {
                const rush2::sheet::Sheet* best = nullptr;
                size_t best_length = 0;
                std::string file_stem = stem.substr(stem.rfind('/') + 1);
                for (const rush2::sheet::Sheet& sh : sheets) {
                    std::string file = sh.path.substr(sh.path.rfind('/') + 1);
                    size_t length = stem.rfind(sh.path, 0) == 0 ? sh.path.size() : file_stem.rfind(file, 0) == 0 ? file.size() : 0;
                    if (length > best_length) {
                        best = &sh;
                        best_length = length;
                    }
                }
                return best;
            };
            std::vector<std::pair<const rush2::sheet::Sheet*, std::filesystem::path>> sheet_upscales;
            std::map<std::string, std::filesystem::path> tile_upscales; // Dumped path -> its own upscale.
            for (const std::filesystem::path& file : files) {
                std::string stem = relative_stem(file, root);
                if (const rush2::sheet::Sheet* sh = sheet_of(stem)) {
                    sheet_upscales.emplace_back(sh, file);
                    found.insert(file);
                }
                else if (const DumpName* n = name_of(stem)) {
                    tile_upscales.emplace(n->path, file);
                }
            }
            for (const auto& [sh, file] : sheet_upscales) {
                Image up;
                if (sh->width == 0 || !read_image(file, up)) continue;
                uint32_t scale = std::max<uint32_t>(1, (up.width + sh->width / 2) / sh->width);
                if (up.width != sh->width * scale || up.height != sh->height * scale) {
                    up = resize_bilinear(up, sh->width * scale, sh->height * scale);
                }
                for (const std::string& tile : rush2::sheet::tiles(*sh)) {
                    auto n = names.find(tile);
                    Image original, fallback, cut;
                    if (n == names.end() || done.count(n->second.key) || !read_original(folder, tile, original)) continue;
                    if (auto own = tile_upscales.find(tile); own != tile_upscales.end()) read_image(own->second, fallback);
                    if (rush2::sheet::cut(*sh, up, tile, original, fallback, cut)) add(n->second.key, tile, std::move(cut), original);
                }
            }
            return found;
        }

        // Builds the texture pack mod from user_texture_pack as mods/rush2_custom_textures.rtz and enables it (on the
        // install thread: the mod calls lock the mod context, the texture pack ones are queued for the renderer and
        // update_mod_list opens the UI context). The pack is built in memory and swapped in with the mods closed, as
        // the mod installer does: the mod context keeps each .rtz open, which on Windows blocks replacing it. Returns
        // how many textures it has, or -1 on failure.
        int install() {
            std::filesystem::path pack_file = pack_path();
            std::error_code ec;
            ZipWriter zip;

            std::vector<PackTexture> textures;
            std::set<uint64_t> done;
            // The disc images' upscales go to the Dreamcast Textures, while they can tell their images.
            bool disc_images = rush2::rom2049::dc::texture_mode() != TextureMode::Off;
            std::vector<std::pair<uint64_t, std::filesystem::path>> disc_upscales;
            for (const std::string& folder : dump_folders()) {
                std::map<uint64_t, std::set<uint64_t>> index = read_dump_index(dump_dir() / folder);
                // Names: an upscale is "<dumped path><anything>.<ext>"; the longest dumped path it starts with wins.
                std::map<std::string, DumpName> names;
                for (DumpName& n : read_dump_names(dump_dir() / folder)) names.emplace(n.path, std::move(n));
                auto longest = [](const std::map<std::string, const DumpName*>& m, const std::string& stem) -> const DumpName* {
                    auto it = m.upper_bound(stem);
                    while (it != m.begin()) {
                        --it;
                        if (stem.rfind(it->first, 0) == 0) return it->second;
                        if (it->first.compare(0, 1, stem, 0, 1) != 0) break;
                    }
                    return nullptr;
                };
                // By the path in the game folder, else by the file name alone (file names are unique in a game folder):
                // an upscaler that writes everything into one folder loses the model folders.
                std::map<std::string, const DumpName*> by_path, by_file;
                for (const auto& [path, n] : names) {
                    by_path.emplace(path, &n);
                    by_file.emplace(path.substr(path.rfind('/') + 1), &n);
                }
                auto name_of = [&](const std::string& stem) -> const DumpName* {
                    if (const DumpName* n = longest(by_path, stem)) return n;
                    return longest(by_file, stem.substr(stem.rfind('/') + 1));
                };
                std::filesystem::path root = upscaled_dir() / folder;
                auto add = [&](uint64_t key, const std::string& path, Image upscaled, const Image& original) {
                    // Listed under its RT64 hashes and its content key too: the key is no hash RT64 draws, but it
                    // finds the image for the textures derived from it (rush2::derive), a paint texture's gray
                    // original included, which has no hash of its own.
                    std::vector<uint64_t> hashes{ key };
                    if (auto it = index.find(key); it != index.end()) hashes.insert(hashes.end(), it->second.begin(), it->second.end());
                    restore_alpha(upscaled, original);
                    std::string name = "textures/" + (folder.empty() ? "" : folder + "/") + path + ".dds";
                    if (!zip.add(name, make_dds(upscaled))) {
                        return false;
                    }
                    done.insert(key);
                    textures.push_back({ name, std::move(hashes) });
                    return true;
                };
                // Only the top of the old flat folders; the game folders are walked whole.
                std::vector<std::filesystem::path> files;
                if (folder.empty()) {
                    for (const auto& file : std::filesystem::directory_iterator(root, ec)) {
                        if (file.is_regular_file()) files.push_back(file.path());
                    }
                }
                else {
                    for (const auto& file : std::filesystem::recursive_directory_iterator(root, ec)) {
                        if (file.is_regular_file()) files.push_back(file.path());
                    }
                }
                // Half-body sheets (rush2::sheet): an upscale of one is cut into its tiles' upscales, pixel for pixel,
                // before the files one by one; texels the sheet doesn't give back come from the tile's own upscale.
                std::set<std::filesystem::path> sheet_files = install_sheets(folder, root, files, names, name_of, done, add);
                for (const std::filesystem::path& file : files) {
                    if (sheet_files.count(file)) continue;
                    std::string stem = relative_stem(file, root);
                    uint64_t key;
                    std::string path;
                    if (const DumpName* n = name_of(stem)) {
                        key = n->key;
                        path = n->path;
                    }
                    else if (parse_key_name(path_utf8(file.filename()), key)) {
                        path = key_name(key);
                    }
                    else {
                        continue;
                    }
                    if (folder == dc_folder && disc_images && rush2::rom2049::dc::is_image(key)) {
                        disc_upscales.emplace_back(key, file);
                        continue;
                    }
                    // Only images of the dump: what the game makes from them at run time (other palettes, stamped
                    // stripes...) is derived from their upscales as it's drawn (rush2::derive).
                    Image upscaled, original;
                    if (done.count(key) || !read_image(file, upscaled) || !read_original(folder, path, original)) {
                        continue;
                    }
                    add(key, path, std::move(upscaled), original);
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
                // Nothing to install: a pack installed earlier goes, as its upscales are gone.
                if (std::filesystem::exists(pack_file, ec)) {
                    replace_pack({}, pack_file);
                    recomp::mods::scan_mods();
                    recompui::update_mod_list(false);
                    recompui::renderer::trigger_texture_pack_update();
                }
                return disc_count;
            }

            std::string database = pack_database(textures);
            std::string manifest = pack_manifest(pack_id, "Custom Upscaled Textures",
                "Textures you upscaled from Settings > Graphics > Dump Textures, installed with Install Upscaled "
                "Textures from asset_dump/user_texture_pack.");
            std::filesystem::path new_file = pack_file;
            new_file += ".new";
            if (!zip.add("rt64.json", std::vector<uint8_t>(database.begin(), database.end())) ||
                !zip.add("mod.json", std::vector<uint8_t>(manifest.begin(), manifest.end())) || !zip.write(new_file)) {
                printf("[upscale] Couldn't write %s\n", path_utf8(new_file).c_str());
                install_error = "couldn't write the pack";
                std::filesystem::remove(new_file, ec);
                return -1;
            }
            if (!replace_pack(new_file, pack_file)) {
                // The old pack stays, switched back on.
                std::filesystem::remove(new_file, ec);
                recomp::mods::scan_mods();
                recomp::mods::enable_mod(pack_id, true);
                recompui::renderer::trigger_texture_pack_update();
                return -1;
            }

            recomp::mods::scan_mods();
            recomp::mods::enable_mod(pack_id, true);
            recompui::update_mod_list(false);
            recompui::renderer::trigger_texture_pack_update(); // Picks the new files up if it was enabled already.
            rush2::derive::packs_changed();
            return (int)textures.size() + disc_count;
        }

        std::mutex mutex;
        std::condition_variable changed;
        std::thread worker;
        std::unordered_map<uint64_t, Seen> seen_hashes;
        std::unordered_map<uint64_t, Entry> entries;
        std::unordered_map<uint64_t, uint64_t> bases;         // Index key -> content key of the variant upscaled.
        std::unordered_set<uint64_t> ui_keys;
        bool ui_upscaling = false;                            // Upscale Interface.
        std::deque<uint64_t> pending;
        std::vector<std::pair<uint64_t, uint64_t>> applies;   // (hash, key) for the worker to replace.
        std::vector<uint64_t> removals;                       // Hashes whose replacement goes.
        std::vector<uint64_t> new_ui_keys;                    // To append to the UI list.
        Mode mode = Mode::Off;
        std::string command;
        std::atomic<uint64_t> command_key = 0; // Hash of command: Custom's cache folder, so a new command upscales anew.
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
        std::string dump_phase; // What the dump is on before it writes images (mutex held).

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
            if (mode == Mode::Off || skip_ui(entry) || entry.failed) {
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
        // Whether an entry is interface art left alone (Upscale Interface off). Mutex held.
        bool skip_ui(const Entry& entry) const {
            return entry.ui && !ui_upscaling;
        }

        // Whether take_jobs can tell interface art: always with Upscale Interface on, else once the catalog is read.
        bool catalog_known() const {
            return ui_upscaling || rush2::derive::index_ready();
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
            if (!ui_upscaling) {
                removals.insert(removals.end(), it->second.hashes.begin(), it->second.hashes.end());
            }
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
                "The game's textures, one PNG per texture, in a folder per game:\n"
                "  rush2       Rush 2's own textures\n"
                "  sfrush      the San Francisco Rush tracks'\n"
                "  rush2049    the Rush 2049 tracks' and cars' (from the N64 ROM)\n"
                "  rush2049dc  the Rush 2049 Dreamcast disc's textures at their full size, by disc file and\n"
                "              texture name (with a disc installed)\n"
                "Inside a game folder, textures are named after the file and model they belong to (Rush 2049's by\n"
                "the Dreamcast disc's texture names, when a disc is installed and Dreamcast Textures is on):\n"
                "  cars/PICKUP/TRK_D0_1.png       a texture with its own name in the game files\n"
                "  tracks/ALCATRAZ/ALCATRAZL3136.2.png   the second texture model ALCATRAZL3136 brings\n"
                "  ....mip2.png                   a smaller copy the game draws in the distance (mipmap)\n"
                "Every texture of every track and car is here, read from the game files. What the game makes from\n"
                "them as you play uses your upscales of these: car paint jobs, stripes stamped on cars, signs it\n"
                "copies. names.txt and hashes.txt are what Install Upscaled Textures reads: leave them be.\n"
                "Car bodies are shown painted violet (MAIN COLOR), yellow (ACCENT COLOR) and, on the Rush 2049 cars,\n"
                "cyan (STRIPE COLOR): the game repaints those in the player's colors, keeping your shading. Keep them\n"
                "apart and their edges where you want them; the other colors (glass, lights, trim) are drawn as they\n"
                "are.\n"
                "A car's body is drawn from several tiles (BMD0_1..., C13_64_1...). Each car's folder also has the whole\n"
                "side of the car as one image, <CAR>_BODY.png (and <CAR>_BODY_D1.png, the damaged body): upscale that\n"
                "instead of the tiles, so the upscale runs smoothly across their seams, and save it under its own name\n"
                "like any other. Install cuts it back into the tiles. The blurry fill between the parts is never drawn.\n"
                "Keep the image's size ratio (2x, 4x...). Tiles the sheet covers whole are not in this folder (they're\n"
                "in ../sheet_tiles, for Install): the ones still here have parts the sheet doesn't show.\n"
                "With the San Francisco Rush ROM, the cars with an SF RUSH stripe also have <CAR>_BODY_SFRUSH.png: that\n"
                "decal alone, laid out as the body and see-through elsewhere, like the stripes in cars/stripes. It is\n"
                "drawn over the body's upscale; cyan marks a decal drawn in the STRIPE COLOR (only its shape counts).\n"
                "\n"
                "1. Upscale these images with any program (Topaz Gigapixel, an image editor...).\n"
                "2. Save the results into the same place under \"user_texture_pack\" next to this folder\n"
                "   (user_texture_pack/rush2/cars/PICKUP/TRK_D0_1.png...). Any size works. Keep each file's name:\n"
                "   extra text after it (\"-gigapixel-2x\") is fine. PNG keeps transparency; when an image comes back\n"
                "   without it, the original's is used. The folders inside a game folder are optional: every file\n"
                "   name is unique within its game, so you can upscale all of textures/rush2 (subfolders included)\n"
                "   and save the results straight into user_texture_pack/rush2.\n"
                "3. Press Install Upscaled Textures in Settings > Graphics. The pack shows up in the mods menu as\n"
                "   \"Custom Upscaled Textures\". It is mods/rush2_custom_textures.rtz, the file to share.\n"
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
                               (!pending.empty() && mode != Mode::Off && catalog_known());
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
                    rush2::derive::packs_changed(); // That took the derived replacements off: they are made again.
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
            if (!catalog_known()) {
                return; // Until the catalog says which textures are interface art.
            }
            while (!pending.empty() && mode != Mode::Off && upscales.size() < batch_size && recolors.size() < 256) {
                uint64_t key = pending.front();
                pending.pop_front();
                Entry& entry = entries[key];
                if (rush2::derive::mod_texture(entry.index_key, key)) {
                    entry.queued = false; // A mod's stand-in: replaced with the mod's own image (rush2::derive).
                    continue;
                }
                if (!entry.ui && rush2::derive::interface_texture(key, entry.index_key)) {
                    entry.ui = true; // Only in the catalog's interface containers (drawn in 3D: the race HUD).
                }
                if (rush2::derive::builtin_car(key, entry.index_key)) {
                    entry.queued = false; // A car texture: the derived replacements upscale its base (builtin_scale).
                    continue;
                }
                if (!entry.queued || skip_ui(entry) || entry.ready || dc_held(entry)) {
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
                    if (base != nullptr && !base->failed && !skip_ui(*base)) {
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

        // Custom results go in a folder per command line (custom/<hash>).
        std::filesystem::path cache_folder(Mode for_mode) {
            std::filesystem::path folder = generated_cache_dir() / mode_folder(for_mode);
            return for_mode == Mode::Custom ? folder / key_name(command_key) : folder;
        }

        std::filesystem::path cache_path(Mode for_mode, uint64_t key) {
            return cache_folder(for_mode) / (key_name(key) + ".dds");
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
                if (for_generation != generation || it == entries.end() || skip_ui(it->second) ||
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
                if (rush2::derive::owns(hash)) continue; // Made from an installed pack's upscale, which wins.
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
                set_message(for_generation, "The command needs {input} and {output}");
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
                set_message(for_generation, "The upscaler made no images (exit code " + std::to_string(result) + ")");
            }
            else if (replaced > 0) {
                set_message(for_generation, "Some upscales didn't match: HQ2x used for those");
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

    void apply_interface(bool enabled) {
        std::call_once(started, []() { upscaler.start(); });
        upscaler.set_ui(enabled);
        rush2::derive::set_ui(enabled);
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
        "texture is only upscaled once. Car paint jobs reuse one upscale. The HUD, menus and text keep their "
        "original look unless Upscale Interface is on. Texture packs you install take priority."
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

    config.add_bool_option(
        interface_option_id,
        "Upscale Interface",
        "Also upscales the menus, HUD, maps and text, and has Dump Textures write them for your own upscales. Off, "
        "they keep their original look and aren't dumped. Takes effect right away; dump again after changing it.",
        false
    );
    config.add_option_change_callback(interface_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
            if (context != recomp::config::OptionChangeContext::Temporary) {
                apply_interface(std::get<bool>(cur_value));
            }
        });

    config.add_bool_option(
        car_colors_option_id,
        "Keep Car Colors",
        "Your upscales of car bodies keep only the colors the original has in each spot. Upscalers can spread the "
        "dump's strong paint colors into small parts beside them, such as green or pink headlights; this takes that "
        "out and keeps your detail. Turn it off if you recolor lights or trim on purpose. Takes effect right away.",
        true
    );
    config.add_option_change_callback(car_colors_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
            if (context != recomp::config::OptionChangeContext::Temporary) {
                rush2::derive::set_keep_car_colors(std::get<bool>(cur_value));
            }
        });

    // The config can move as later tabs are added, so the callbacks look it up again. The graphics tab applies changes
    // as they're made; the Temporary check only matters if it ever stages them again.
    config.add_option_change_callback(mode_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
            if (context != recomp::config::OptionChangeContext::Temporary) {
                apply_config(cur_value, recompui::config::get_graphics_config().get_option_value(command_option_id));
            }
        });
    config.add_option_change_callback(command_option_id,
        [](recomp::config::ConfigValueVariant cur_value, recomp::config::ConfigValueVariant, recomp::config::OptionChangeContext context) {
            if (context != recomp::config::OptionChangeContext::Temporary) {
                apply_config(recompui::config::get_graphics_config().get_option_value(mode_option_id), cur_value);
            }
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
    apply_interface(std::get<bool>(config.get_option_value(interface_option_id)));
    rush2::derive::set_keep_car_colors(std::get<bool>(config.get_option_value(car_colors_option_id)));
    apply_config(config.get_option_value(mode_option_id), config.get_option_value(command_option_id));
}

uint32_t builtin_scale() {
    return builtin_car_scale;
}

std::filesystem::path asset_dump_dir() {
    return recomp::get_config_path() / "asset_dump";
}

std::filesystem::path generated_cache_dir() {
    return asset_dump_dir() / "generated_texture_cache";
}

namespace {
    // Moves folder `from` to `to`: a rename when `to` doesn't exist, else file by file. A file `to` already has stays
    // where it was, unless drop_duplicates (the same dumped original twice), when it's deleted. Folders left empty go.
    void move_tree(const std::filesystem::path& from, const std::filesystem::path& to, bool drop_duplicates) {
        std::error_code ec;
        if (!std::filesystem::is_directory(from, ec)) {
            return;
        }
        std::filesystem::create_directories(to.parent_path(), ec);
        if (!std::filesystem::exists(to, ec)) {
            std::filesystem::rename(from, to, ec);
            if (!ec) return;
            ec.clear();
        }
        std::vector<std::filesystem::path> files, dirs;
        for (auto it = std::filesystem::recursive_directory_iterator(from, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_directory(ec)) dirs.push_back(it->path());
            else files.push_back(it->path());
        }
        for (const auto& file : files) {
            std::filesystem::path target = to / std::filesystem::relative(file, from, ec);
            std::filesystem::create_directories(target.parent_path(), ec);
            if (!std::filesystem::exists(target, ec)) {
                std::filesystem::rename(file, target, ec);
            }
            else if (drop_duplicates) {
                std::filesystem::remove(file, ec);
            }
        }
        // Deepest first, so a parent is empty by the time it's tried; remove() leaves folders that still hold files.
        std::sort(dirs.begin(), dirs.end(), [](const auto& a, const auto& b) { return a.native().size() > b.native().size(); });
        for (const auto& dir : dirs) std::filesystem::remove(dir, ec);
        std::filesystem::remove(from, ec);
    }
}

void migrate_folders() {
    std::error_code ec;
    std::filesystem::path config = recomp::get_config_path(), old = config / "texture_upscale";
    std::filesystem::path dump = asset_dump_dir(), cache = generated_cache_dir();
    if (std::filesystem::is_directory(old, ec)) {
        printf("[upscale] Moving texture_upscale into asset_dump\n");
        move_tree(old / "dump", dump / "textures", true);
        // Dump New Textures' originals: a dump always holds every image now, so they go back into it.
        move_tree(old / "originals", dump / "textures", true);
        move_tree(old / "upscaled", dump / "user_texture_pack", false);
        move_tree(old / "cache", cache, true);
        move_tree(old / "dreamcast", cache / "dreamcast", true);
        if (!std::filesystem::exists(dump / "ui_textures.txt", ec)) {
            std::filesystem::rename(old / "ui_textures.txt", dump / "ui_textures.txt", ec);
        }
        std::filesystem::remove(old / (pack_id + ".rtz"), ec); // The share copy: the installed pack is an .rtz now.
        // Left over: the Custom upscaler's work files, the ESRGAN modes' program (both made again when needed).
        for (const char* name : { "work", "realesrgan" }) std::filesystem::remove_all(old / name, ec);
        std::filesystem::remove(old, ec); // Only once empty: anything else stays where it was.
    }
    // Install used to leave the pack as a loose folder in mods. It still loads as it is (a folder mod), so startup
    // leaves it: zipping it here held up the window for half a minute. The next Install replaces it with the .rtz
    // (replace_pack).
    // Install made the Dreamcast folder even with no upscales of the disc's images; an empty one goes.
    std::filesystem::remove(cache / "dreamcast", ec);
}

void add_exact_image(std::vector<uint8_t> indices, Image image) {
    upscaler.add_exact(std::move(indices), std::move(image));
}

void add_buttons(rush2::ui::OptionsPage* page) {
    using namespace recompui;
    ContextId context = get_current_context();

    // What the upscaling mode is doing shows in the corner of its option, laid over the row so nothing moves.
    if (Label* status_label = page->add_option_status(mode_option_id)) {
        auto shown = std::make_shared<std::string>();
        page->add_update_callback([status_label, shown]() {
            std::string status = upscaler.status();
            if (status != *shown) {
                *shown = status;
                status_label->set_text(status);
            }
        });
    }

    page->add_heading("Your Own Upscales", "",
        "Make your own sharper textures: dump the game's textures, make bigger versions of them with any image "
        "upscaler, save those in the user_texture_pack folder next to the dumped textures, then press "
        "<recomp-color primary>Install Upscaled Textures</recomp-color>.");

    // Each row of buttons gets a line of its own, so the buttons keep their full size; it scrolls into view as the
    // controller reaches them.
    Element* row = page->add_row(
        "<recomp-color primary>Dump Textures</recomp-color> saves every texture of every car and track, named after "
        "the car or track part it's on, in a folder per game, straight from the game files. Textures the game makes "
        "from them as you play (paint jobs, stripes) use your upscales of these.<br/><br/>"
        "<recomp-color primary>Dump Models</recomp-color> saves every model of Rush 2, SF Rush and Rush 2049 (cars, "
        "props, effects, menus, track pieces) as its own 3D model (.glb) with its textures, for Blender or any 3D "
        "viewer. Each folder also has _all.glb with all of its models, and each track _placed.glb with everything in "
        "its place.");

    Button* dump_button = context.create_element<Button>(row, "Dump Textures", ButtonStyle::Secondary);
    auto dump_running = std::make_shared<bool>(upscaler.is_dumping());
    dump_button->add_pressed_callback([dump_button, dump_running]() {
        if (upscaler.is_dumping() || upscaler.installing()) {
            return;
        }
        *dump_running = true;
        upscaler.start_dump();
        dump_button->set_text("Dumping...");
    });
    auto dump_shown = std::make_shared<std::string>();
    page->add_update_callback([dump_button, dump_running, dump_shown]() {
        if (!*dump_running) {
            return;
        }
        std::string status = upscaler.dump_status();
        if (status.empty()) {
            *dump_running = false;
            status = "Dumped " + std::to_string(upscaler.dumped()) + " Textures";
        }
        if (status != *dump_shown) {
            *dump_shown = status;
            dump_button->set_text(status);
        }
    });

    Button* models_button = context.create_element<Button>(row, "Dump Models", ButtonStyle::Secondary);
    auto models_running = std::make_shared<bool>(rush2::catalog::model_dump_running());
    auto models_shown = std::make_shared<std::string>();
    models_button->add_pressed_callback([models_button, models_running]() {
        if (rush2::catalog::start_model_dump()) {
            *models_running = true;
            models_button->set_text("Dumping...");
        }
    });
    page->add_update_callback([models_button, models_running, models_shown]() {
        if (!*models_running) {
            return;
        }
        std::string status = rush2::catalog::model_dump_status();
        if (!rush2::catalog::model_dump_running()) {
            *models_running = false;
        }
        if (status != *models_shown) {
            *models_shown = status;
            models_button->set_text(status);
        }
    });

    row = page->add_row(
        "<recomp-color primary>Open Dump Folder</recomp-color> shows the asset_dump folder: the dumped textures in "
        "textures, the models in models. Save your upscaled textures in user_texture_pack, in the same folders and "
        "keeping the file names, then press <recomp-color primary>Install Upscaled Textures</recomp-color> to use "
        "them.");
    Button* folder_button = context.create_element<Button>(row, "Open Dump Folder", ButtonStyle::Secondary);
    folder_button->add_pressed_callback([]() {
        std::error_code ec;
        std::filesystem::create_directories(asset_dump_dir(), ec);
        open_folder(asset_dump_dir());
    });

    Button* install_button = context.create_element<Button>(row, "Install Upscaled Textures", ButtonStyle::Secondary);
    // An install still running from before the page was last closed shows here until it ends.
    auto install_running = std::make_shared<bool>(upscaler.installing());
    if (*install_running) {
        install_button->set_text("Installing...");
    }
    install_button->add_pressed_callback([install_button, install_running]() {
        if (upscaler.is_dumping() || upscaler.installing()) {
            return;
        }
        upscaler.start_install();
        *install_running = true;
        install_button->set_text("Installing...");
    });
    page->add_update_callback([install_button, install_running]() {
        if (!*install_running || upscaler.installing()) {
            return;
        }
        *install_running = false;
        int count = upscaler.installed();
        if (count > 0) {
            install_button->set_text("Installed " + std::to_string(count) + " Textures");
        }
        else {
            const std::string& why = upscaler.install_failure();
            install_button->set_text(count == 0 ? "Nothing in user_texture_pack"
                                                : why.empty() ? "Install Failed" : "Install Failed: " + why);
        }
    });
}

}
