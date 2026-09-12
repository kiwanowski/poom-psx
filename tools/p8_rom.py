
import re
import os

ROM_SIZE = 0x4300

GFX_ADDR = 0x0000
MAP_ADDR = 0x2000
GFF_ADDR = 0x3000
MUS_ADDR = 0x3100
SFX_ADDR = 0x3200


def _sections(path):
    out = {}
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n\r")
            m = re.match(r"^__([a-z0-9]+)__\s*$", line)
            if m:
                cur = m.group(1)
                out.setdefault(cur, [])
                continue
            if cur is not None:
                out[cur].append(line)
    return out


def _hexlines(lines, width):
    for line in lines:
        line = line.strip()
        if not line:
            continue
        if len(line) != width or not re.fullmatch(r"[0-9a-fA-F]+", line):
            continue
        yield line


def _decode_gfx(lines, rom):
    for y, line in enumerate(_hexlines(lines, 128)):
        if y >= 128:
            break
        base = GFX_ADDR + y * 64
        for x in range(0, 128, 2):
            lo = int(line[x], 16)
            hi = int(line[x + 1], 16)
            rom[base + (x >> 1)] = (hi << 4) | lo


def _decode_map(lines, rom):
    for y, line in enumerate(_hexlines(lines, 256)):
        if y >= 32:
            break
        base = MAP_ADDR + y * 128
        for x in range(128):
            rom[base + x] = int(line[2 * x:2 * x + 2], 16)


def _decode_gff(lines, rom):
    for y, line in enumerate(_hexlines(lines, 256)):
        if y >= 2:
            break
        base = GFF_ADDR + y * 128
        for x in range(128):
            rom[base + x] = int(line[2 * x:2 * x + 2], 16)


def _decode_sfx(lines, rom):
    for i, line in enumerate(_hexlines(lines, 168)):
        if i >= 64:
            break
        base = SFX_ADDR + i * 68
        rom[base + 64] = int(line[0:2], 16)
        rom[base + 65] = int(line[2:4], 16)
        rom[base + 66] = int(line[4:6], 16)
        rom[base + 67] = int(line[6:8], 16)
        for n in range(32):
            off = 8 + n * 5
            pitch = int(line[off:off + 2], 16)
            wave = int(line[off + 2], 16)
            vol = int(line[off + 3], 16)
            eff = int(line[off + 4], 16)
            custom = (wave >> 3) & 1
            wave &= 7
            rom[base + n * 2] = (pitch & 0x3F) | ((wave & 3) << 6)
            rom[base + n * 2 + 1] = (wave >> 2) | (vol << 1) | (eff << 4) | (custom << 7)


def _decode_music(lines, rom):
    for i, line in enumerate(lines):
        line = line.strip()
        if not line:
            continue
        m = re.fullmatch(r"([0-9a-fA-F]{2}) ([0-9a-fA-F]{8})", line)
        if not m:
            continue
        if i >= 64:
            break
        flags = int(m.group(1), 16)
        chans = [int(m.group(2)[2 * c:2 * c + 2], 16) for c in range(4)]
        for b in range(4):
            if flags & (1 << b):
                chans[b] |= 0x80
        base = MUS_ADDR + i * 4
        for c in range(4):
            rom[base + c] = chans[c]


def read_rom(path):
    rom = bytearray(ROM_SIZE)
    sec = _sections(path)
    if "gfx" in sec:
        _decode_gfx(sec["gfx"], rom)
    if "map" in sec:
        _decode_map(sec["map"], rom)
    if "gff" in sec:
        _decode_gff(sec["gff"], rom)
    if "sfx" in sec:
        _decode_sfx(sec["sfx"], rom)
    if "music" in sec:
        _decode_music(sec["music"], rom)
    return rom


class CartStream:

    def __init__(self, carts_dir, mod_name, cart_id=0, mem=0):
        self.dir = carts_dir
        self.mod = mod_name
        self.cart_id = cart_id
        self.mem = mem
        self._cache = {}
        self.rom = self._rom(cart_id)

    def _rom(self, cart_id):
        if cart_id not in self._cache:
            path = os.path.join(self.dir, "{}_{}.p8".format(self.mod, cart_id))
            self._cache[cart_id] = read_rom(path)
        return self._cache[cart_id]

    def peek(self):
        if self.mem > 0x42FF:
            self.cart_id += 1
            self.mem = 0
            self.rom = self._rom(self.cart_id)
        b = self.rom[self.mem]
        self.mem += 1
        return b
