
FIXED_ONE = 1 << 16


def fx_to_float(raw):
    return raw / 65536.0


class Reader:

    def __init__(self, stream):
        self.s = stream
        self.count = 0

    def byte(self):
        self.count += 1
        return self.s.peek()

    def variant(self):
        h = self.byte()
        if h & 0x80:
            return ((h & 0x7F) << 8) | self.byte()
        return h

    def fixed(self):
        v = (self.byte() << 24) | (self.byte() << 16) | (self.byte() << 8) | self.byte()
        if v & 0x80000000:
            v -= 1 << 32
        return v

    def short(self):
        return self.byte() - 128

    def ratio(self):
        return self.byte() / 255.0

    def array(self, fn):
        n = self.variant()
        return [fn(i + 1) for i in range(n)]

    def ref(self, table):
        i = self.variant()
        if i == 0:
            return None
        return table.get(i) if isinstance(table, dict) else table[i - 1]



class Texture:

    __slots__ = ("mx", "my", "w", "h", "raw")

    def __init__(self, raw):
        self.raw = raw & 0xFFFFFFFF
        self.my = (self.raw >> 24) & 0xFF
        self.mx = (self.raw >> 16) & 0xFF
        self.h = (self.raw >> 8) & 0xFF
        self.w = self.raw & 0xFF

    def __bool__(self):
        return self.raw != 0

    def __eq__(self, other):
        return isinstance(other, Texture) and other.raw == self.raw

    def __hash__(self):
        return self.raw

    def __repr__(self):
        if not self.raw:
            return "Texture(none)"
        return "Texture(@{},{} {}x{} tiles)".format(self.mx, self.my, self.w, self.h)


def _texture(r):
    return Texture(r.fixed() & 0xFFFFFFFF)



ACTOR_FLAGS = [
    (0x0001, "is_solid"), (0x0002, "is_shootable"), (0x0004, "is_missile"),
    (0x0008, "is_monster"), (0x0010, "is_nogravity"), (0x0020, "floating"),
    (0x0040, "is_dropoff"), (0x0080, "is_dontfall"), (0x0100, "randomize"),
    (0x0200, "countkill"), (0x0400, "nosectordmg"), (0x0800, "noblood"),
]

ACTOR_PROPS = [
    (0x000001, "health", "variant"), (0x000002, "armor", "variant"),
    (0x000004, "amount", "variant"), (0x000008, "maxamount", "variant"),
    (0x000010, "icon", "byte"), (0x000020, "slot", "byte"),
    (0x000040, "ammouse", "variant"), (0x000080, "speed", "variant"),
    (0x000100, "damage", "variant"), (0x000200, "ammotype", "actorref"),
    (0x000800, "mass", "variant"), (0x001000, "pickupsound", "variant"),
    (0x002000, "attacksound", "variant"), (0x004000, "hudcolor", "variant"),
    (0x008000, "deathsound", "variant"), (0x010000, "meleerange", "variant"),
    (0x020000, "maxtargetrange", "variant"), (0x040000, "ammogive", "variant"),
    (0x080000, "trailtype", "actorref"), (0x100000, "drag", "fixed"),
]

STATE_LABELS = ["Spawn", "Idle", "See", "Melee", "Missile", "Death", "XDeath",
                "Ready", "Hold", "Fire", "Pickup", "Pain"]

ACTION_FUNCS = {
    1:  ("A_FireBullets", ["fixed", "fixed", "byte", "byte", "actorref"]),
    2:  ("A_PlaySound", ["byte"]),
    3:  ("A_FireProjectile", ["actorref"]),
    4:  ("A_WeaponReady", []),
    5:  ("A_Explode", ["variant", "variant"]),
    6:  ("A_FaceTarget", ["ratio"]),
    7:  ("A_Look", []),
    8:  ("A_Chase", []),
    9:  ("A_Light", ["ratio"]),
    10: ("A_MeleeAttack", ["byte", "actorref"]),
    11: ("A_SkullAttack", ["variant"]),
}


class Frame:

    def __init__(self, w, xoffset, yoffset, transparent):
        self.w = w
        self.xoffset = xoffset
        self.yoffset = yoffset
        self.transparent = transparent
        self.tiles = {}
        self.h = 0

    def __repr__(self):
        return "Frame({}x{} n={} t={})".format(self.w, self.h, len(self.tiles),
                                               self.transparent)


class State:
    def __init__(self):
        self.stop = False
        self.jmp = None
        self.ticks = 0
        self.flipbits = 0
        self.bright = False
        self.frames = []
        self.func = None

    def __repr__(self):
        if self.stop:
            return "State(stop)"
        if self.jmp is not None:
            return "State(goto {})".format(STATE_LABELS[self.jmp])
        return "State(t={} sides={} fn={})".format(self.ticks, len(self.frames),
                                                   self.func and self.func[0])


