
PICO8_RGB = {
    0: (0x00, 0x00, 0x00), 1: (0x1d, 0x2b, 0x53), 2: (0x7e, 0x25, 0x53),
    3: (0x00, 0x87, 0x51), 4: (0xab, 0x52, 0x36), 5: (0x5f, 0x57, 0x4f),
    6: (0xc2, 0xc3, 0xc7), 7: (0xff, 0xf1, 0xe8), 8: (0xff, 0x00, 0x4d),
    9: (0xff, 0xa3, 0x00), 10: (0xff, 0xec, 0x27), 11: (0x00, 0xe4, 0x36),
    12: (0x29, 0xad, 0xff), 13: (0x83, 0x76, 0x9c), 14: (0xff, 0x77, 0xa8),
    15: (0xff, 0xcc, 0xaa),
    128: (0x29, 0x18, 0x14), 129: (0x11, 0x1d, 0x35), 130: (0x42, 0x21, 0x36),
    131: (0x12, 0x53, 0x59), 132: (0x74, 0x2f, 0x29), 133: (0x49, 0x33, 0x3b),
    134: (0xa2, 0x88, 0x79), 135: (0xf3, 0xef, 0x7d), 136: (0xbe, 0x12, 0x50),
    137: (0xff, 0x6c, 0x24), 138: (0xa8, 0xe7, 0x2e), 139: (0x00, 0xb5, 0x43),
    140: (0x06, 0x5a, 0xb5), 141: (0x75, 0x46, 0x65), 142: (0xff, 0x6e, 0x59),
    143: (0xff, 0x9d, 0x81),
}

PLAYPAL_ADDR = 0x0000
PAINPAL_ADDR = 0x0100
SKY_ADDR = 0x0200
TILES_ADDR = 0x1000
MAP_ADDR = 0x2000


class CartGfx:
    def __init__(self, rom):
        self.rom = rom

    def light_ramp(self, light):
        base = PLAYPAL_ADDR + light * 16
        return list(self.rom[base:base + 16])

    def pain_ramp(self, pain):
        base = PAINPAL_ADDR + pain * 16
        return list(self.rom[base:base + 16])

    def clut_rgb(self, light, pain=0):
        ramp = self.light_ramp(light)
        pal = self.pain_ramp(pain)
        return [PICO8_RGB[pal[c] & 0xFF] for c in ramp]

    def sky_column(self, height):
        return [self.rom[SKY_ADDR + y] & 0x0F for y in range(height + 1)]

    def sprite_pixel(self, index, x, y):
        sx = (index % 16) * 8 + x
        sy = (index // 16) * 8 + y
        b = self.rom[sy * 64 + (sx >> 1)]
        return (b >> 4) if (sx & 1) else (b & 0x0F)

    def tile_pixels(self, index):
        return [[self.sprite_pixel(index, x, y) for x in range(8)]
                for y in range(8)]

    def map_tile(self, mx, my):
        if my < 0 or my >= 32 or mx < 0 or mx >= 128:
            return 0
        return self.rom[MAP_ADDR + my * 128 + mx]

    def texture_pixels(self, tex):
        w, h = tex.w * 8, tex.h * 8
        out = [[0] * w for _ in range(h)]
        for ty in range(tex.h):
            for tx in range(tex.w):
                idx = self.map_tile(tex.mx + tx, tex.my + ty)
                if idx == 0:
                    continue
                px = self.tile_pixels(idx)
                for y in range(8):
                    row = out[ty * 8 + y]
                    src = px[y]
                    for x in range(8):
                        row[tx * 8 + x] = src[x]
        return out


def sprite_tile_pixels(tiles, tile_id):
    out = [[0] * 16 for _ in range(16)]
    base = tile_id * 32
    for row in range(16):
        for half in range(2):
            word = tiles[base + row * 2 + half]
            for k in range(8):
                out[row][half * 8 + k] = (word >> (4 * k)) & 0xF
    return out
