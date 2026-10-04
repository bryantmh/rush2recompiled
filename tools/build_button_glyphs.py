"""Builds the button glyphs drawn by the in-game Controller Setup screen (src/controls_menu.cpp).

Every glyph is rendered from PromptFont (assets/promptfont/promptfont.ttf, SIL Open Font License), the same font the
frontend's menus use for button prompts, so the game shows the buttons of the controller each player actually holds:
Xbox, PlayStation or keyboard. Each glyph is a 64x56 8-bit intensity (I8) image, 4x the size of the game's 16x14 button
icons, which the RDP samples as alpha and the screen tints with the primitive color. Keys PromptFont has no glyph for
are drawn as a blank key with their label cut out of it.

  python tools/build_button_glyphs.py

Writes assets/button_glyphs.bin (the images, in glyph order) and include/button_glyphs.h (the glyph enum).
"""

import os
import re

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT_PATH = os.path.join(ROOT, "assets", "promptfont", "promptfont.ttf")
PROMPTFONT_H = os.path.join(ROOT, "lib", "RecompFrontend", "recompinput", "include", "recompinput", "promptfont.h")
OUT_BIN = os.path.join(ROOT, "assets", "button_glyphs.bin")
OUT_H = os.path.join(ROOT, "include", "button_glyphs.h")

WIDTH = 64
HEIGHT = 56
SUPERSAMPLE = 4

