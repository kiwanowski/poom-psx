
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from p8_rom import read_rom, CartStream
from poom_unpack import (Reader, unpack_actors, unpack_map, fx_to_float,
                         ACTION_FUNCS, STATE_LABELS)
from poom_gfx import CartGfx, sprite_tile_pixels, PICO8_RGB
from build_font import build as build_pico8_font

CARTS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
BUILD = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build")

MAPS = [
    ("E1M1", "e1", 6, 11232), ("E1M2", "e1", 7, 15551), ("E1M3", "e1", 11, 10408),
    ("E2M1", "e2", 14, 13277), ("E2M2", "e2", 19, 4019), ("E2M3", "e2", 23, 14557),
]

AMMO_ICON_OVERRIDE = {
    2008: 154,
    2007: 153,
    2010: 26,
    2047: 151,
}

SECTOR_SIZE = 2048
PAGE_W = 256
PAGE_H = 256
FLAT_GRID = 64
SKY_SRC_H = 56
SCREEN_H = 240
SKY_SCALE = SCREEN_H / 128.0



class AtlasPage:
    def __init__(self):
        self.px = bytearray(PAGE_W * PAGE_H)
        self.shelf_y = 0
        self.shelf_h = 0
        self.shelf_x = 0

    def blit(self, x, y, pixels):
        for j, row in enumerate(pixels):
            base = (y + j) * PAGE_W + x
            self.px[base:base + len(row)] = bytes(row)

    def to_4bpp(self):
        out = bytearray(PAGE_W * PAGE_H // 2)
        for i in range(0, PAGE_W * PAGE_H, 2):
            out[i >> 1] = (self.px[i] & 0xF) | ((self.px[i + 1] & 0xF) << 4)
        return bytes(out)


class Atlas:

    def __init__(self):
        self.pages = [AtlasPage()]

    def alloc(self, w, h, align=False):
        for pi, p in enumerate(self.pages):
            x = p.shelf_x
            if align:
                x = (x + w - 1) // w * w
            if x + w <= PAGE_W and p.shelf_y + max(h, p.shelf_h) <= PAGE_H and h <= PAGE_H:
                if h <= p.shelf_h or p.shelf_x == 0:
                    y = p.shelf_y
                    if align:
                        y = (y + h - 1) // h * h
                    if y + h <= PAGE_H:
                        p.shelf_x = x + w
                        p.shelf_h = max(p.shelf_h, y + h - p.shelf_y)
                        return pi, x, y
            ny = p.shelf_y + p.shelf_h
            if align:
                ny = (ny + h - 1) // h * h
            if ny + h <= PAGE_H:
                x = 0
                p.shelf_y = ny
                p.shelf_h = h
                p.shelf_x = w
                return pi, x, ny
        self.pages.append(AtlasPage())
        return self.alloc(w, h, align)

    def place(self, pixels, align=False):
        h = len(pixels)
        w = len(pixels[0])
        pi, x, y = self.alloc(w, h, align)
        self.pages[pi].blit(x, y, pixels)
        return pi, x, y



def bgr555(rgb, transparent=False):
    if transparent:
        return 0
    r, g, b = rgb
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | 0x8000


def build_cluts(gfx, variants):
    data = bytearray()
    for var in variants:
        for light in range(16):
            cols = gfx.clut_rgb(light)
            for c in range(16):
                data += struct.pack("<H", bgr555(cols[c], transparent=(c == var)))
    screen = gfx.pain_ramp(0)
    for c in range(16):
        entries = [0, bgr555(PICO8_RGB[screen[c] & 0xFF])] + [0] * 14
        for e in entries:
            data += struct.pack("<H", e)
    return bytes(data)



def clip_convex(poly, nx, ny, d):
    out = []
    n = len(poly)
    for i in range(n):
        a = poly[i]
        b = poly[(i + 1) % n]
        da = nx * a[0] + ny * a[1] - d
        db = nx * b[0] + ny * b[1] - d
        if da <= 0:
            out.append(a)
        if (da < 0) != (db < 0) and da != db:
            t = da / (da - db)
            out.append((a[0] + t * (b[0] - a[0]), a[1] + t * (b[1] - a[1])))
    return out


def subdivide_flat(poly, grid=FLAT_GRID):
    xs = [p[0] for p in poly]
    ys = [p[1] for p in poly]
    gx0 = math.floor(min(xs) / grid)
    gx1 = math.floor((max(xs) - 1e-6) / grid)
    gy0 = math.floor(min(ys) / grid)
    gy1 = math.floor((max(ys) - 1e-6) / grid)
    out = []
    for gy in range(gy0, gy1 + 1):
        for gx in range(gx0, gx1 + 1):
            p = poly
            p = clip_convex(p, -1, 0, -gx * grid)
            if p:
                p = clip_convex(p, 1, 0, (gx + 1) * grid)
            if p:
                p = clip_convex(p, 0, -1, -gy * grid)
            if p:
                p = clip_convex(p, 0, 1, (gy + 1) * grid)
            if p and len(p) >= 3:
                area = 0.0
                for i in range(len(p)):
                    a, b = p[i], p[(i + 1) % len(p)]
                    area += a[0] * b[1] - b[0] * a[1]
                if abs(area) > 1.0:
                    out.append(p)
    return out



RUNTIME_MAX_POLY = 32
FLAT_MAX_VERTS = RUNTIME_MAX_POLY - 3


def flat_uv(x):
    q = x >> 16
    return q // 2 if q >= 0 else -((-q) // 2)


def strip_cell(size):
    cell = (2 * (256 - size)) // FLAT_GRID * FLAT_GRID
    if cell < FLAT_GRID:
        raise SystemExit("a {} texel texture cannot fit 8 bit UVs even on the "
                         "flat grid".format(size))
    return cell


def cut_strips(poly, gu, gv):
    xs = [p[0] for p in poly]
    ys = [p[1] for p in poly]
    rx = (range(math.floor(min(xs) / gu), math.floor((max(xs) - 1e-6) / gu) + 1)
          if gu else [None])
    ry = (range(math.floor(min(ys) / gv), math.floor((max(ys) - 1e-6) / gv) + 1)
          if gv else [None])
    out = []
    for gy in ry:
        for gx in rx:
            p = poly
            if gx is not None:
                p = clip_convex(p, -1, 0, -gx * gu)
                if p:
                    p = clip_convex(p, 1, 0, (gx + 1) * gu)
            if p and gy is not None:
                p = clip_convex(p, 0, -1, -gy * gv)
                if p:
                    p = clip_convex(p, 0, 1, (gy + 1) * gv)
            if p and len(p) >= 3:
                area = 0.0
                for i in range(len(p)):
                    a, b = p[i], p[(i + 1) % len(p)]
                    area += a[0] * b[1] - b[0] * a[1]
                if abs(area) > 1.0:
                    out.append(p)
    return out



class Archive:
    def __init__(self):
        self.lumps = []

    def add(self, name, data):
        assert len(name) <= 11
        self.lumps.append((name, data))

    def write(self, path):
        header_size = 16 + 20 * len(self.lumps)
        header_size = (header_size + SECTOR_SIZE - 1) // SECTOR_SIZE * SECTOR_SIZE
        blob = bytearray()
        entries = []
        ofs = header_size
        for name, data in self.lumps:
            entries.append((name, ofs, len(data)))
            padded = data + b"\0" * (-len(data) % SECTOR_SIZE)
            blob += padded
            ofs += len(padded)
        head = bytearray()
        head += b"POOM"
        head += struct.pack("<III", 1, len(self.lumps), 0)
        for name, o, s in entries:
            head += name.encode("ascii").ljust(12, b"\0")
            head += struct.pack("<II", o, s)
        head += b"\0" * (header_size - len(head))
        with open(path, "wb") as f:
            f.write(bytes(head) + bytes(blob))
        print("wrote {} ({} lumps, {} KB)".format(
            os.path.basename(path), len(self.lumps),
            (header_size + len(blob)) // 1024))
        for name, o, s in entries:
            print("   {:<12} {:>8} bytes @ {}".format(name, s, o))



def build_sprites(frames, tiles, variants):
    atlas = Atlas()
    uniq = {}
    frame_map = []
    records = []

    for f in frames:
        sig = (f.w, f.transparent, tuple(sorted(f.tiles.items())))
        idx = uniq.get(sig)
        if idx is None:
            idx = len(records)
            uniq[sig] = idx
            w = max(1, f.w)
            h = max(1, f.h)
            px = [[f.transparent] * (w * 16) for _ in range(h * 16)]
            for gi, tid in f.tiles.items():
                tp = sprite_tile_pixels(tiles, tid)
                ox = (gi % w) * 16
                oy = (gi // w) * 16
                for y in range(16):
                    row = px[oy + y]
                    for x in range(16):
                        row[ox + x] = tp[y][x]
            page, ax, ay = atlas.place(px)
            records.append({
                "page": page, "u": ax, "v": ay, "w": w * 16, "h": h * 16,
                "xoffset": f.xoffset, "yoffset": f.yoffset,
                "variant": variants.index(f.transparent),
            })
        frame_map.append(idx)

    fram = bytearray()
    for r in records:
        fram += struct.pack("<BBBBBBhh",
                            r["page"], r["u"], r["v"], r["w"] - 1, r["h"] - 1,
                            r["variant"],
                            int(round(r["xoffset"] * 16)),
                            int(round(r["yoffset"] * 16)))
    frmx = b"".join(struct.pack("<H", i) for i in frame_map)
    return atlas, bytes(fram), frmx, len(records)



def build_actors(actors, frame_map):
    ids = sorted(actors)
    index_of = {aid: i for i, aid in enumerate(ids)}

    def aref(actor_id):
        if not actor_id:
            return 0xFFFF
        return index_of.get(actor_id, 0xFFFF)

    states = bytearray()
    state_base = []
    for aid in ids:
        a = actors[aid]
        state_base.append(len(state_base) and 0 or 0)

    offsets = {}
    total = 0
    for aid in ids:
        offsets[aid] = total
        total += len(actors[aid].states)

    for aid in ids:
        a = actors[aid]
        for st in a.states:
            if st.stop:
                kind, arg = 0, 0
            elif st.jmp is not None:
                kind, arg = 1, st.jmp
            else:
                kind, arg = 2, 0
            fn_id = 0
            fa = [0, 0, 0, 0, 0]
            if st.func:
                _, fn_id, args = st.func
                for i, v in enumerate(args[:5]):
                    if ACTION_FUNCS[fn_id][1][i] == "actorref":
                        v = aref(v)
                    elif ACTION_FUNCS[fn_id][1][i] == "ratio":
                        v = int(round(v * 255))
                    fa[i] = int(v)
            sides = [frame_map[f] if 0 <= f < len(frame_map) else 0
                     for f in st.frames]
            sides = (sides + [0] * 8)[:8]
            states += struct.pack("<BbBBBB", kind, st.ticks, arg,
                                  1 if st.bright else 0, st.flipbits,
                                  min(len(st.frames), 8))
            states += struct.pack("<8H", *sides)
            states += struct.pack("<B", fn_id)
            states += struct.pack("<B", 0)
            states += struct.pack("<5i", *fa)

    actr = bytearray()
    for aid in ids:
        a = actors[aid]
        labels = [0xFFFF] * len(STATE_LABELS)
        for lbl, addr in a.labels.items():
            if lbl < len(labels):
                labels[lbl] = addr - 1
        p = a.props
        actr += struct.pack("<HHii", aid, a.kind, a.radius, a.height)
        actr += struct.pack("<HH", a.flags, offsets[aid])
        actr += struct.pack("<HH", len(a.states), aref(p.get("ammotype")))
        actr += struct.pack("<HH", aref(p.get("trailtype")), p.get("health", 0))
        actr += struct.pack("<HH", p.get("armor", 0), p.get("amount", 0))
        actr += struct.pack("<HH", p.get("maxamount", 0), p.get("ammouse", 0))
        actr += struct.pack("<HH", p.get("speed", 0), p.get("damage", 0))
        actr += struct.pack("<HH", p.get("mass", 100), p.get("ammogive", 0))
        actr += struct.pack("<HH", p.get("meleerange", 0),
                            p.get("maxtargetrange", 0))
        actr += struct.pack("<BBBB", p.get("slot", 0),
                            AMMO_ICON_OVERRIDE.get(aid, p.get("icon", 0)),
                            p.get("hudcolor", 7), 0)
        actr += struct.pack("<BBBB", p.get("pickupsound", 0),
                            p.get("attacksound", 0), p.get("deathsound", 0),
                            len(a.startitems))
        actr += struct.pack("<i", p.get("drag", 0))
        items = [(aref(r), n) for r, n in a.startitems][:8]
        items = (items + [(0xFFFF, 0)] * 8)[:8]
        for r, n in items:
            actr += struct.pack("<HH", r, n)
        actr += struct.pack("<16H", *labels[:12], 0, 0, 0, 0)
    return bytes(actr), bytes(states), ids, index_of



def build_textures(gfx, texs, sky_rows):
    atlas = Atlas()
    table = {}
    for raw, t in sorted(texs.items(), key=lambda kv: -(kv[1].w * kv[1].h)):
        w, h = t.w * 8, t.h * 8
        if (w & (w - 1)) or (h & (h - 1)):
            raise SystemExit("texture {} is not power of two - the GPU texture "
                             "window cannot repeat it".format(t))
        px = gfx.texture_pixels(t)
        page, x, y = atlas.place(px, align=True)
        table[raw] = (page, x, y, w, h)

    sky = [[0] * 8 for _ in range(PAGE_H)]
    for y in range(PAGE_H):
        src = min(int(y / SKY_SCALE), len(sky_rows) - 1)
        sky[y] = [sky_rows[src]] * 8
    sky_page, sky_x, sky_y = atlas.place(sky, align=True)

    order = sorted(table)
    tex_index = {raw: i for i, raw in enumerate(order)}
    tbl = bytearray()
    for raw in order:
        page, x, y, w, h = table[raw]
        tbl += struct.pack("<BBBBBB", page, x, y, w - 1, h - 1, 0)
    return atlas, bytes(tbl), tex_index, (sky_page, sky_x, sky_y)



def build_level(m, tex_index, tex_sizes, actor_index):
    def tref(t):
        if not t:
            return 0xFFFF
        return tex_index.get(t.raw, 0xFFFF)

    sectors = bytearray()
    for s in m["sectors"]:
        sectors += struct.pack("<iiHHBBH", s.floor, s.ceil,
                               tref(s.floortex), tref(s.ceiltex),
                               int(round(s.lightlevel * 255)), s.special, 0)

    sides = bytearray()
    for s in m["sides"]:
        sides += struct.pack("<HHHH", s.sector.id, tref(s.toptex),
                             tref(s.midtex), tref(s.bottomtex))

    verts = bytearray()
    for v in m["verts"]:
        verts += struct.pack("<ii", v[0], v[1])

    lines = bytearray()
    specials = bytearray()
    special_ofs = []
    for ln in m["lines"]:
        sp = 0xFFFF
        if ln.special:
            sp = len(special_ofs)
            special_ofs.append(len(specials))
            specials += encode_special(ln.special, actor_index)
        lines += struct.pack("<HHHH",
                             0xFFFF if ln.front is None else ln.front.id,
                             0xFFFF if ln.back is None else ln.back.id,
                             ln.flags, sp)

    def fx(v):
        return int(round(v * 65536))

    segs = bytearray()
    subs = bytearray()
    flats = bytearray()
    flat_verts = bytearray()
    nseg = 0
    nflat = 0
    nfv = 0
    stats = dict(strips=0, surfaces=0, subs=0, strip_tris=0, grid_tris=0,
                 max_strip_verts=0, max_segs=0)
    for ss in m["subs"]:
        first = nseg
        for sg in ss.segs:
            segs += struct.pack("<HHHBB", sg.v,
                                0xFFFF if sg.line < 0 else sg.line,
                                0xFFFF if sg.partner < 0 else sg.partner,
                                1 if sg.side else 0,
                                1 if sg.major == "v" else 0)
            segs += struct.pack("<iiiiiii", fx(sg.dirx), fx(sg.diry),
                                fx(sg.ddist), fx(sg.length), fx(sg.nx),
                                fx(sg.ny), fx(sg.ndist))
            nseg += 1
        poly = [(fx_to_float(m["verts"][s.v][0]),
                 fx_to_float(m["verts"][s.v][1])) for s in ss.segs]
        ff = nflat
        grid_pieces = subdivide_flat(poly)
        for piece in grid_pieces:
            if len(piece) > FLAT_MAX_VERTS:
                raise SystemExit("sub-sector {}: a grid piece has {} vertices, and the "
                                 "renderer takes {}".format(ss.id, len(piece), FLAT_MAX_VERTS))
        for piece in grid_pieces:
            flats += struct.pack("<HH", nfv, len(piece))
            for (px, py) in piece:
                flat_verts += struct.pack("<ii", fx(px), fx(py))
                nfv += 1
            nflat += 1
        grid = nflat - ff

        stats["max_segs"] = max(stats["max_segs"], len(ss.segs))
        outline = [m["verts"][s.v] for s in ss.segs]
        us = [flat_uv(v[0]) for v in outline]
        vs = [flat_uv(v[1]) for v in outline]
        rerouted = []
        gu = gv = None
        if ss.sector is not None and 3 <= len(ss.segs) <= FLAT_MAX_VERTS:
            for t in (ss.sector.floortex, ss.sector.ceiltex):
                idx = tref(t)
                if idx == 0xFFFF:
                    continue
                w, h = tex_sizes[idx]
                over_u = (max(us) - min(us)) + (w - 1) > 255
                over_v = (max(vs) - min(vs)) + (h - 1) > 255
                if over_u:
                    gu = min(gu, strip_cell(w)) if gu else strip_cell(w)
                if over_v:
                    gv = min(gv, strip_cell(h)) if gv else strip_cell(h)
                if over_u or over_v:
                    rerouted.append((w, h))
        strips = cut_strips(poly, gu, gv) if rerouted else []
        for piece in strips:
            if len(piece) > FLAT_MAX_VERTS:
                raise SystemExit("sub-sector {}: a strip has {} vertices, and the "
                                 "renderer takes {}".format(ss.id, len(piece),
                                                            FLAT_MAX_VERTS))
            pu = [flat_uv(fx(px)) for (px, _py) in piece]
            pv = [flat_uv(fx(py)) for (_px, py) in piece]
            for (w, h) in rerouted:
                if ((max(pu) - min(pu)) + (w - 1) > 255 or
                        (max(pv) - min(pv)) + (h - 1) > 255):
                    raise SystemExit("sub-sector {}: a strip still overflows 8 bit "
                                     "UVs".format(ss.id))
            flats += struct.pack("<HH", nfv, len(piece))
            for (px, py) in piece:
                flat_verts += struct.pack("<ii", fx(px), fx(py))
                nfv += 1
            nflat += 1
        if strips:
            stats["strips"] += len(strips)
            stats["surfaces"] += len(rerouted)
            stats["subs"] += 1
            stats["strip_tris"] += sum(len(p) - 2 for p in strips)
            stats["grid_tris"] += sum(len(p) - 2 for p in grid_pieces)
            stats["max_strip_verts"] = max(stats["max_strip_verts"],
                                           max(len(p) for p in strips))
        subs += struct.pack("<HHHHHH", first, len(ss.segs),
                            0xFFFF if ss.sector is None else ss.sector.id,
                            ff, grid, len(strips))

    m["strip_stats"] = stats

    n = len(m["subs"])
    stride = (n + 31) // 32 * 4
    pvs = bytearray(stride * n)
    for ss in m["subs"]:
        for other in ss.pvs:
            if 0 <= other < n:
                pvs[ss.id * stride + (other >> 3)] |= 1 << (other & 7)
        pvs[ss.id * stride + (ss.id >> 3)] |= 1 << (ss.id & 7)

    nodes = bytearray()
    for nd in m["nodes"]:
        nodes += struct.pack("<iii", nd.nx, nd.ny, nd.d)
        for side in (0, 1):
            kind, idx = nd.child[side]
            nodes += struct.pack("<HH", idx, 1 if kind == "sub" else 0)
        for side in (0, 1):
            if nd.bbox[side] is None:
                nodes += struct.pack("<hhhh", 0, 0, 0, 0)
            else:
                t, b, l, r = nd.bbox[side]
                nodes += struct.pack("<hhhh", t >> 16, b >> 16, l >> 16, r >> 16)

    things = bytearray()
    for t in m["things"]:
        sp = 0xFFFF
        if t.special:
            sp = len(special_ofs)
            special_ofs.append(len(specials))
            specials += encode_special(t.special, actor_index)
        things += struct.pack("<HHiiBBH", actor_index.get(t.actor, 0xFFFF), 0,
                              t.x, t.y, int(round(t.angle * 256)) & 0xFF,
                              t.skills, sp)

    onoff = bytearray()
    for raw, other in m["onoff"].items():
        a = tex_index.get(raw, 0xFFFF)
        b = tex_index.get(other.raw, 0xFFFF)
        if a != 0xFFFF and b != 0xFFFF and a != b:
            onoff += struct.pack("<HH", a, b)

    transparent = bytearray()
    for raw in sorted(m["transparent"]):
        i = tex_index.get(raw, 0xFFFF)
        if i != 0xFFFF:
            transparent += struct.pack("<H", i)

    spofs = b"".join(struct.pack("<I", o) for o in special_ofs)

    blocks = [sectors, sides, verts, lines, segs, subs, flats, flat_verts,
              pvs, nodes, things, onoff, transparent, spofs, bytes(specials)]
    counts = [len(m["sectors"]), len(m["sides"]), len(m["verts"]),
              len(m["lines"]), nseg, len(m["subs"]), nflat, nfv,
              n, len(m["nodes"]), len(m["things"]), len(onoff) // 4,
              len(transparent) // 2, len(special_ofs), len(specials)]

    head = bytearray()
    head += b"PLVL"
    head += struct.pack("<I", stride)
    ofs = 8 + 4 * len(blocks) * 2
    ofs = (ofs + 3) & ~3
    body = bytearray()
    table = bytearray()
    for blk, cnt in zip(blocks, counts):
        table += struct.pack("<II", ofs + len(body), cnt)
        body += blk
        body += b"\0" * (-len(body) % 4)
    return bytes(head) + bytes(table) + b"\0" * (ofs - 8 - len(table)) + bytes(body)


SPECIAL_MOVE, SPECIAL_LIGHT, SPECIAL_EXIT = 0, 1, 2


def encode_special(sp, actor_index=None):
    out = bytearray()
    if sp["kind"] == "move":
        lock = sp["lock"]
        if not lock:
            lock = 0xFFFF
        elif actor_index is not None:
            lock = actor_index.get(lock, 0xFFFF)
        out += struct.pack("<BBHHHiHBB", SPECIAL_MOVE,
                           1 if sp["what"] == "ceil" else 0,
                           sp["trigger_delay"], sp["delay"],
                           len(sp["targets"]),
                           int(round(sp["speed"] * 65536)),
                           lock,
                           1 if sp["special"] == 13 else 0,
                           0)
        for sec, target in sp["targets"]:
            out += struct.pack("<HHi", sec.id if sec else 0xFFFF, 0, target)
    elif sp["kind"] == "light":
        out += struct.pack("<BBH", SPECIAL_LIGHT,
                           int(round(sp["lightlevel"] * 255)),
                           len(sp["targets"]))
        for sec in sp["targets"]:
            out += struct.pack("<H", sec.id if sec else 0xFFFF)
    else:
        out += struct.pack("<BBH", SPECIAL_EXIT, 0, sp["delay"])
    out += b"\0" * (-len(out) % 4)
    return bytes(out)



def main():
    os.makedirs(BUILD, exist_ok=True)
    ar = Archive()

    print("== actors & sprites ==")
    actors, frames, tiles = unpack_actors(Reader(CartStream(CARTS, "poom", 0, 0)))
    variants = sorted({f.transparent for f in frames})
    print("transparency variants: {}".format(variants))

    sprite_atlas, fram, frmx, nuniq = build_sprites(frames, tiles, variants)
    print("unique frames {} in {} atlas pages".format(nuniq, len(sprite_atlas.pages)))

    gfx_e1 = CartGfx(read_rom(os.path.join(CARTS, "poom_e1.p8")))
    ar.add("CLUT", build_cluts(gfx_e1, [-1] + variants))

    print("== font ==")
    font_table, font_page = build_pico8_font()
    ar.add("FONT", font_table)
    ar.add("FONTPG", font_page)

    for i, p in enumerate(sprite_atlas.pages):
        ar.add("SPR{}".format(i), p.to_4bpp())
    ar.add("FRAM", fram)
    ar.add("FRMX", frmx)

    actr, stat, ids, actor_index = build_actors(actors, [i for i in
                                                        struct.unpack("<{}H".format(len(frmx) // 2), frmx)])
    ar.add("ACTR", actr)
    ar.add("STAT", stat)
    print("actors {} states {} bytes".format(len(ids), len(stat)))

    levels = {}
    for name, group, cart, offset in MAPS:
        levels[name] = (group, unpack_map(Reader(CartStream(CARTS, "poom", cart, offset))))

    for group in ("e1", "e2"):
        gfx = CartGfx(read_rom(os.path.join(CARTS, "poom_{}.p8".format(group))))
        texs = {}
        for name, g, m in [(n, g, mm) for n, (g, mm) in levels.items() if g == group]:
            for s in m["sectors"]:
                for t in (s.ceiltex, s.floortex):
                    if t:
                        texs[t.raw] = t
            for s in m["sides"]:
                for t in (s.toptex, s.midtex, s.bottomtex):
                    if t:
                        texs[t.raw] = t
            for _n, _g, mm in [(n, g, m2) for n, (g, m2) in levels.items()
                               if g == group]:
                for raw, other in mm["onoff"].items():
                    if raw in texs and other.raw not in texs:
                        texs[other.raw] = other
        sky_rows = gfx.sky_column(SKY_SRC_H - 1)
        atlas, tbl, tex_index, sky = build_textures(gfx, texs, sky_rows)
        tex_sizes = [(tbl[i * 6 + 3] + 1, tbl[i * 6 + 4] + 1) for i in range(len(tbl) // 6)]
        print("{}: {} textures in {} pages, sky at page {} ({},{})".format(
            group.upper(), len(texs), len(atlas.pages), *sky))
        for i, p in enumerate(atlas.pages):
            ar.add("{}TX{}".format(group.upper(), i), p.to_4bpp())
        ar.add("{}TBL".format(group.upper()),
               struct.pack("<BBBBBB", sky[0], sky[1], sky[2], 7, PAGE_H - 1, 0) + tbl)

        for name, (g, m) in levels.items():
            if g != group:
                continue
            data = build_level(m, tex_index, tex_sizes, actor_index)
            ar.add(name, data)
            st = m["strip_stats"]
            print("   {} {} KB, {} strips for {} surfaces in {} sub-sectors: {} triangles "
                  "where the grid has {}; largest strip {} vertices, largest outline "
                  "{} segs".format(name, len(data) // 1024, st["strips"], st["surfaces"],
                                   st["subs"], st["strip_tris"], st["grid_tris"],
                                   st["max_strip_verts"], st["max_segs"]))

    ar.write(os.path.join(BUILD, "POOM.DAT"))


if __name__ == "__main__":
    main()
