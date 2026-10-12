"""Lists the joysticks and gamepads SDL3 sees, with vendor:product, type, path and rumble support (input debugging).

Usage: python tools/sdl_devices.py [--wait SECONDS] [--events SECONDS] [path/to/SDL3.dll] [HINT=value ...]
Loads build/SDL3.dll by default. Extra HINT=value arguments are set as SDL hints before init, e.g.
SDL_JOYSTICK_HIDAPI_STEAM=1. Waits a few seconds (--wait) for wireless controllers to report in before listing: a
Steam Controller's puck only becomes a gamepad once the controller is awake and connected to it. Set
SDL_LOGGING=*=verbose to see every HID device SDL finds and the driver it picks.
--events opens a window and prints every key, mouse button and gamepad button press for that many seconds, with the
keyboard or mouse it came from (raw keyboard input is turned on, so injected keys, like Steam's desktop layout typing
for a controller, show as device 0). Use it to find controllers that also type keys (double inputs).
"""
import ctypes
import os
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))

SDL_INIT_VIDEO = 0x00000020
SDL_INIT_GAMEPAD = 0x00002000
SDL_EVENT_KEY_DOWN = 0x300
SDL_EVENT_MOUSE_BUTTON_DOWN = 0x401
SDL_EVENT_GAMEPAD_BUTTON_DOWN = 0x651
SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN = b"SDL.joystick.cap.rumble"
SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN = b"SDL.joystick.cap.trigger_rumble"


def main():
    args = sys.argv[1:]
    wait = 2.5
    events = 0.0
    while len(args) >= 2 and args[0] in ('--wait', '--events'):
        if args[0] == '--wait':
            wait = float(args[1])
        else:
            events = float(args[1])
        args = args[2:]
    dll = os.path.join(ROOT, 'build', 'SDL3.dll')
    if args and not '=' in args[0]:
        dll = args.pop(0)
    sdl = ctypes.CDLL(dll)
    sdl.SDL_GetError.restype = ctypes.c_char_p
    sdl.SDL_GetJoysticks.restype = ctypes.POINTER(ctypes.c_uint32)
    sdl.SDL_GetJoysticks.argtypes = [ctypes.POINTER(ctypes.c_int)]
    for name in ('SDL_GetJoystickNameForID', 'SDL_GetJoystickPathForID'):
        getattr(sdl, name).restype = ctypes.c_char_p
        getattr(sdl, name).argtypes = [ctypes.c_uint32]
    for name in ('SDL_GetJoystickVendorForID', 'SDL_GetJoystickProductForID'):
        getattr(sdl, name).restype = ctypes.c_uint16
        getattr(sdl, name).argtypes = [ctypes.c_uint32]
    sdl.SDL_IsGamepad.argtypes = [ctypes.c_uint32]
    sdl.SDL_GetGamepadTypeForID.argtypes = [ctypes.c_uint32]
    sdl.SDL_OpenGamepad.restype = ctypes.c_void_p
    sdl.SDL_OpenGamepad.argtypes = [ctypes.c_uint32]
    sdl.SDL_GetGamepadProperties.argtypes = [ctypes.c_void_p]
    sdl.SDL_GetBooleanProperty.argtypes = [ctypes.c_uint32, ctypes.c_char_p, ctypes.c_bool]
    sdl.SDL_GetBooleanProperty.restype = ctypes.c_bool
    sdl.SDL_SetHint.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    for hint in args:
        key, value = hint.split('=', 1)
        sdl.SDL_SetHint(key.encode(), value.encode())

    if events:
        sdl.SDL_SetHint(b'SDL_WINDOWS_RAW_KEYBOARD', b'1')
    if not sdl.SDL_Init(SDL_INIT_GAMEPAD | (SDL_INIT_VIDEO if events else 0)):
        print('SDL_Init failed:', sdl.SDL_GetError().decode())
        return
    deadline = time.time() + wait
    while time.time() < deadline:
        sdl.SDL_PumpEvents()
        time.sleep(0.05)

    count = ctypes.c_int()
    ids = sdl.SDL_GetJoysticks(ctypes.byref(count))
    print(f'{count.value} joystick(s)')
    for i in range(count.value):
        jid = ids[i]
        name = (sdl.SDL_GetJoystickNameForID(jid) or b'?').decode(errors='replace')
        path = (sdl.SDL_GetJoystickPathForID(jid) or b'').decode(errors='replace')
        line = (f'  {name}  {sdl.SDL_GetJoystickVendorForID(jid):04X}:{sdl.SDL_GetJoystickProductForID(jid):04X}'
                f'  gamepad={bool(sdl.SDL_IsGamepad(jid))} type={sdl.SDL_GetGamepadTypeForID(jid)}')
        pad = sdl.SDL_OpenGamepad(jid) if sdl.SDL_IsGamepad(jid) else None
        if pad:
            props = sdl.SDL_GetGamepadProperties(pad)
            line += (f' rumble={sdl.SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, False)}'
                     f' trigger_rumble={sdl.SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, False)}')
        print(line)
        print(f'    path {path}')
    if events:
        print_events(sdl, events)
    sdl.SDL_Quit()


def print_events(sdl, seconds):
    """Prints key, mouse button and gamepad button presses with their source device for `seconds` (see --events)."""
    sdl.SDL_CreateWindow.restype = ctypes.c_void_p
    sdl.SDL_CreateWindow.argtypes = [ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_uint64]
    sdl.SDL_RaiseWindow.argtypes = [ctypes.c_void_p]
    sdl.SDL_GetKeyboardNameForID.restype = ctypes.c_char_p
    sdl.SDL_GetKeyboardNameForID.argtypes = [ctypes.c_uint32]
    sdl.SDL_GetMouseNameForID.restype = ctypes.c_char_p
    sdl.SDL_GetMouseNameForID.argtypes = [ctypes.c_uint32]
    sdl.SDL_GetScancodeName.restype = ctypes.c_char_p
    sdl.SDL_GetScancodeName.argtypes = [ctypes.c_uint32]
    sdl.SDL_GetJoystickNameForID.argtypes = [ctypes.c_uint32]
    window = sdl.SDL_CreateWindow(b'sdl_devices events: keep this focused', 640, 200, 0)
    sdl.SDL_RaiseWindow(window)
    print(f'Recording presses for {seconds:g} s (keep the window focused)')
    event = (ctypes.c_uint8 * 128)()
    u32 = lambda off: int.from_bytes(bytes(event[off:off + 4]), 'little')
    name = lambda raw: (raw or b'?').decode(errors='replace')
    start = time.time()
    while time.time() - start < seconds:
        while sdl.SDL_PollEvent(ctypes.byref(event)):
            kind = u32(0)
            t = time.time() - start
            if kind == SDL_EVENT_KEY_DOWN and not event[37]:
                which = u32(20)
                print(f'{t:6.2f} key {name(sdl.SDL_GetScancodeName(u32(24)))!r} from keyboard {which} '
                      f'({name(sdl.SDL_GetKeyboardNameForID(which)) if which else "injected/unknown"})')
            elif kind == SDL_EVENT_MOUSE_BUTTON_DOWN:
                which = u32(20)
                print(f'{t:6.2f} mouse button {event[24]} from mouse {which} '
                      f'({name(sdl.SDL_GetMouseNameForID(which)) if which else "injected/unknown"})')
            elif kind == SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                print(f'{t:6.2f} gamepad button {event[20]} from {name(sdl.SDL_GetJoystickNameForID(u32(16)))}')
        time.sleep(0.005)


if __name__ == '__main__':
    main()
