#include <array>
#include <filesystem>
#include <fstream>
#include <string>

#include "librecomp/game.hpp"
#include "util/file.h"

#include "rush2.h"

// Portable mode: recompui::file::get_app_folder_path() uses the working directory (the executable's folder, see main)
// instead of %LOCALAPPDATA% when portable.txt exists there. The app folder is resolved once at startup, so switching
// only takes effect on the next launch. Until then the old folder stays in use, so the move happens at the next launch
// too: data_move.txt records the folder that was in use and apply_pending_move() copies its data across.

namespace fs = std::filesystem;

namespace {
    const fs::path portable_marker = "portable.txt";
    const fs::path move_marker = "data_move.txt";

    // Folders and files in the app folder that hold user data. In portable mode the app folder is also the program's
    // folder, so everything else there (the executable, assets, user ROMs to import) is left alone.
    // "ghosts" is where older versions kept ghosts; rush2::data_files::migrate() moves it into saves.
    constexpr std::array data_folders = { "saves", "mods", "mod_config", "ghosts" };
    constexpr std::array data_files = { "rush2.n64.us.z64", "rush2049.z64", "rush1.z64" };

    bool is_data_file(const fs::path& path) {
        std::string name = path.filename().string();
        auto ends_with = [&name](const std::string& suffix) {
            return name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
        };
        if (ends_with(".json") || ends_with(".json.bak")) {
            return true;
        }
        for (const char* data_file : data_files) {
            if (name == data_file) {
                return true;
            }
        }
        return false;
    }

    bool is_data_folder(const fs::path& path) {
        std::string name = path.filename().string();
        for (const char* data_folder : data_folders) {
            if (name == data_folder) {
                return true;
            }
        }
        return false;
    }

    bool same_folder(const fs::path& a, const fs::path& b) {
        std::error_code ec;
        bool equivalent = fs::equivalent(a, b, ec);
        return ec ? fs::absolute(a, ec).lexically_normal() == fs::absolute(b, ec).lexically_normal() : equivalent;
    }
}

bool rush2::data_location::is_portable() {
    std::error_code ec;
    return fs::exists(portable_marker, ec);
}

bool rush2::data_location::set_portable(bool portable) {
    std::error_code ec;
    if (portable != is_portable()) {
        if (portable) {
            std::ofstream marker{ portable_marker };
            marker << "Settings and saves are stored in this folder. Delete this file to store them in the user's app data folder.\n";
            if (!marker.good()) {
                return false;
            }
        }
        else if (!fs::remove(portable_marker, ec)) {
            return false;
        }
    }

    // Schedule a copy from the folder this session uses, or cancel one if this switches back to it.
    fs::path current_folder = recomp::get_config_path();
    if (same_folder(recompui::file::get_app_folder_path(), current_folder)) {
        fs::remove(move_marker, ec);
    }
    else {
        std::ofstream marker{ move_marker, std::ios::binary };
        std::u8string source = current_folder.u8string();
        marker.write(reinterpret_cast<const char*>(source.data()), source.size());
        if (!marker.good()) {
            return false;
        }
    }
    return true;
}

void rush2::data_location::apply_pending_move() {
    std::error_code ec;
    if (!fs::exists(move_marker, ec)) {
        return;
    }

    std::u8string source_string;
    {
        std::ifstream marker{ move_marker, std::ios::binary };
        std::string contents{ std::istreambuf_iterator<char>(marker), std::istreambuf_iterator<char>() };
        source_string.assign(contents.begin(), contents.end());
    }
    fs::remove(move_marker, ec);

    fs::path source{ source_string };
    fs::path dest = recompui::file::get_app_folder_path();
    if (source.empty() || dest.empty() || !fs::is_directory(source, ec) || same_folder(source, dest)) {
        return;
    }

    // The old folder keeps its copy, so nothing is lost if the copy fails partway.
    bool failed = !fs::create_directories(dest, ec) && ec;
    for (const auto& entry : fs::directory_iterator(source, ec)) {
        std::error_code copy_ec;
        if (entry.is_regular_file() && is_data_file(entry.path())) {
            fs::copy_file(entry.path(), dest / entry.path().filename(), fs::copy_options::overwrite_existing, copy_ec);
        }
        else if (entry.is_directory() && is_data_folder(entry.path())) {
            fs::copy(entry.path(), dest / entry.path().filename(),
                fs::copy_options::recursive | fs::copy_options::overwrite_existing, copy_ec);
        }
        failed |= static_cast<bool>(copy_ec);
    }

    if (failed || ec) {
        std::u8string dest_string = dest.u8string();
        std::string message = "Some settings or saves could not be copied to the new data folder:\n";
        message.append(dest_string.begin(), dest_string.end());
        message += "\n\nThe originals are still in:\n";
        message.append(source_string.begin(), source_string.end());
        recompui::file::show_error_message_box("Data Location", message.c_str());
    }
}
