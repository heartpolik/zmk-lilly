"""Static 1-bit art for the right (peripheral) OLED, 32 px wide.

Shapes are built from simple geometry (circles/ellipses/polygons), rasterized,
and optionally reduced to outlines. Each function returns a list of strings
('#' = lit) of width 32.
"""

import math

W = 32


def blank(h):
    return [[0] * W for _ in range(h)]


def to_rows(g):
    return ["".join("#" if v else "." for v in row) for row in g]


def outline(mask):
    h = len(mask)
    out = blank(h)
    for y in range(h):
        for x in range(W):
            if not mask[y][x]:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if not (0 <= nx < W and 0 <= ny < h) or not mask[ny][nx]:
                    out[y][x] = 1
                    break
    return out


def ellipse(g, cx, cy, rx, ry, on=1):
    for y in range(len(g)):
        for x in range(W):
            if ((x + 0.5 - cx) / rx) ** 2 + ((y + 0.5 - cy) / ry) ** 2 <= 1:
                g[y][x] = on


def polygon(g, pts, on=1):
    for y in range(len(g)):
        for x in range(W):
            px_, py_ = x + 0.5, y + 0.5
            inside = False
            j = len(pts) - 1
            for i in range(len(pts)):
                xi, yi = pts[i]
                xj, yj = pts[j]
                if (yi > py_) != (yj > py_) and px_ < (xj - xi) * (py_ - yi) / (yj - yi) + xi:
                    inside = not inside
                j = i
            if inside:
                g[y][x] = on


def plot(g, x, y, on=1):
    x, y = int(round(x)), int(round(y))
    if 0 <= x < W and 0 <= y < len(g):
        g[y][x] = on


def arc(g, cx, cy, r, a0, a1, on=1, step=1.0):
    a = a0
    while a <= a1:
        plot(g, cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a)), on)
        a += step


def bezier(g, pts, on=1, steps=200):
    n = len(pts) - 1
    for i in range(steps + 1):
        t = i / steps
        x = y = 0.0
        for k, (px_, py_) in enumerate(pts):
            b = math.comb(n, k) * (1 - t) ** (n - k) * t ** k
            x += b * px_
            y += b * py_
        plot(g, x, y, on)


def line(g, x0, y0, x1, y1, on=1):
    n = int(max(abs(x1 - x0), abs(y1 - y0))) + 1
    for i in range(n + 1):
        t = i / n
        plot(g, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, on)


# --------------------------------------------------------------------- cat --
def cat():
    """Sitting cat, full 32x128 screen, eyes closed, tail on the right."""
    h = 128
    m = blank(h)
    ellipse(m, 14, 98, 12, 24)   # body
    ellipse(m, 14, 76, 9, 14)    # chest
    ellipse(m, 14, 52, 10, 10)   # head
    polygon(m, [(5, 50), (6, 35), (12, 44)])    # left ear
    polygon(m, [(23, 50), (22, 35), (16, 44)])  # right ear
    g = outline(m)
    line(g, 7, 41, 8, 46)
    line(g, 21, 41, 20, 46)
    arc(g, 10, 52, 2.2, 20, 160)
    arc(g, 18, 52, 2.2, 20, 160)
    plot(g, 14, 56)
    plot(g, 13, 57); plot(g, 15, 57)
    line(g, 0, 55, 6, 57); line(g, 0, 60, 6, 59)
    line(g, 22, 57, 28, 55); line(g, 22, 59, 28, 60)
    line(g, 10, 98, 10, 120)
    line(g, 18, 98, 18, 120)
    # tail: leaves the body at the bottom right, rises along the side, tip curls
    bezier(g, [(22, 116), (31, 116), (31, 100), (29, 88)])
    bezier(g, [(29, 88), (28, 82), (31, 80), (31, 84)])
    return to_rows(g)


# -------------------------------------------------------------------- fuji --
def fuji():
    """Mount Fuji with sun, full 32x128 screen."""
    h = 128
    g = blank(h)
    sun = blank(h)
    ellipse(sun, 22, 22, 8, 8)
    for y, row in enumerate(outline(sun)):
        for x, v in enumerate(row):
            if v:
                g[y][x] = 1
    # small clouds
    line(g, 2, 44, 12, 44); line(g, 6, 47, 15, 47)
    top, base = 60, 104
    m = blank(h)
    polygon(m, [(12, top), (20, top), (32, base), (0, base)])
    edge = outline(m)
    for y in range(h):
        for x in range(W):
            if edge[y][x] and y < base - 1:
                g[y][x] = 1
    snow = [72, 76, 71, 77, 73, 78, 74, 71, 77, 72, 76, 71, 75, 78, 72, 76, 71, 77, 74]
    for y in range(top, base):
        for x in range(W):
            if m[y][x]:
                k = min(int(x * len(snow) / W), len(snow) - 1)
                if y <= snow[k]:
                    g[y][x] = 1
    line(g, 0, base - 1, 31, base - 1)
    for y, x0, x1 in ((108, 3, 13), (108, 18, 28), (112, 8, 23), (116, 12, 19)):
        line(g, x0, y, x1, y)
    return to_rows(g)