class Actor:
    def __init__(self):
        self.kind = 0
        self.id = 0
        self.radius = 0
        self.height = 0
        self.flags = 0
        self.props = {}
        self.startitems = []
        self.labels = {}
        self.states = []

    def __repr__(self):
        return "Actor(id={} kind={} states={})".format(self.id, self.kind,
                                                       len(self.states))


def unpack_actors(r):
    frames = []
    tiles = []

    def read_frame(_i):
        wtc = r.byte()
        f = Frame(wtc & 0xF, r.short() / 16.0, r.short() / 16.0, wtc >> 4)
        def read_tile(_j):
            idx = r.byte()
            f.tiles[idx] = (r.variant() - 1) // 32
        r.array(read_tile)
        if f.tiles:
            f.h = max(f.tiles) // max(1, f.w) + 1
        frames.append(f)

    r.array(read_frame)

    def read_tile_words(_i):
        for _ in range(32):
            tiles.append(r.fixed() & 0xFFFFFFFF)

    r.array(read_tile_words)

    actors = {}

    def read_arg(kind):
        if kind == "fixed":
            return r.fixed()
        if kind == "byte":
            return r.byte()
        if kind == "variant":
            return r.variant()
        if kind == "ratio":
            return r.ratio()
        if kind == "actorref":
            return r.variant()
        raise ValueError(kind)

    def read_actor(_i):
        a = Actor()
        a.kind = r.variant()
        a.id = r.variant()
        a.radius = r.fixed()
        a.height = r.fixed()
        a.flags = r.byte() | (r.byte() << 8)

        active = r.fixed() & 0xFFFFFFFF
        for mask, name, kind in ACTOR_PROPS:
            if active & mask:
                a.props[name] = read_arg(kind)
        if active & 0x400:
            def read_startitem(_j):
                ref = r.variant()
                amount = r.variant()
                a.startitems.append((ref, amount))
            r.array(read_startitem)

        def read_label(_j):
            label = r.byte()
            addr = r.byte()
            a.labels[label] = addr
        r.array(read_label)

        def read_state(_j):
            flags = r.byte()
            st = State()
            ctrl = flags & 0x3
            if ctrl == 2:
                st.jmp = flags >> 4
            elif ctrl == 0:
                st.ticks = r.short()
                st.flipbits = r.byte()
                st.bright = (flags & 0x4) != 0
                def read_side(_k):
                    st.frames.append(r.variant() - 1)
                r.array(read_side)
                if flags & 0x8:
                    fid = r.byte()
                    name, argkinds = ACTION_FUNCS[fid]
                    st.func = (name, fid, [read_arg(k) for k in argkinds])
            else:
                st.stop = True
            a.states.append(st)
        r.array(read_state)

        actors[a.id] = a

    r.array(read_actor)
    return actors, frames, tiles



LINE_FLAGS = [
    (0x01, "twosided"), (0x02, "special"), (0x04, "dontpegtop"),
    (0x08, "playeruse"), (0x10, "playercross"), (0x20, "repeatspecial"),
    (0x40, "blocking"),
]


class Sector:
    __slots__ = ("special", "ceil", "floor", "ceiltex", "floortex", "lightlevel", "id")


class Side:
    __slots__ = ("sector", "toptex", "midtex", "bottomtex", "id")


class Line:
    __slots__ = ("front", "back", "flags", "special", "id")

    def flag(self, name):
        for mask, n in LINE_FLAGS:
            if n == name:
                return (self.flags & mask) != 0
        raise KeyError(name)


class Seg:
    __slots__ = ("v", "side", "line", "partner", "id",
                 "dirx", "diry", "ddist", "length", "nx", "ny", "ndist", "major")


class SubSector:
    __slots__ = ("segs", "pvs", "id", "sector")


class Node:
    __slots__ = ("nx", "ny", "d", "flags", "child", "bbox", "id")


class Thing:
    __slots__ = ("actor", "x", "y", "angle", "skills", "special")


def unpack_special(r, sectors):
    special = r.byte()

    def moving(what, trigger_delay):
        speed = (r.byte() - 128) / 8.0
        delay = r.variant()
        lock = r.variant()
        targets = []

        def read_sector(_i):
            sec = r.ref(sectors)
            target = r.fixed()
            targets.append((sec, target))
        r.array(read_sector)
        return {
            "special": special, "kind": "move", "what": what,
            "trigger_delay": trigger_delay, "speed": speed, "delay": delay,
            "lock": lock, "targets": targets,
        }

    if special == 13:
        trigger_delay = r.variant()
        return moving("ceil", trigger_delay)
    if special == 64:
        return moving("floor", 0)
    if special == 112:
        lightlevel = r.ratio()
        targets = []
        r.array(lambda _i: targets.append(r.ref(sectors)))
        return {"special": special, "kind": "light",
                "lightlevel": lightlevel, "targets": targets}
    if special == 243:
        return {"special": special, "kind": "exit", "delay": r.variant()}
    raise ValueError("unsupported special {} - stream would desync".format(special))


