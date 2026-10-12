// Drops Steam's injected keys and mouse clicks for a Steam Controller (include/steam_input_filter.h).
//
// Only the game's own window is touched: no system-wide hooks (an earlier version used low-level keyboard and mouse
// hooks, which froze the whole PC's keyboard while the game was stopped in a crash). The window procedure is subclassed
// and the window registered for raw keyboard and mouse input. Raw input names the device of each key and click, and
// input injected with SendInput (how Steam's desktop layout types) has none (hDevice 0). Raw input reaches the window
// before the WM_KEYDOWN/WM_LBUTTONDOWN it also produces, so a count per key and button of injected presses still to come
// lets the matching window messages be swallowed before SDL sees them. Mouse movement is left alone.
//
// SDL only registers raw keyboard and mouse input for its raw keyboard hint and relative mouse mode, neither of which
// the game uses; if it ever did, its registration would replace this one and the filter would just stop. Measured with
// tools/sdl_devices.py --events: each Steam Controller press arrived as its gamepad button and, at the same moment, an
// injected key (A Return, B Escape, Start Escape, the d-pad arrows, the bumpers Ctrl and Alt, Y Space) or click.

#include <array>
#include <atomic>
#include <cstdio>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "steam_input_filter.h"

namespace {
    std::atomic_bool steam_controller = false;

#ifdef _WIN32
    WNDPROC sdl_proc = nullptr;

    // Window thread only. Injected presses (and releases) seen in raw input whose window message hasn't come yet, per
    // key scan code (bit 8 for extended keys) and per mouse button.
    constexpr int max_pending = 4; // An injected press whose message never came stops being waited for after this many.
    std::array<uint8_t, 512> keys_down{};
    std::array<uint8_t, 512> keys_up{};
    std::array<uint8_t, 5> buttons_down{};
    std::array<uint8_t, 5> buttons_up{};

    void count(uint8_t& pending) {
        pending = (uint8_t)(pending < max_pending ? pending + 1 : max_pending);
    }

    // Takes one of the injected events still to come; true if there was one (its window message is swallowed).
    bool take(uint8_t& pending) {
        if (pending == 0) {
            return false;
        }
        pending--;
        return true;
    }

    void clear_pending() {
        keys_down.fill(0);
        keys_up.fill(0);
        buttons_down.fill(0);
        buttons_up.fill(0);
    }

    void on_raw_input(HRAWINPUT handle) {
        RAWINPUT input;
        UINT size = sizeof(input);
        if (GetRawInputData(handle, RID_INPUT, &input, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1 ||
            input.header.hDevice != nullptr || !steam_controller.load(std::memory_order_relaxed)) {
            return;
        }
        if (input.header.dwType == RIM_TYPEKEYBOARD) {
            const RAWKEYBOARD& k = input.data.keyboard;
            int code = (k.MakeCode & 0xFF) | ((k.Flags & RI_KEY_E0) != 0 ? 0x100 : 0);
            count((k.Flags & RI_KEY_BREAK) != 0 ? keys_up[code] : keys_down[code]);
        }
        else if (input.header.dwType == RIM_TYPEMOUSE) {
            USHORT flags = input.data.mouse.usButtonFlags;
            static constexpr USHORT down_flags[5] = { RI_MOUSE_BUTTON_1_DOWN, RI_MOUSE_BUTTON_2_DOWN,
                RI_MOUSE_BUTTON_3_DOWN, RI_MOUSE_BUTTON_4_DOWN, RI_MOUSE_BUTTON_5_DOWN };
            static constexpr USHORT up_flags[5] = { RI_MOUSE_BUTTON_1_UP, RI_MOUSE_BUTTON_2_UP, RI_MOUSE_BUTTON_3_UP,
                RI_MOUSE_BUTTON_4_UP, RI_MOUSE_BUTTON_5_UP };
            for (int b = 0; b < 5; b++) {
                if ((flags & down_flags[b]) != 0) {
                    count(buttons_down[b]);
                }
                if ((flags & up_flags[b]) != 0) {
                    count(buttons_up[b]);
                }
            }
        }
    }

    // Whether a window message comes from an injected key or click (and takes it).
    bool injected(UINT msg, WPARAM wparam, LPARAM lparam) {
        switch (msg) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP: {
                int code = (int)((lparam >> 16) & 0xFF) | ((lparam & (1 << 24)) != 0 ? 0x100 : 0);
                bool up = msg == WM_KEYUP || msg == WM_SYSKEYUP;
                return take(up ? keys_up[code] : keys_down[code]);
            }
            case WM_LBUTTONDOWN: return take(buttons_down[0]);
            case WM_LBUTTONUP: return take(buttons_up[0]);
            case WM_RBUTTONDOWN: return take(buttons_down[1]);
            case WM_RBUTTONUP: return take(buttons_up[1]);
            case WM_MBUTTONDOWN: return take(buttons_down[2]);
            case WM_MBUTTONUP: return take(buttons_up[2]);
            case WM_XBUTTONDOWN: return take(buttons_down[GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 3 : 4]);
            case WM_XBUTTONUP: return take(buttons_up[GET_XBUTTON_WPARAM(wparam) == XBUTTON1 ? 3 : 4]);
            default: return false;
        }
    }

    LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        if (msg == WM_INPUT) {
            on_raw_input((HRAWINPUT)lparam);
            // SDL would read raw keyboard input as keys of their own: it only needs the WM_INPUT it registered for.
            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
        if (msg == WM_KILLFOCUS) {
            clear_pending();
        }
        if (injected(msg, wparam, lparam)) {
            return 0;
        }
        return CallWindowProcW(sdl_proc, hwnd, msg, wparam, lparam);
    }
#endif
}

void rush2::steam_input_filter::install(void* window) {
#ifdef _WIN32
    HWND hwnd = (HWND)window;
    RAWINPUTDEVICE devices[2] = {};
    devices[0].usUsagePage = 0x01; // Generic desktop
    devices[0].usUsage = 0x06;     // Keyboard
    devices[0].hwndTarget = hwnd;
    devices[1].usUsagePage = 0x01;
    devices[1].usUsage = 0x02;     // Mouse
    devices[1].hwndTarget = hwnd;
    if (!RegisterRawInputDevices(devices, 2, sizeof(devices[0]))) {
        fprintf(stderr, "[Input] Couldn't register raw input for the Steam Controller filter (error %lu)\n", GetLastError());
        return;
    }
    sdl_proc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)window_proc);
#else
    (void)window;
#endif
}

void rush2::steam_input_filter::set_steam_controller_connected(bool connected) {
    if (!steam_controller.exchange(connected, std::memory_order_relaxed) && connected) {
        printf("[Input] Steam Controller connected: ignoring the keys and clicks Steam types for it\n");
    }
}
