"""Render PNG mock-ups of the Lilly OLED status screen.

Mirrors the layout in src/display/lilly_status_screen.c pixel for pixel
(same font/icons from font.py). Usage: python3 preview.py OUT_DIR
"""

import os
import struct
import sys
import zlib

from font import ADV_X, ADV_Y, FONT, GLYPH_H, GLYPH_W, ICONS

PW, PH = 32, 128

# --- layout (keep in sync with lilly_status_screen.c) ---------------------
TOP_Y, TOP_H, TOP_GAP = 1, 12, 1  # top row: [win mark] | centered die / USB | [caps dot]
DIE_W = 12
BAT_X, BAT_Y, BAT_W, BAT_H = 1, 16, 27, 9
BAR_X, BAR_MAX_W = 3, 23
LAYER_Y = 28
LOG_Y, LOG_ROWS, LOG_COLS = 42, 12, 8


class Canvas:
    def __init__(self):
        self.px = [[0] * PW for _ in range(PH)]

    def set(self, x, y):
        if 0 <= x < PW and 0 <= y < PH:
            self.px[y][x] = 1

    def rect(self, x, y, w, h, on=1):
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                if 0 <= xx < PW and 0 <= yy < PH:
                    self.px[yy][xx] = on

    def bitmap(self, x, y, rows, scale=1):
        for r, line in enumerate(rows):
            for c, ch in enumerate(line):
                if ch == "#":
                    self.rect(x + c * scale, y + r * scale, scale, scale)

    def text(self, x, y, s, scale=1):
        for i, ch in enumerate(s):
            self.bitmap(x + i * ADV_X * scale, y, FONT.get(ch.upper(), FONT["?"]), scale)


def battery(c, left, right, split):
    c.rect(BAT_X, BAT_Y, BAT_W, BAT_H)
    c.rect(BAT_X + 1, BAT_Y + 1, BAT_W - 2, BAT_H - 2, 0)
    c.rect(BAT_X + BAT_W, BAT_Y + 2, 2, BAT_H - 4)  # terminal

    def bar(y, h, level):
        if level is None:
            return
        w = (min(level, 100) * BAR_MAX_W + 50) // 100
        c.rect(BAR_X, y, max(w, 1 if level > 0 else 0), h)

    if split:
        bar(BAT_Y + 2, 2, left)
        bar(BAT_Y + 5, 2, right)
    else:
        bar(BAT_Y + 2, 5, left)


def keylog(c, lines):
    lines = lines[-LOG_ROWS:]
    for i, line in enumerate(lines):
        c.text(0, LOG_Y + i * ADV_Y, line[:LOG_COLS])


def top_row(c, icon, info="", caps=False, os=None, invert_icon=False):
    """OS mark on the left, icon strictly centered, info right of it, caps dot on the right."""
    def vmid(h):
        return TOP_Y + (TOP_H - h) // 2
    # side marks are centered in the free space left / right of the (fixed-size) die
    side = (PW - DIE_W) // 2
    if os == "win":
        w = len(ICONS["win"][0])
        c.bitmap((side - w) // 2, vmid(len(ICONS["win"])), ICONS["win"])
    if caps:
        w = len(ICONS["dot"][0])
        c.bitmap(PW - side + (side - w) // 2, vmid(len(ICONS["dot"])), ICONS["dot"])
    iw = len(icon[0])
    ix = (PW - iw) // 2
    if invert_icon:
        # solid die (rounded corners) with dark pips: keep only the pips from the outline icon
        h_, w_ = len(icon), len(icon[0])
        corners = {(0, 0), (w_ - 1, 0), (0, h_ - 1), (w_ - 1, h_ - 1)}
        frame = lambda x, y: x in (0, w_ - 1) or y in (0, h_ - 1)
        icon = ["".join(
            "." if (x, y) in corners or (icon[y][x] == "#" and not frame(x, y)) else "#"
            for x in range(w_)) for y in range(h_)]
    c.bitmap(ix, vmid(len(icon)), icon)
    if info:
        c.text(ix + iw + TOP_GAP, vmid(GLYPH_H), info)


def central(transport="ble", profile=1, connected=True, caps=False, left=80, right=45,
            log=(), layer="BASE", os=None):
    c = Canvas()
    if transport == "usb":
        top_row(c, ICONS["usb"], caps=caps, os=os)
    else:
        top_row(c, ICONS[f"die{profile}"], caps=caps, os=os, invert_icon=not connected)
    battery(c, left, right, True)
    keylog(c, list(log))
    w = len(layer) * ADV_X * 2 - 2
    c.text((PW - w) // 2, LAYER_Y, layer, scale=2)
    return c


def peripheral(connected=True, level=64):
    c = Canvas()
    top_row(c, ICONS["bt"], "" if connected else "?")
    battery(c, level, None, False)
    return c


# --- PNG output -----------------------------------------------------------
SCALE = 6
GAP = 24
ON = (235, 245, 255)
OFF = (12, 14, 18)
BEZEL = (40, 42, 48)
PAGE = (250, 250, 250)


def write_png(path, screens):
    pad = 12
    sw, sh = PW * SCALE, PH * SCALE
    W = pad * 2 + len(screens) * (sw + pad * 2) + (len(screens) - 1) * GAP
    H = pad * 4 + sh
    img = [[PAGE] * W for _ in range(H)]
    for i, c in enumerate(screens):
        ox = pad + i * (sw + pad * 2 + GAP)
        oy = pad
        for y in range(oy, oy + sh + pad * 2):
            for x in range(ox, ox + sw + pad * 2):
                img[y][x] = BEZEL
        for y in range(PH):
            for x in range(PW):
                col = ON if c.px[y][x] else OFF
                for dy in range(SCALE):
                    row = img[oy + pad + y * SCALE + dy]
                    for dx in range(SCALE):
                        row[ox + pad + x * SCALE + dx] = col
    raw = b"".join(b"\x00" + bytes(v for p in row for v in p) for row in img)
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    typing = ["HELLO WO", "RLD!", "GIT PUSH", "ZMK"]
    write_png(os.path.join(out, "1-left-ble-typing.png"), [
        central(log=typing, caps=False),
        central(log=typing, caps=True, layer="LO"),
        peripheral(),
    ])
    write_png(os.path.join(out, "2-states.png"), [
        central(transport="usb", left=100, right=None, log=[], layer="HI"),
        central(profile=3, connected=False, left=12, right=90, log=[], layer="CFG"),
        peripheral(connected=False, level=8),
    ])
    print("written to", out)