# (enum name, PromptFont define or None, label drawn on a blank key or None).
GLYPHS = [
    ("XboxA", "PF_XBOX_A", None),
    ("XboxB", "PF_XBOX_B", None),
    ("XboxX", "PF_XBOX_X", None),
    ("XboxY", "PF_XBOX_Y", None),
    ("XboxLB", "PF_XBOX_LEFT_SHOULDER", None),
    ("XboxRB", "PF_XBOX_RIGHT_SHOULDER", None),
    ("XboxLT", "PF_XBOX_LEFT_TRIGGER", None),
    ("XboxRT", "PF_XBOX_RIGHT_TRIGGER", None),
    ("XboxView", "PF_XBOX_VIEW", None),
    ("XboxMenu", "PF_XBOX_MENU", None),
    ("SonyCross", "PF_SONY_A", None),
    ("SonyCircle", "PF_SONY_B", None),
    ("SonySquare", "PF_SONY_X", None),
    ("SonyTriangle", "PF_SONY_Y", None),
    ("SonyL1", "PF_SONY_LEFT_SHOULDER", None),
    ("SonyR1", "PF_SONY_RIGHT_SHOULDER", None),
    ("SonyL2", "PF_SONY_LEFT_TRIGGER", None),
    ("SonyR2", "PF_SONY_RIGHT_TRIGGER", None),
    ("SonyCreate", "PF_SONY_DUALSENSE_SHARE", None),
    ("SonyOptions", "PF_SONY_DUALSENSE_OPTIONS", None),
    ("SonyTouchpad", "PF_SONY_DUALSENSE_TOUCHPAD", None),
    ("LeftStickClick", "PF_ANALOG_L_CLICK", None),
    ("RightStickClick", "PF_ANALOG_R_CLICK", None),
    ("Home", "PF_GAMEPAD_HOME", None),
    ("Misc", "PF_GAMEPAD_M1", None),
    ("PaddleR4", "PF_GAMEPAD_R4", None),
    ("PaddleL4", "PF_GAMEPAD_L4", None),
    ("PaddleR5", "PF_GAMEPAD_R5", None),
    ("PaddleL5", "PF_GAMEPAD_L5", None),
    ("DpadUp", "PF_DPAD_UP", None),
    ("DpadDown", "PF_DPAD_DOWN", None),
    ("DpadLeft", "PF_DPAD_LEFT", None),
    ("DpadRight", "PF_DPAD_RIGHT", None),
    ("Dpad", "PF_DPAD", None),
    ("LeftStick", "PF_ANALOG_L", None),
    ("RightStick", "PF_ANALOG_R", None),
    ("LeftStickUp", "PF_ANALOG_L_UP", None),
    ("LeftStickDown", "PF_ANALOG_L_DOWN", None),
    ("LeftStickLeft", "PF_ANALOG_L_LEFT", None),
    ("LeftStickRight", "PF_ANALOG_L_RIGHT", None),
    ("RightStickUp", "PF_ANALOG_R_UP", None),
    ("RightStickDown", "PF_ANALOG_R_DOWN", None),
    ("RightStickLeft", "PF_ANALOG_R_LEFT", None),
    ("RightStickRight", "PF_ANALOG_R_RIGHT", None),
    # promptfont.h has the up and right arrow keys swapped (U+23F5 is the right arrow, U+23F6 the up arrow).
    ("KeyUp", "PF_KEYBOARD_RIGHT", None),
    ("KeyDown", "PF_KEYBOARD_DOWN", None),
    ("KeyLeft", "PF_KEYBOARD_LEFT", None),
    ("KeyRight", "PF_KEYBOARD_UP", None),
    ("KeyArrows", "PF_KEYBOARD_ARROWS", None),
    ("KeySpace", "PF_KEYBOARD_SPACE", None),
    ("KeyShift", "PF_KEYBOARD_SHIFT", None),
    ("KeyControl", "PF_KEYBOARD_CONTROL", None),
    ("KeyAlt", "PF_KEYBOARD_ALT", None),
    ("KeyTab", "PF_KEYBOARD_TAB", None),
    ("KeyCaps", "PF_KEYBOARD_CAPS", None),
    ("KeyEnter", "PF_KEYBOARD_ENTER", None),
    ("KeyBackspace", "PF_KEYBOARD_BACKSPACE", None),
    ("KeyEscape", "PF_KEYBOARD_ESCAPE", None),
    ("KeyDelete", "PF_KEYBOARD_DELETE", None),
    ("KeyInsert", "PF_KEYBOARD_INSERT", None),
    ("KeyHome", "PF_KEYBOARD_HOME", None),
    ("KeyEnd", "PF_KEYBOARD_END", None),
    ("KeyPageUp", "PF_KEYBOARD_PAGE_UP", None),
    ("KeyPageDown", "PF_KEYBOARD_PAGE_DOWN", None),
    ("KeySuper", "PF_KEYBOARD_SUPER", None),
    ("KeyBlank", "PF_KEYBOARD_KEY", None),
]
GLYPHS += [("Key" + c, "PF_KEYBOARD_" + c, None) for c in "ABCDEFGHIJKLMNOPQRSTUVWXYZ"]
GLYPHS += [("Key" + c, "PF_KEYBOARD_" + c, None) for c in "0123456789"]
GLYPHS += [("KeyF%d" % i, "PF_KEYBOARD_F%d" % i, None) for i in range(1, 13)]
# Keys without a PromptFont glyph: a blank key with the label drawn from PromptFont's ASCII glyphs.
GLYPHS += [
    ("KeyMinus", None, "-"),
    ("KeyEquals", None, "="),
    ("KeyLeftBracket", None, "["),
    ("KeyRightBracket", None, "]"),
    ("KeyBackslash", None, "\\"),
    ("KeySemicolon", None, ";"),
    ("KeyApostrophe", None, "'"),
    ("KeyGrave", None, "`"),
    ("KeyComma", None, ","),
    ("KeyPeriod", None, "."),
    ("KeySlash", None, "/"),
    ("KeyPadPlus", None, "+"),
    ("KeyPadMinus", None, "-"),
    ("KeyPadMultiply", None, "*"),
    ("KeyPadDivide", None, "/"),
    ("KeyPadEnter", "PF_KEYBOARD_ENTER", None),
    ("KeyPadPeriod", None, "."),
] + [("KeyPad%d" % i, None, str(i)) for i in range(10)] + [
    ("KeyUnknown", None, "?"),
]