def _build_seg_geometry(subs, verts):
    import math

    for ss in subs:
        n = len(ss.segs)
        for i, s0 in enumerate(ss.segs):
            s1 = ss.segs[(i + 1) % n]
            v0 = verts[s0.v]
            v1 = verts[s1.v]
            x0, y0 = fx_to_float(v0[0]), fx_to_float(v0[1])
            dx = fx_to_float(v1[0]) - x0
            dy = fx_to_float(v1[1]) - y0
            length = math.hypot(dx, dy)
            if length == 0:
                s0.dirx = s0.diry = 0.0
                s0.length = 0.0
            else:
                s0.dirx, s0.diry = dx / length, dy / length
                s0.length = length
            s0.ddist = s0.dirx * x0 + s0.diry * y0
            s0.nx, s0.ny = -s0.diry, s0.dirx
            s0.ndist = s0.nx * x0 + s0.ny * y0
            s0.major = "v" if abs(s0.diry) > abs(s0.dirx) else "u"


def unpack_map(r):
    sectors = []

    def read_sector(i):
        s = Sector()
        s.id = i - 1
        s.special = r.byte()
        s.ceil = r.fixed()
        s.floor = r.fixed()
        s.ceiltex = _texture(r)
        s.floortex = _texture(r)
        s.lightlevel = r.ratio()
        sectors.append(s)
    r.array(read_sector)

    sides = []

    def read_side(i):
        s = Side()
        s.id = i - 1
        s.sector = r.ref(sectors)
        s.toptex = _texture(r)
        s.midtex = _texture(r)
        s.bottomtex = _texture(r)
        sides.append(s)
    r.array(read_side)

    verts = []
    r.array(lambda _i: verts.append((r.fixed(), r.fixed())))

    lines = []

    def read_line(i):
        ln = Line()
        ln.id = i - 1
        ln.front = r.ref(sides)
        ln.back = r.ref(sides)
        ln.flags = r.byte()
        ln.special = unpack_special(r, sectors) if (ln.flags & 0x2) else None
        lines.append(ln)
    r.array(read_line)

    subs = []

    def read_sub(i):
        ss = SubSector()
        ss.id = i - 1
        ss.segs = []
        ss.sector = None

        def read_seg(_j):
            sg = Seg()
            sg.v = r.variant() - 1
            flags = r.byte()
            sg.side = (flags & 0x1) == 0
            sg.line = (r.variant() - 1) if (flags & 0x2) else -1
            sg.partner = (r.variant() - 1) if (flags & 0x4) else -1
            ss.segs.append(sg)
            if sg.line >= 0 and ss.sector is None:
                ld = lines[sg.line]
                sd = ld.front if sg.side else ld.back
                ss.sector = sd.sector
        r.array(read_seg)

        pvs = []
        r.array(lambda _j: pvs.append(r.variant() - 1))
        ss.pvs = pvs
        subs.append(ss)
    r.array(read_sub)

    nodes = []

    def read_node(i):
        n = Node()
        n.id = i - 1
        n.nx = r.fixed()
        n.ny = r.fixed()
        n.d = r.fixed()
        n.flags = r.byte()
        n.child = [None, None]
        n.bbox = [None, None]
        for side, leafmask in ((0, 0x1), (1, 0x2)):
            if n.flags & leafmask:
                n.child[side] = ("sub", r.variant() - 1)
            else:
                t = r.fixed()
                b = r.fixed()
                l = r.fixed()
                rr = r.fixed()
                n.bbox[side] = (t, b, l, rr)
                n.child[side] = ("node", r.variant() - 1)
        nodes.append(n)
    r.array(read_node)

    onoff = {}

    def read_onoff(_i):
        a = _texture(r)
        b = _texture(r)
        onoff[a.raw] = b
    r.array(read_onoff)

    transparent = set()
    r.array(lambda _i: transparent.add(_texture(r).raw))

    def read_thing(_i):
        t = Thing()
        flags = r.byte()
        t.angle = (flags & 0xF) / 8.0
        t.skills = flags >> 4
        t.actor = r.variant()
        t.x = r.fixed()
        t.y = r.fixed()
        t.special = None
        return t

    things = r.array(read_thing)

    def read_special_thing(_i):
        t = read_thing(_i)
        t.special = unpack_special(r, sectors)
        return t

    things += r.array(read_special_thing)

    _build_seg_geometry(subs, verts)

    return {
        "sectors": sectors, "sides": sides, "verts": verts, "lines": lines,
        "subs": subs, "nodes": nodes, "onoff": onoff,
        "transparent": transparent, "things": things,
    }
