
import os
import struct

from PIL import Image, ImageDraw, ImageFont

FONT_SIZE = 6
CELL_H = 8
PAGE_W = PAGE_H = 256
PAGE_SCALE = 2
ATLAS_W = PAGE_W // PAGE_SCALE
ATLAS_H = PAGE_H // PAGE_SCALE
GAP = 1

P8_TO_UNICODE = {
    16: "▮", 17: "■", 18: "□", 19: "⁙", 20: "⁘",
    21: "‖", 22: "◀", 23: "▶", 26: "¥", 27: "•",
    128: "○", 129: "█", 130: "▒", 133: "░", 134: "✽",
    135: "●", 136: "♥", 137: "☉", 138: "웃", 139: "⌂",
    142: "♪", 144: "◆", 145: "…", 147: "★", 148: "⧗",
    150: "ˇ", 151: "∧", 152: "❎", 153: "▤", 154: "▥",
}


def rasterise(font, ch):
    adv = int(round(font.getlength(ch)))
    if adv <= 0:
        return None, 0
    img = Image.new("L", (adv + 8, CELL_H + 8), 0)
    ImageDraw.Draw(img).text((0, 0), ch, fill=255, font=font)
    px = img.load()
    rows = []
    for y in range(CELL_H):
        rows.append([1 if px[x, y] > 127 else 0 for x in range(adv)])
    return rows, adv


def build():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
    path = os.path.join(root, "pico-8.otf")
    font = ImageFont.truetype(path, FONT_SIZE)

    wanted = [(c, chr(c)) for c in range(32, 127)]
    wanted += [(c, u) for c, u in sorted(P8_TO_UNICODE.items())]

    page = bytearray(PAGE_W * PAGE_H)
    glyphs = []
    charmap = [0xFF] * 256
    shelf_x = shelf_y = 0
    missing = []

    for code, ch in wanted:
        rows, adv = rasterise(font, ch)
        if rows is None or not any(any(r) for r in rows):
            if ch != " ":
                missing.append(code)
                continue
        if shelf_x + adv > ATLAS_W - 1:
            shelf_x = 0
            shelf_y += CELL_H + GAP
        if shelf_y + CELL_H > ATLAS_H - 1:
            raise SystemExit("font atlas overflow")
        for y in range(CELL_H):
            for x in range(adv):
                t = rows[y][x] if rows else 0
                for sy in range(PAGE_SCALE):
                    row = (PAGE_SCALE * (shelf_y + y) + sy) * PAGE_W
                    for sx in range(PAGE_SCALE):
                        page[row + PAGE_SCALE * (shelf_x + x) + sx] = t
        charmap[code] = len(glyphs)
        glyphs.append((shelf_x, shelf_y, adv, adv))
        shelf_x += adv + GAP

    if missing:
        print("   note: {} characters absent from the font: {}".format(
            len(missing), missing))

    img = bytearray(PAGE_W * PAGE_H // 2)
    for i in range(0, PAGE_W * PAGE_H, 2):
        img[i >> 1] = (page[i] & 0xF) | ((page[i + 1] & 0xF) << 4)

    table = bytearray()
    table += struct.pack("<BBBB", FONT_SIZE, CELL_H, len(glyphs), 0)
    table += bytes(charmap)
    for u, v, w, adv in glyphs:
        table += struct.pack("<BBBB", u, v, w, adv)

    print("   font: {} glyphs, {} of {} native rows used, stored at {}x".format(
        len(glyphs), shelf_y + CELL_H, ATLAS_H, PAGE_SCALE))
    return bytes(table), bytes(img)


if __name__ == "__main__":
    t, i = build()
    print("FONT {} bytes, FONTPG {} bytes".format(len(t), len(i)))