def read_promptfont_defines():
    defines = {}
    with open(PROMPTFONT_H, encoding="utf-8") as f:
        for line in f:
            m = re.match(r'#define (PF_\w+) "(.+)"', line)
            if m:
                defines[m.group(1)] = m.group(2)
    return defines


def fit(img, box_w, box_h):
    """Scales the ink in img (an L image) to fit box_w x box_h, keeping its aspect ratio."""
    bbox = img.getbbox()
    if bbox is None:
        return Image.new("L", (box_w, box_h), 0)
    ink = img.crop(bbox)
    scale = min(box_w / ink.width, box_h / ink.height)
    size = (max(1, round(ink.width * scale)), max(1, round(ink.height * scale)))
    return ink.resize(size, Image.LANCZOS)


def render_char(font, char):
    size = font.size * 2
    img = Image.new("L", (size, size), 0)
    ImageDraw.Draw(img).text((size // 4, size // 4), char, font=font, fill=255)
    return img


def render_glyph(font, defines, define, label):
    w, h = WIDTH * SUPERSAMPLE, HEIGHT * SUPERSAMPLE
    canvas = Image.new("L", (w, h), 0)
    margin = 2 * SUPERSAMPLE
    if define is not None:
        ink = fit(render_char(font, defines[define]), w - 2 * margin, h - 2 * margin)
        canvas.paste(ink, ((w - ink.width) // 2, (h - ink.height) // 2))
    else:
        key = fit(render_char(font, defines["PF_KEYBOARD_KEY"]), w - 2 * margin, h - 2 * margin)
        kx, ky = (w - key.width) // 2, (h - key.height) // 2
        canvas.paste(key, (kx, ky))
        # The label sits in the key's face, the upper part of the blank key glyph. Every label is scaled by the same
        # factor and keeps its place relative to the digits' box, so punctuation stays small and at its own height.
        cell = render_char(font, "0").getbbox()
        scale = key.height * 45 / 100 / (cell[3] - cell[1])
        img = render_char(font, label)
        bbox = img.getbbox()
        text = img.crop(bbox)
        text = text.resize((max(1, round(text.width * scale)), max(1, round(text.height * scale))), Image.LANCZOS)
        tx = kx + (key.width - text.width) // 2
        top = ky + key.height * 40 // 100 - round((cell[3] - cell[1]) * scale) // 2
        ty = top + round((bbox[1] - cell[1]) * scale)
        # Cut out of the key like PromptFont's own key labels.
        canvas.paste(0, (tx, ty), text)
    return canvas.resize((WIDTH, HEIGHT), Image.LANCZOS)


def main():
    defines = read_promptfont_defines()
    font = ImageFont.truetype(FONT_PATH, 256)

    data = bytearray()
    for name, define, label in GLYPHS:
        data += render_glyph(font, defines, define, label).tobytes()

    with open(OUT_BIN, "wb") as f:
        f.write(b"R2BG")
        f.write(len(GLYPHS).to_bytes(4, "big"))
        f.write(WIDTH.to_bytes(4, "big"))
        f.write(HEIGHT.to_bytes(4, "big"))
        f.write(data)

    with open(OUT_H, "w", newline="\n") as f:
        f.write("// Generated by tools/build_button_glyphs.py. Glyph order in assets/button_glyphs.bin.\n\n")
        f.write("#ifndef __BUTTON_GLYPHS_H__\n#define __BUTTON_GLYPHS_H__\n\n")
        f.write("#include <cstdint>\n\n")
        f.write("namespace rush2::controls {\n")
        f.write("    constexpr uint32_t glyph_width = %d;\n" % WIDTH)
        f.write("    constexpr uint32_t glyph_height = %d;\n\n" % HEIGHT)
        f.write("    enum class Glyph : uint16_t {\n")
        for name, _, _ in GLYPHS:
            f.write("        %s,\n" % name)
        f.write("        Count,\n        None = 0xFFFF,\n    };\n}\n\n#endif\n")

    print("%d glyphs, %d bytes" % (len(GLYPHS), len(data)))


if __name__ == "__main__":
    main()