# -------------------------------------------------------------------- wave --
def wave():
    """Great-wave inspired curl with a small Fuji, full 32x128 screen."""
    h = 128
    g = blank(h)
    # back of the wave: rises from the sea on the left, rolls over the top
    back = [(0, 104), (0, 66), (1, 38), (14, 32)]
    crest = [(14, 32), (26, 28), (33, 40), (27, 52)]
    bezier(g, back)
    bezier(g, crest)
    # striations following the back
    bezier(g, [(4, 104), (3, 70), (4, 44), (14, 37)])
    bezier(g, [(14, 37), (23, 33), (28, 42), (24, 49)])
    bezier(g, [(8, 104), (7, 76), (8, 52), (15, 43)])
    # curl tip spiralling inward
    arc(g, 23, 52, 4, -40, 220, step=1)
    arc(g, 23, 52, 1.5, 0, 360, step=15)
    # concave front face falling to the sea
    bezier(g, [(19, 52), (12, 56), (10, 80), (13, 104)])
    # foam claws hanging from the crest
    for t in [i / 7 for i in range(1, 7)]:
        x = (1-t)**3*14 + 3*(1-t)**2*t*26 + 3*(1-t)*t**2*33 + t**3*27
        y = (1-t)**3*32 + 3*(1-t)**2*t*28 + 3*(1-t)*t**2*40 + t**3*52
        plot(g, x + 1, y - 1)
        plot(g, x + 2, y - 2)
        plot(g, x + 2, y)
    # small Fuji in the trough
    fm = blank(h)
    polygon(fm, [(21, 88), (24, 88), (31, 104), (15, 104)])
    for y, row in enumerate(outline(fm)):
        for x, v in enumerate(row):
            if v and y < 103:
                g[y][x] = 1
    for x in range(21, 25):
        g[89][x] = 1
    # sea
    line(g, 0, 104, 31, 104)
    for y, x0, x1 in ((108, 15, 29), (112, 2, 12), (112, 18, 27), (116, 8, 22), (120, 13, 18)):
        line(g, x0, y, x1, y)
    return to_rows(g)


# -------------------------------------------------------------- fuji close --
def fuji_summit(sun=True, clouds=True, shade="d25", stars=False, sky_clouds=False, spread=13):
    """Close-up of Fuji's summit: slopes run off both edges, snow gullies."""
    h = 128
    g = blank(h)
    top = 50
    left_x, right_x = 10, 21
    m = blank(h)

    def slope(t):  # steep near the summit, spreading out lower down
        return spread * (0.55 * t + 0.45 * t * t)

    pts = [(left_x, top), (left_x + 2, top - 1), (left_x + 4, top), (left_x + 7, top - 1),
           (left_x + 9, top), (right_x, top)]
    n = 60
    for i in range(1, n + 1):
        t = i / n
        pts.append((right_x + slope(t), top + 34 * t))
    pts += [(40, 128), (-8, 128)]
    for i in range(n, 0, -1):
        t = i / n
        pts.append((left_x - slope(t), top + 34 * t))
    polygon(m, pts)
    for y, row in enumerate(outline(m)):
        for x, v in enumerate(row):
            if v and 0 < x < W - 1 and y < h - 1:
                g[y][x] = 1

    # shade the mountain body so it stands out from the black sky
    for y in range(top, h):
        for x in range(W):
            if not m[y][x]:
                continue
            if shade == "d25":
                on = (x % 2 == 0) and (y % 2 == 0) and ((x // 2 + y // 2) % 2 == 0)
            elif shade == "d50":
                on = (x + y) % 2 == 0
            elif shade == "hatch":
                on = (x + y) % 4 == 0
            else:
                on = False
            if on:
                g[y][x] = 1

    # snow cap with a ragged lower edge
    cap = [66, 69, 65, 70, 67, 64, 71, 66, 68, 65, 69, 66, 64, 70, 67, 65,
           69, 66, 71, 64, 68, 66, 70, 65, 67, 69, 64, 68, 66, 70, 65, 67]
    # long gullies: (x, width, end_y)
    gullies = [(2, 1, 88), (5, 2, 98), (9, 1, 84), (12, 2, 104), (16, 1, 90),
               (19, 2, 100), (23, 1, 86), (26, 2, 96), (29, 1, 90)]
    for y in range(top, h):
        for x in range(W):
            if m[y][x] and y < cap[x]:
                g[y][x] = 1
    for gx, gw, gy in gullies:
        for y in range(60, gy):
            # gullies taper: lose 1px of width in their last third
            w = gw if y < gy - (gy - 60) // 3 else 1
            for x in range(gx, gx + w):
                if 0 <= x < W and m[y][x]:
                    g[y][x] = 1

    if stars:
        # (x, y, size): size 1 = single pixel, 2 = small cross
        for sx, sy, sz in ((4, 8, 2), (15, 4, 1), (27, 11, 1), (21, 19, 2), (8, 24, 1),
                           (29, 30, 1), (3, 36, 1), (13, 33, 2), (24, 40, 1)):
            plot(g, sx, sy)
            if sz == 2:
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    plot(g, sx + dx, sy + dy)
    if sky_clouds:
        # stylized woodblock clouds: rounded bars drifting past the summit
        for y, x0, x1 in ((22, 2, 14), (27, 8, 22), (38, 18, 31), (43, 12, 26)):
            line(g, x0 + 1, y, x1 - 1, y)
            line(g, x0, y + 1, x1, y + 1)
            line(g, x0 + 1, y + 2, x1 - 1, y + 2)
    if sun:
        sm = blank(h)
        ellipse(sm, 23, 18, 6, 6)
        for y, row in enumerate(outline(sm)):
            for x, v in enumerate(row):
                if v:
                    g[y][x] = 1
    if clouds:
        for y, x0, x1 in ((106, 0, 13), (106, 19, 31), (112, 6, 26), (118, 0, 10), (118, 21, 31)):
            for x in range(x0, x1 + 1):
                g[y][x] = 1
                for k in range(1, 4):
                    if y + k < h:
                        g[y + k][x] = 0
    return to_rows(g)


ART = {"cat": cat, "fuji": fuji, "wave": wave, "fuji_summit": fuji_summit}

# The picture shown on the right (peripheral) OLED.
def peripheral_art():
    return fuji_summit(shade="hatch", sun=False, stars=True, spread=19)
