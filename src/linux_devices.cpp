// Linux device lookups through sysfs for the controller haptics (include/linux_devices.h).
//
// /sys/class/hidraw/hidrawN/device and /sys/class/sound/cardN/device lead to the HID device and the sound interface;
// their USB device is the nearest parent with a busnum file (a USB interface's folder has none). A hidraw node's USB
// IDs are in its device's uevent (HID_ID=bus:vendor:product), its report descriptor in report_descriptor (readable
// without opening the node, which may need udev rules).

#ifdef __linux__

#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

#include "linux_devices.h"

namespace fs = std::filesystem;

namespace {
    std::string read_line(const fs::path& path) {
        std::ifstream file{ path };
        std::string line;
        std::getline(file, line);
        return line;
    }

    unsigned read_hex(const fs::path& path) {
        try {
            return (unsigned)std::stoul(read_line(path), nullptr, 16);
        }
        catch (...) {
            return 0;
        }
    }

    // The nearest folder at or above a sysfs device that is a USB device, or empty.
    fs::path usb_parent(fs::path path) {
        std::error_code ec;
        for (; path.has_relative_path(); path = path.parent_path()) {
            if (fs::exists(path / "busnum", ec) && fs::exists(path / "devnum", ec)) {
                return path;
            }
        }
        return {};
    }

    // The sysfs device of a /dev node, or empty.
    fs::path sys_device_of(const std::string& node) {
        std::string name = fs::path{ node }.filename().string();
        fs::path device;
        if (name.rfind("hidraw", 0) == 0) {
            device = fs::path{ "/sys/class/hidraw" } / name / "device";
        }
        else if (name.rfind("event", 0) == 0 || name.rfind("js", 0) == 0) {
            device = fs::path{ "/sys/class/input" } / name / "device";
        }
        else {
            return {};
        }
        std::error_code ec;
        device = fs::canonical(device, ec);
        return ec ? fs::path{} : device;
    }
}

std::string rush2::linux_devices::usb_device_of(const std::string& node) {
    fs::path device = sys_device_of(node);
    return device.empty() ? std::string{} : usb_parent(device).string();
}

bool rush2::linux_devices::is_virtual(const std::string& node) {
    fs::path device = sys_device_of(node);
    return device.empty() || device.string().rfind("/sys/devices/virtual/", 0) == 0;
}

std::vector<rush2::linux_devices::Hidraw> rush2::linux_devices::hidraw_devices() {
    std::vector<Hidraw> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/hidraw", ec)) {
        Hidraw h;
        h.node = "/dev/" + entry.path().filename().string();
        fs::path device = fs::canonical(entry.path() / "device", ec);
        if (ec) {
            continue;
        }
        std::ifstream uevent{ device / "uevent" };
        for (std::string line; std::getline(uevent, line);) {
            unsigned bus, vendor, product;
            if (sscanf(line.c_str(), "HID_ID=%x:%x:%x", &bus, &vendor, &product) == 3) {
                h.vendor = vendor;
                h.product = product;
            }
        }
        fs::path usb_interface = device.parent_path();
        if (fs::exists(usb_interface / "bInterfaceNumber", ec)) {
            h.interface = (int)read_hex(usb_interface / "bInterfaceNumber");
        }
        h.usb_device = usb_parent(device).string();
        found.push_back(std::move(h));
    }
    return found;
}

std::vector<rush2::linux_devices::SoundCard> rush2::linux_devices::sound_cards() {
    std::vector<SoundCard> found;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator("/sys/class/sound", ec)) {
        std::string name = entry.path().filename().string();
        int index;
        if (sscanf(name.c_str(), "card%d", &index) != 1) {
            continue;
        }
        SoundCard card;
        card.index = index;
        fs::path device = fs::canonical(entry.path() / "device", ec);
        if (!ec) {
            fs::path usb = usb_parent(device);
            card.usb_device = usb.string();
            if (!usb.empty()) {
                card.vendor = read_hex(usb / "idVendor");
                card.product = read_hex(usb / "idProduct");
            }
        }
        found.push_back(card);
    }
    return found;
}

size_t rush2::linux_devices::report_bytes(const std::string& hidraw_node, ReportKind kind, uint8_t id) {
    std::string name = fs::path{ hidraw_node }.filename().string();
    std::ifstream file{ fs::path{ "/sys/class/hidraw" } / name / "device" / "report_descriptor", std::ios::binary };
    std::vector<uint8_t> desc{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };

    // Short items only (HID 1.11 6.2.2.2): the global report size, count and ID, Push and Pop, and the Input (tag 8),
    // Output (9) and Feature (11) main items, whose bits add up per report.
    struct Globals { uint32_t size = 0, count = 0; uint8_t id = 0; };
    Globals globals;
    std::vector<Globals> stack;
    std::map<uint8_t, size_t> bits;
    bool numbered = false;
    const uint8_t main_tag = kind == ReportKind::Input ? 8 : kind == ReportKind::Output ? 9 : 11;
    for (size_t i = 0; i < desc.size();) {
        uint8_t prefix = desc[i];
        if (prefix == 0xFE) { // Long item.
            i += 3 + (i + 1 < desc.size() ? desc[i + 1] : 0);
            continue;
        }
        size_t size = (prefix & 3) == 3 ? 4 : (prefix & 3);
        uint32_t value = 0;
        for (size_t b = 0; b < size && i + 1 + b < desc.size(); b++) {
            value |= (uint32_t)desc[i + 1 + b] << (8 * b);
        }
        uint8_t type = (prefix >> 2) & 3, tag = prefix >> 4;
        if (type == 1) { // Global.
            switch (tag) {
                case 7: globals.size = value; break;
                case 8: globals.id = (uint8_t)value; numbered = true; break;
                case 9: globals.count = value; break;
                case 10: stack.push_back(globals); break;
                case 11: if (!stack.empty()) { globals = stack.back(); stack.pop_back(); } break;
            }
        }
        else if (type == 0 && tag == main_tag) {
            bits[globals.id] += (size_t)globals.size * globals.count;
        }
        i += 1 + size;
    }
    auto it = bits.find(numbered ? id : 0);
    if (it == bits.end() || it->second == 0) {
        return 0;
    }
    return (it->second + 7) / 8 + 1; // The ID byte is written even when the device numbers no reports (as 0).
}

#endif
