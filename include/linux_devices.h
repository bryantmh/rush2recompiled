#ifndef __LINUX_DEVICES_H__
#define __LINUX_DEVICES_H__

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Linux device lookups through sysfs for the controller haptics (src/linux_devices.cpp): which USB device a
// controller's SDL path belongs to, its hidraw nodes and its sound card. SDL's path for a controller is a hidraw node
// when its HIDAPI driver has it, an evdev node when it doesn't, and a virtual evdev node (on no USB device) when Steam
// Input stands in for it; for those, the real device is found by its USB IDs, which SDL still reports.
// Linux only.

namespace rush2::linux_devices {
    struct Hidraw {
        std::string node;       // /dev/hidrawN.
        unsigned vendor = 0;
        unsigned product = 0;
        int interface = -1;     // USB interface number, or -1 (Bluetooth).
        std::string usb_device; // sysfs path of its USB device, or empty.
    };

    struct SoundCard {
        int index = -1; // ALSA card number.
        unsigned vendor = 0;
        unsigned product = 0;
        std::string usb_device; // sysfs path of its USB device, or empty.
    };

    // The sysfs path of the USB device a /dev node (hidrawN, input/eventN, input/jsN) belongs to, or empty.
    std::string usb_device_of(const std::string& node);
    // Whether a /dev node belongs to no hardware (Steam Input's virtual pads, uinput), unlike one over Bluetooth.
    bool is_virtual(const std::string& node);

    std::vector<Hidraw> hidraw_devices();
    std::vector<SoundCard> sound_cards();

    enum class ReportKind { Input, Output, Feature };
    // The length of a report as written to the hidraw node (with its ID byte first, 0 if the device numbers none), from
    // the report descriptor, or 0 if the device has no such report.
    size_t report_bytes(const std::string& hidraw_node, ReportKind kind, uint8_t id);
}

#endif
