
#include "poom.hh"

#include "psyqo/fragments.hh"
#include "psyqo/primitives/common.hh"
#include "psyqo/primitives/quads.hh"
#include "psyqo/primitives/triangles.hh"

#include <new>

#ifdef POOM_EMU_NOINLINE
#define PROFILE_NOINLINE __attribute__((noinline))
#else
#define PROFILE_NOINLINE
#endif

enum PrimKind {
    PK_WALL, PK_FLAT, PK_SKY, PK_SPRITE, PK_COUNT
};

#ifdef POOM_PROFILE
extern int g_primKind;
void poomProfileQuad(const psyqo::Prim::TexturedQuad &p);
void poomProfileTri(const psyqo::Prim::GouraudTexturedTriangle &p);
void poomProfileFlatSpan(int span);
extern int g_profSub;
void poomProfileClipOut(int n);
inline void poomProfilePrim(const psyqo::Prim::TexturedQuad &p) { poomProfileQuad(p); }
inline void poomProfilePrim(const psyqo::Prim::GouraudTexturedTriangle &p) { poomProfileTri(p); }
inline void poomProfilePrim(...) {}
struct ProfSurface {
    enum { NONE, FLAT, WALL } kind;
    fixed_t height;
    int32_t uScroll;
    fixed_t ax0, az0, ax1, az1;
    fixed_t m0, m1;
    fixed_t vTopWorld;
    int32_t baseU, baseV;
};
extern ProfSurface g_profSurf;
extern long g_leafStat[8];
extern long g_leafByZ[8];
#define PROF_SURF(stmt) do { stmt; } while (0)
#else
#define PROF_SURF(stmt) ((void)0)
#endif

namespace {


static constexpr int OT_SIZE = 4096;
static constexpr int ARENA_WORDS = 24 * 1024;
static constexpr int MIN_FRAG_WORDS = 10;
static_assert(OT_SIZE > ARENA_WORDS / MIN_FRAG_WORDS,
              "the arena must run out before the ordering table does, or "
              "submit() clamps z to 0 and the painter's order breaks");

struct RenderBuffer {
    psyqo::OrderingTable<OT_SIZE, psyqo::Safe::No> ot;
    uint32_t arena[ARENA_WORDS];
    uint32_t used;
};

RenderBuffer g_buffers[2];
RenderBuffer *g_rb;
int32_t g_z;

template <typename Prim>
struct Frag : psyqo::Fragments::ChainEntry {
    Prim prim;
    size_t getActualFragmentSize() const { return sizeof(Prim) / sizeof(uint32_t); }
};

template <typename Prim>
Frag<Prim> *alloc() {
    constexpr size_t words = 1 + sizeof(Prim) / sizeof(uint32_t);
    static_assert(sizeof(Frag<Prim>) == words * sizeof(uint32_t),
                  "a fragment is no longer its chain word followed by its packet");
    if (g_rb->used + words > ARENA_WORDS) return nullptr;
    Frag<Prim> *f = (Frag<Prim> *)(g_rb->arena + g_rb->used);
    g_rb->used += words;
    return f;
}

template <typename OT>
void clearOT(OT &ot) {
    auto *table = ot.m_table;
    table[0].setEndMarker();
    for (unsigned i = 1; i <= OT_SIZE; i++) {
        table[i].head = (uintptr_t)&table[i - 1] & 0xffffff;
    }
#ifdef POOM_PROFILE
    ot.inserts = 0;
#endif
}

template <typename Prim>
void submit(Frag<Prim> *f) {
#ifdef POOM_PROFILE
    poomProfilePrim(f->prim);
#endif
    g_rb->ot.insert(*f, g_z);
    if (g_z > 0) g_z--;
}

inline void setUV(psyqo::PrimPieces::UVCoords &c, int u, int v) {
    c.u = (uint8_t)u;
    c.v = (uint8_t)v;
}
inline void setUV(psyqo::PrimPieces::UVCoordsPadded &c, int u, int v) {
    c.u = (uint8_t)u;
    c.v = (uint8_t)v;
}

struct TexWindow {
    uint32_t command;
    TexWindow() : command(0xE2000000) {}
    TexWindow(uint8_t maskX, uint8_t maskY, uint8_t offX, uint8_t offY)
        : command(0xE2000000 | (maskX & 0x1F) | ((maskY & 0x1F) << 5) |
                  ((offX & 0x1F) << 10) | ((offY & 0x1F) << 15)) {}
};

Level *g_lvl;
Assets *g_as;


Camera g_cam;

struct CamVert {
    fixed_t ax, az;
    fixed_t w;
    int32_t sx;
    uint8_t outcode;
};

inline void toCamera(fixed_t x, fixed_t y, fixed_t *ax, fixed_t *az) {
    fixed_t dx = x - g_cam.x;
    fixed_t dy = y - g_cam.y;
    *ax = fmul(g_cam.ca, dx) - fmul(g_cam.sa, dy);
    *az = fmul(g_cam.sa, dx) + fmul(g_cam.ca, dy);
}

inline fixed_t projW(fixed_t az) {
    uint32_t d = (uint32_t)az >> 8;
    if (d == 0) d = 1;
    FIXED_COUNT(div32);
    return (fixed_t)(0xF0000000u / d);
}

inline int32_t screenY(fixed_t worldZ, fixed_t w) {
    return CENTER_Y + g_cam.pitch + fixedToInt(fmul(g_cam.z - worldZ, w));
}

inline int32_t screenX(fixed_t ax, fixed_t w) {
    int64_t p = ((int64_t)ax * (int64_t)w) >> 32;
    if (p < -1024) p = -1024;
    if (p > 1024) p = 1024;
    return CENTER_X + (int32_t)p;
}

static constexpr int32_t DRAW_Y_MIN = -16;
static constexpr int32_t DRAW_Y_MAX = SCREEN_H + 16;
inline int16_t gpuX(int32_t x) { return (int16_t)clampi(x, -1024, 1023); }
inline int16_t gpuY(int32_t y) { return (int16_t)clampi(y, DRAW_Y_MIN, DRAW_Y_MAX); }

static constexpr int32_t DRAW_X_MIN = -32;
static constexpr int32_t DRAW_X_MAX = SCREEN_W + 32;

inline bool clipSpan(int32_t &p0, int32_t &p1, int32_t &t0, int32_t &t1,
                     int32_t lo, int32_t hi) {
    if (p1 <= p0) return false;
    if (p1 < lo || p0 > hi) return false;
    int32_t op0 = p0, ot0 = t0, ot1 = t1;
    int32_t d = p1 - p0;
    if (p0 < lo) {
        FIXED_COUNT(div64);
        t0 = ot0 + (int32_t)(((int64_t)(ot1 - ot0) * (lo - op0)) / d);
        p0 = lo;
    }
    if (p1 > hi) {
        FIXED_COUNT(div64);
        t1 = ot0 + (int32_t)(((int64_t)(ot1 - ot0) * (hi - op0)) / d);
        p1 = hi;
    }
    return true;
}

fixed_t g_flatNearBelow, g_flatNearAbove;

void updateFlatNearScales() {
    int horizon = CENTER_Y + g_cam.pitch;
    int below = DRAW_Y_MAX - horizon;
    int above = horizon - DRAW_Y_MIN;
    if (below < 8) below = 8;
    if (above < 8) above = 8;
    g_flatNearBelow = fdiv(FOCAL, intToFixed(below));
    g_flatNearAbove = fdiv(FOCAL, intToFixed(above));
}

inline fixed_t flatNearZ(fixed_t height) {
    fixed_t dz = g_cam.z - height;
    fixed_t z = (dz >= 0) ? fmul(dz, g_flatNearBelow)
                          : fmul(-dz, g_flatNearAbove);
    return z > NEAR_Z ? z : NEAR_Z;
}

static constexpr int MAX_VERTS = 1100;
int16_t g_vcacheStamp[MAX_VERTS];
CamVert g_vcache[MAX_VERTS];
int16_t g_stamp;

const CamVert *projectVertex(uint16_t vi) {
    if (vi < MAX_VERTS && g_vcacheStamp[vi] == g_stamp) return &g_vcache[vi];
    CamVert v;
    const VertexDef &vd = g_lvl->verts[vi];
    toCamera(vd.x, vd.y, &v.ax, &v.az);
    uint8_t code = 2;
    if (v.az > NEAR_Z) code = 0;
    if (v.az > FAR_Z) code |= 1;
    int64_t l = (int64_t)v.ax * FOV_DEN;
    int64_t r = (int64_t)v.az * FOV_NUM;
    if (-l > r) code |= 4;
    if (l > r) code |= 8;
    v.w = (v.az > 0) ? projW(v.az) : FRACUNIT;
    v.sx = screenX(v.ax, v.w);
    if (vi < MAX_VERTS) {
        g_vcache[vi] = v;
        g_vcacheStamp[vi] = g_stamp;
        return &g_vcache[vi];
    }
    static CamVert scratch;
    scratch = v;
    return &scratch;
}


inline psyqo::PrimPieces::ClutIndex clutFor(int variant, int light) {
    int row = clutRow(variant, clampi(light, 0, 15));
    return psyqo::PrimPieces::ClutIndex(VRAM_CLUT_X >> 4, VRAM_CLUT_Y + row);
}

inline int lightForW(int light255, fixed_t w) {
    int maxlight = (light255 * 241) >> 12;
    int32_t wc = fixedToInt(w * 8) + (fixedToInt(w) >> 1);
    if (wc > 1024) wc = 1024;
    int v = (int)(((uint64_t)(uint32_t)(light255 * wc * 2) * 0x1010102u) >> 32);
    return v < maxlight ? v : maxlight;
}

inline int lightForSector(const SectorDef &s) {
    int l = s.lightlevel;
    if (g_ambientLight > l) l = g_ambientLight;
    return l;
}


struct TexInfo {
    const TexDef *def;
    uint8_t pageX, pageY;
    int w, h;
};

bool texInfo(uint16_t index, TexInfo *out) {
    if (index >= g_as->numTextures) return false;
    const TexDef *t = &g_as->textures[index];
    out->def = t;
    out->w = t->w + 1;
    out->h = t->h + 1;
    out->pageX = g_as->texPages[t->page].px;
    out->pageY = g_as->texPages[t->page].py;
    return true;
}

inline TexWindow windowFor(const TexInfo &t) {
    uint8_t mx = (uint8_t)((t.w >> 3) - 1);
    uint8_t my = (uint8_t)((t.h >> 3) - 1);
    return TexWindow((uint8_t)(~mx), (uint8_t)(~my),
                     (uint8_t)(t.def->u >> 3), (uint8_t)(t.def->v >> 3));
}

inline int32_t alignDown(int32_t v, int32_t align) {
    return v & ~(align - 1);
}

uint32_t g_lastWindow = 0xFFFFFFFF;

void setWindowCommand(uint32_t command) {
    if (command == g_lastWindow) return;
    g_lastWindow = command;
    auto *f = alloc<TexWindow>();
    if (!f) return;
    f->prim.command = command;
    submit(f);
}

void setWindow(const TexInfo &t) {
    TexWindow w = windowFor(t);
    if (w.command == g_lastWindow) return;
    g_lastWindow = w.command;
    auto *f = alloc<TexWindow>();
    if (!f) return;
    f->prim = w;
    submit(f);
}

void resetWindow() {
    TexWindow w;
    if (w.command == g_lastWindow) return;
    g_lastWindow = w.command;
    auto *f = alloc<TexWindow>();
    if (!f) return;
    f->prim = w;
    submit(f);
}


inline int32_t texelOf(fixed_t world) { return (world + FRACUNIT) >> 17; }

#ifndef POOM_WARP_E8
#define POOM_WARP_E8 16
#endif
static constexpr int WARP_E8 = POOM_WARP_E8;
#ifndef POOM_WALL_E8
#define POOM_WALL_E8 24
#endif
static constexpr int WALL_E8 = POOM_WALL_E8;

struct FlatVert {
    fixed_t ax, az;
    fixed_t x, y;
};

static constexpr int MAX_FLAT_VERTS = 29;
static constexpr int MAX_POLY = 56;

struct ClipPlane { int a, b; fixed_t c; };
static constexpr ClipPlane kNearPlane = {0, 1, -NEAR_Z};
static constexpr ClipPlane kLeftPlane = {FOV_DEN, FOV_NUM, 0};
static constexpr ClipPlane kRightPlane = {-FOV_DEN, FOV_NUM, 0};

inline int64_t planeDist(fixed_t ax, fixed_t az, const ClipPlane &p) {
    return (int64_t)p.a * ax + (int64_t)p.b * az + p.c;
}

inline fixed_t crossFraction32(int32_t d0, int32_t d1) {
    int32_t den = d0 - d1;
    if (den < 0) { den = -den; d0 = -d0; }
    int sh = 0;
    while ((den >> sh) >= (1 << 15)) sh++;
    den >>= sh;
    if (den == 0) return 0;
    FIXED_COUNT(div32);
    return (fixed_t)(((d0 >> sh) << 16) / den);
}

inline fixed_t crossFraction(int64_t d0, int64_t d1) {
    if (d0 == (int32_t)d0 && d1 == (int32_t)d1 && (d0 - d1) == (int32_t)(d0 - d1))
        return crossFraction32((int32_t)d0, (int32_t)d1);
    int64_t den = d0 - d1;
    if (den < 0) { den = -den; d0 = -d0; }
    int sh = 0;
    while ((den >> sh) >= ((int64_t)1 << 31)) sh += 8;
    return crossFraction32((int32_t)(d0 >> sh), (int32_t)(-((den >> sh) - (d0 >> sh))));
}

void crossFlat(const FlatVert &a, const FlatVert &b, int64_t da, int64_t db, FlatVert &o) {
    bool swap = b.x < a.x || (b.x == a.x && b.y < a.y);
    const FlatVert &p = swap ? b : a, &q = swap ? a : b;
    fixed_t t = swap ? crossFraction(db, da) : crossFraction(da, db);
    o.ax = p.ax + fmul(q.ax - p.ax, t);
    o.az = p.az + fmul(q.az - p.az, t);
    o.x = p.x + fmul(q.x - p.x, t);
    o.y = p.y + fmul(q.y - p.y, t);
}

struct FlatRing {
    const FlatVert *v;
    int n, start;
    int phys(int j) const {
        int k = start + j;
        return k < n ? k : k - n;
    }
    const FlatVert &operator[](int j) const { return v[phys(j)]; }
    void rotate() { start = start ? start - 1 : n - 1; }
};

int clipFlatPoly(const FlatRing &in, FlatVert *out, const ClipPlane &p) {
    int n = in.n;
    if (n == 0) return 0;
    int m = 0;
    const FlatVert *prev = &in[n - 1];
    int64_t dp = planeDist(prev->ax, prev->az, p);
    for (int i = 0; i < n; i++) {
        const FlatVert *cur = &in[i];
        int64_t dc = planeDist(cur->ax, cur->az, p);
        bool pin = dp >= 0, cin = dc >= 0;
        if (pin) {
            if (m < MAX_POLY) out[m++] = *prev;
            else FIXED_COUNT(clipDrop);
        }
        if (pin != cin) {
            if (m < MAX_POLY) crossFlat(*prev, *cur, dp, dc, out[m++]);
            else FIXED_COUNT(clipDrop);
        }
        prev = cur;
        dp = dc;
    }
#ifdef POOM_PROFILE
    poomProfileClipOut(m);
#endif
    return m;
}

int clipFlatPoly(const FlatVert *in, int n, FlatVert *out, const ClipPlane &p) {
    return clipFlatPoly(FlatRing{in, n, 0}, out, p);
}

void countOutsideSides(const FlatRing &r, int &outL, int &outR) {
    outL = outR = 0;
    for (int k = 0; k < r.n; k++) {
        int64_t l = (int64_t)FOV_DEN * r.v[k].ax;
        int64_t z = (int64_t)FOV_NUM * r.v[k].az;
        if (z + l < 0) outL++;
        if (z - l < 0) outR++;
    }
}

bool clipStage(FlatRing &r, int outside, FlatVert *scratch, const ClipPlane &p) {
    if (outside == 0) {
        r.rotate();
#ifdef POOM_PROFILE
        poomProfileClipOut(r.n);
#endif
    } else if (outside == r.n) {
        return false;
    } else {
        r = FlatRing{scratch, clipFlatPoly(r, scratch, p), 0};
    }
    return r.n >= 3;
}

int countNear(const FlatRing &r, fixed_t nearZ) {
    int outside = 0;
    for (int k = 0; k < r.n; k++) {
        if (r.v[k].az < nearZ) outside++;
    }
    return outside;
}

struct FlatProj {
    bool valid;
    fixed_t w[MAX_POLY];
    int32_t sx[MAX_POLY];
};

struct WallEnd {
    fixed_t ax, az;
    fixed_t wx, wy;
};

bool clipWallPlane(WallEnd &p0, WallEnd &p1, const ClipPlane &p) {
    int64_t d0 = planeDist(p0.ax, p0.az, p);
    int64_t d1 = planeDist(p1.ax, p1.az, p);
    if (d0 < 0 && d1 < 0) return false;
    if (d0 >= 0 && d1 >= 0) return true;
    fixed_t t = crossFraction(d0, d1);
    WallEnd n;
    n.ax = p0.ax + fmul(p1.ax - p0.ax, t);
    n.az = p0.az + fmul(p1.az - p0.az, t);
    n.wx = p0.wx + fmul(p1.wx - p0.wx, t);
    n.wy = p0.wy + fmul(p1.wy - p0.wy, t);
    if (d0 < 0) {
        p0 = n;
    } else {
        p1 = n;
    }
    return true;
}

void emitFlatSurface(const FlatVert *v, int n, fixed_t height,
                     uint16_t texIndex, int light255, bool isSky,
                     int32_t uScroll, FlatProj &shared) {
#ifdef POOM_PROFILE
    if (isSky) g_primKind = PK_SKY;
#endif
    const fixed_t nearZ = flatNearZ(height);
    const ClipPlane nearPlane = {0, 1, -nearZ};
    const FlatRing in = {v, n, 0};
    FlatVert scratch[MAX_POLY];
    FlatProj local;
    FlatRing c = in;
    if (!clipStage(c, countNear(in, nearZ), scratch, nearPlane)) return;
    FlatProj &proj = (c.v == in.v) ? shared : local;
    if (c.v != in.v) local.valid = false;
    if (!proj.valid) {
        for (int k = 0; k < c.n; k++) {
            proj.w[k] = projW(c.v[k].az);
            proj.sx[k] = screenX(c.v[k].ax, proj.w[k]);
        }
        proj.valid = true;
    }
    const int m = c.n;
    int at[MAX_POLY], ys[MAX_POLY];
    int32_t tu[MAX_POLY], tv[MAX_POLY];
    for (int j = 0; j < m; j++) {
        at[j] = c.phys(j);
        ys[j] = screenY(height, proj.w[at[j]]);
    }

    uint8_t pageX, pageY;
    int light = 15;
    TexInfo tex;
    const TexDef &sky = g_as->sky;
    if (isSky) {
        pageX = g_as->texPages[sky.page].px;
        pageY = g_as->texPages[sky.page].py;
        resetWindow();
        for (int j = 0; j < m; j++) {
            tu[j] = sky.u + 2;
            tv[j] = sky.v + clampi(ys[j] - g_cam.pitch, 0, 255);
        }
        PROF_SURF(g_profSurf = {});
    } else {
        if (!texInfo(texIndex, &tex)) return;
        pageX = tex.pageX;
        pageY = tex.pageY;
        for (int j = 0; j < m; j++) {
            tu[j] = texelOf(c.v[at[j]].x) + uScroll;
            tv[j] = texelOf(c.v[at[j]].y);
        }
        int32_t minU = tu[0], minV = tv[0];
        for (int j = 1; j < m; j++) {
            if (tu[j] < minU) minU = tu[j];
            if (tv[j] < minV) minV = tv[j];
        }
        int32_t baseU = alignDown(minU, tex.w);
        int32_t baseV = alignDown(minV, tex.h);
        int32_t span = 0;
        for (int j = 0; j < m; j++) {
            tu[j] -= baseU;
            tv[j] -= baseV;
            if (tu[j] > span) span = tu[j];
            if (tv[j] > span) span = tv[j];
        }
#ifdef POOM_PROFILE
        poomProfileFlatSpan(span);
#endif
        if (span > 255) return;
        setWindow(tex);
        light = lightForW(light255, proj.w[at[0]]);
        PROF_SURF(g_profSurf = {});
        PROF_SURF(g_profSurf.kind = ProfSurface::FLAT);
        PROF_SURF(g_profSurf.height = height);
        PROF_SURF(g_profSurf.uScroll = uScroll);
        PROF_SURF(g_profSurf.baseU = baseU);
        PROF_SURF(g_profSurf.baseV = baseV);
    }

    using PrimVertex = decltype(psyqo::Prim::TexturedQuad::pointA);
    auto point = [&](int j) -> PrimVertex {
        return {{.x = gpuX(proj.sx[at[j]]), .y = gpuY(ys[j])}};
    };
    const psyqo::PrimPieces::ClutIndex clut = clutFor(0, light);

    int i = 1;
    for (; i + 2 < m; i += 2) {
        auto *f = alloc<psyqo::Prim::TexturedQuad>();
        if (!f) return;
        auto &p = f->prim;
        p = psyqo::Prim::TexturedQuad();
        p.pointA = point(i);
        p.pointB = point(0);
        p.pointC = point(i + 1);
        p.pointD = point(i + 2);
        setUV(p.uvA, tu[i], tv[i]);
        setUV(p.uvB, tu[0], tv[0]);
        setUV(p.uvC, tu[i + 1], tv[i + 1]);
        setUV(p.uvD, tu[i + 2], tv[i + 2]);
        p.clutIndex = clut;
        p.tpage.setPageX(pageX).setPageY(pageY)
            .set(psyqo::Prim::TPageAttr::Tex4Bits)
            .setDithering(false);
        p.setColor({{.r = 128, .g = 128, .b = 128}});
        submit(f);
    }
    if (i + 1 < m) {
        auto *f = alloc<psyqo::Prim::GouraudTexturedTriangle>();
        if (!f) return;
        auto &p = f->prim;
        p = psyqo::Prim::GouraudTexturedTriangle();
        p.pointA = point(0);
        p.pointB = point(i);
        p.pointC = point(i + 1);
        setUV(p.uvA, tu[0], tv[0]);
        setUV(p.uvB, tu[i], tv[i]);
        setUV(p.uvC, tu[i + 1], tv[i + 1]);
        p.clutIndex = clut;
        p.tpage.setPageX(pageX).setPageY(pageY)
            .set(psyqo::Prim::TPageAttr::Tex4Bits)
            .setDithering(false);
        p.setColorA({{.r = 128, .g = 128, .b = 128}});
        p.setColorB({{.r = 128, .g = 128, .b = 128}});
        p.setColorC({{.r = 128, .g = 128, .b = 128}});
        submit(f);
    }
}

bool clipFlatSides(FlatRing &r, FlatVert *a, FlatVert *b) {
    int outL, outR;
    countOutsideSides(r, outL, outR);
    bool clipped = outL != 0 && outL != r.n;
    if (!clipStage(r, outL, a, kLeftPlane)) return false;
    if (clipped) {
        int unused;
        countOutsideSides(r, unused, outR);
    }
    return clipStage(r, outR, b, kRightPlane);
}

void emitFlatWhole(const FlatVert *base, int n, const SectorDef &sec, int light) {
    FlatVert a[MAX_POLY], b[MAX_POLY], c[MAX_POLY];
    FlatRing r = {base, n, 0};
    if (!clipFlatSides(r, a, b)) return;
    for (int k = 0; k < r.n; k++) c[k] = r[k];
    FlatProj shared;
    shared.valid = false;
    emitFlatSurface(c, r.n, sec.ceil, sec.ceiltex, light, true, 0, shared);
}


static constexpr int FLAT_ROOT = 256;
static constexpr int FLAT_MIN_CELL = 2;
static constexpr int FLAT_DEPTH = 15;
static constexpr int FLAT_SIDE_MAX = 24;
#ifndef POOM_FLAT_A
#define POOM_FLAT_A 12
#endif
#ifndef POOM_FLAT_B
#define POOM_FLAT_B 2
#endif
static constexpr int32_t FLAT_A = POOM_FLAT_A, FLAT_B = POOM_FLAT_B;
static_assert((int64_t)(FLAT_A + FLAT_B) * 8192 * 8192 <= UINT32_MAX, "FLAT_A + FLAT_B too large");
static constexpr fixed_t FLAT_ZCLAMP = intToFixed(16);
#ifndef POOM_FLAT_ZTEXEL
#define POOM_FLAT_ZTEXEL 240
#endif
static constexpr int FLAT_ZTEXEL = POOM_FLAT_ZTEXEL;
#ifndef POOM_FLAT_RELIEF2
#define POOM_FLAT_RELIEF2 0
#endif

struct FlatCell {
    int32_t x0, y0;
    fixed_t ax0, az0;
    int32_t lxs, lys;
};

fixed_t g_flatSaAbs, g_flatCaAbs, g_flatSaMin, g_flatCaMin;

void updateFlatSplit() {
    g_flatSaAbs = g_cam.sa < 0 ? -g_cam.sa : g_cam.sa;
    g_flatCaAbs = g_cam.ca < 0 ? -g_cam.ca : g_cam.ca;
    g_flatSaMin = g_cam.sa < 0 ? g_cam.sa : 0;
    g_flatCaMin = g_cam.ca < 0 ? g_cam.ca : 0;
}

static constexpr int FLAT_ROOT_SHIFT = 8;
static constexpr int FLAT_MIN_SHIFT = 1;
static_assert((1 << FLAT_ROOT_SHIFT) == FLAT_ROOT && (1 << FLAT_MIN_SHIFT) == FLAT_MIN_CELL, "");

FlatCell flatRootCell(int32_t x0, int32_t y0) {
    FlatCell c;
    c.x0 = x0;
    c.y0 = y0;
    toCamera(intToFixed(x0), intToFixed(y0), &c.ax0, &c.az0);
    c.lxs = c.lys = FLAT_ROOT_SHIFT;
    return c;
}

int flatCellSplit(const FlatCell &c) {
    bool canX = c.lxs > FLAT_MIN_SHIFT, canY = c.lys > FLAT_MIN_SHIFT;
    if (!canX && !canY) return 0;
    fixed_t az = c.az0 + (g_flatSaMin << c.lxs) + (g_flatCaMin << c.lys);
    if (az < FLAT_ZCLAMP) az = FLAT_ZCLAMP;
    if (az >= intToFixed(4096)) return 0;
    int32_t z = az >> 12;
    int32_t dzx = g_flatSaAbs >> (12 - c.lxs), dzy = g_flatCaAbs >> (12 - c.lys);
    int32_t D = dzx + dzy;
    int32_t S = (g_flatCaAbs >> (12 - c.lxs)) + (g_flatSaAbs >> (12 - c.lys));
    int32_t sCap = (z * 427) >> 8;
    if (S > sCap) S = sCap;
    uint32_t err = ((uint32_t)FLAT_A * S + (uint32_t)FLAT_B * D) * (uint32_t)D;
    constexpr int32_t zt = FLAT_ZTEXEL * 16;
    bool splits;
    if (z <= zt) {
        splits = err > (((uint32_t)WARP_E8 * (uint32_t)(z * z)) >> 3);
    } else {
#if POOM_FLAT_RELIEF2
        uint32_t r = ((uint32_t)z * (uint32_t)z) / (uint32_t)zt;
        splits = (uint64_t)err * 8 > (uint64_t)((uint32_t)WARP_E8 * r) * r;
#else
        splits = (uint64_t)err * (uint32_t)(8 * zt) >
                 (uint64_t)((uint32_t)z * (uint32_t)z) * (uint32_t)(WARP_E8 * z);
#endif
    }
    if (!splits) return 0;
    if (canX && (!canY || dzx >= dzy)) return 1;
    return 2;
}

inline FlatCell flatChild(const FlatCell &c, int split, int k) {
    if (split == 1) {
        int32_t h = c.lxs - 1;
        return {c.x0 + (k << h), c.y0, c.ax0 + (k ? g_cam.ca << h : 0),
                c.az0 + (k ? g_cam.sa << h : 0), h, c.lys};
    }
    int32_t h = c.lys - 1;
    return {c.x0, c.y0 + (k << h), c.ax0 - (k ? g_cam.sa << h : 0),
            c.az0 + (k ? g_cam.ca << h : 0), c.lxs, h};
}

int splitFlat(const FlatVert *in, int n, int axis, fixed_t c,
              FlatVert *lo, int &nlo, FlatVert *hi, int &nhi) {
    auto coord = [axis](const FlatVert &v) { return axis ? v.y : v.x; };
    bool anyLo = false, anyHi = false;
    for (int i = 0; i < n; i++) {
        fixed_t d = coord(in[i]);
        if (d < c) anyLo = true;
        if (d > c) anyHi = true;
    }
    nlo = nhi = 0;
    if (!anyHi) return 1;
    if (!anyLo) return 2;
    const FlatVert *prev = &in[n - 1];
    fixed_t dp = coord(*prev) - c;
    for (int i = 0; i < n; i++) {
        const FlatVert *cur = &in[i];
        fixed_t dc = coord(*cur) - c;
        if ((dp < 0 && dc > 0) || (dp > 0 && dc < 0)) {
            FlatVert o;
            fixed_t other = axis ? prev->x : prev->y;
            if (other == (axis ? cur->x : cur->y)) {
                o.x = axis ? other : c;
                o.y = axis ? c : other;
                toCamera(o.x, o.y, &o.ax, &o.az);
            } else {
                crossFlat(*prev, *cur, dp, dc, o);
                if (axis) o.y = c; else o.x = c;
            }
            if (nlo < MAX_POLY) lo[nlo++] = o; else FIXED_COUNT(clipDrop);
            if (nhi < MAX_POLY) hi[nhi++] = o; else FIXED_COUNT(clipDrop);
        }
        if (dc <= 0) { if (nlo < MAX_POLY) lo[nlo++] = *cur; else FIXED_COUNT(clipDrop); }
        if (dc >= 0) { if (nhi < MAX_POLY) hi[nhi++] = *cur; else FIXED_COUNT(clipDrop); }
        prev = cur;
        dp = dc;
    }
    return 0;
}

struct FlatSurf {
    bool on;
    fixed_t height;
    fixed_t dz;
    fixed_t nearZ;
    uint16_t texIndex;
    TexInfo tex;
    uint32_t window;
    psyqo::PrimPieces::TPageAttr tpage;
    int32_t uScroll;
};

bool makeFlatSurf(FlatSurf &f, bool on, fixed_t height, uint16_t texIndex, int32_t uScroll) {
    f.on = on && texInfo(texIndex, &f.tex);
    if (!f.on) return false;
    f.height = height;
    f.dz = g_cam.z - height;
    f.nearZ = flatNearZ(height);
    f.texIndex = texIndex;
    f.window = windowFor(f.tex).command;
    f.tpage = psyqo::PrimPieces::TPageAttr();
    f.tpage.setPageX(f.tex.pageX).setPageY(f.tex.pageY)
        .set(psyqo::Prim::TPageAttr::Tex4Bits)
        .setDithering(false);
    f.uScroll = uScroll;
    return true;
}

struct FlatJob {
    const SectorDef *sec;
    int light;
    FlatSurf floor, ceil;
    fixed_t needZ;
};

struct FlatPV {
    int32_t sx;
    fixed_t w;
    int32_t u, v;
};
static constexpr int FLAT_FAST_MAX = 8;

template <typename T>
inline uint32_t raw16(const T &t) {
    static_assert(sizeof(T) == 2, "");
    uint16_t r;
    __builtin_memcpy(&r, &t, 2);
    return r;
}
inline uint32_t packXY(int32_t x, int32_t y) { return (uint16_t)x | ((uint32_t)(uint16_t)y << 16); }

void emitFlatQuad4(const FlatSurf &f, int light255, const FlatPV *pv, int32_t minU, int32_t minV) {
    auto drift = [&](const FlatPV &a, const FlatPV &b) {
        int32_t dw = a.w > b.w ? a.w - b.w : b.w - a.w;
        int32_t dx = a.sx > b.sx ? a.sx - b.sx : b.sx - a.sx;
        int32_t dy = fixedToInt(fmul(f.dz, dw));
        if (dy < 0) dy = -dy;
        return (int64_t)(dx + dy) * dw;
    };
    bool pick13 = drift(pv[1], pv[3]) < drift(pv[0], pv[2]);
    const FlatPV &A = pick13 ? pv[2] : pv[1], &B = pick13 ? pv[1] : pv[0];
    const FlatPV &C = pick13 ? pv[3] : pv[2], &D = pick13 ? pv[0] : pv[3];
    const int32_t baseU = alignDown(minU + f.uScroll, f.tex.w);
    const int32_t baseV = alignDown(minV, f.tex.h);
    const int32_t du = f.uScroll - baseU;
    const int32_t horizon = CENTER_Y + g_cam.pitch;
    auto xy = [&](const FlatPV &q) {
        return packXY(gpuX(q.sx), gpuY(horizon + fixedToInt(fmul(f.dz, q.w))));
    };
    auto uv = [&](const FlatPV &q) {
        return (uint32_t)(uint8_t)(q.u + du) | ((uint32_t)(uint8_t)(q.v - baseV) << 8);
    };
#ifdef POOM_PROFILE
    {
        int32_t span = 0;
        for (int k = 0; k < 4; k++) {
            if (pv[k].u + du > span) span = pv[k].u + du;
            if (pv[k].v - baseV > span) span = pv[k].v - baseV;
        }
        poomProfileFlatSpan(span);
    }
#endif
    setWindowCommand(f.window);
    PROF_SURF(g_profSurf = {});
    PROF_SURF(g_profSurf.kind = ProfSurface::FLAT);
    PROF_SURF(g_profSurf.height = f.height);
    PROF_SURF(g_profSurf.uScroll = f.uScroll);
    PROF_SURF(g_profSurf.baseU = baseU);
    PROF_SURF(g_profSurf.baseV = baseV);
    const uint32_t clut = raw16(clutFor(0, lightForW(light255, pv[0].w)));
    auto *fr = alloc<psyqo::Prim::TexturedQuad>();
    if (!fr) return;
    uint32_t *o = reinterpret_cast<uint32_t *>(&fr->prim);
    o[0] = 0x2c808080;
    o[1] = xy(A);
    o[2] = uv(A) | (clut << 16);
    o[3] = xy(B);
    o[4] = uv(B) | (raw16(f.tpage) << 16);
    o[5] = xy(C);
    o[6] = uv(C);
    o[7] = xy(D);
    o[8] = uv(D);
    submit(fr);
}

void emitFlatPV(const FlatSurf &f, int light255, const FlatPV *pvIn, int m,
                int32_t minU, int32_t minV) {
    if (m == 4) {
        emitFlatQuad4(f, light255, pvIn, minU, minV);
        return;
    }
    const FlatPV *pv = pvIn;
    const int32_t baseU = alignDown(minU + f.uScroll, f.tex.w);
    const int32_t baseV = alignDown(minV, f.tex.h);
    const int32_t du = f.uScroll - baseU;
    int16_t xs[FLAT_FAST_MAX], ys[FLAT_FAST_MAX];
    uint8_t us[FLAT_FAST_MAX], vs[FLAT_FAST_MAX];
    const int32_t horizon = CENTER_Y + g_cam.pitch;
    for (int k = 0; k < m; k++) {
        xs[k] = gpuX(pv[k].sx);
        ys[k] = gpuY(horizon + fixedToInt(fmul(f.dz, pv[k].w)));
        us[k] = (uint8_t)(pv[k].u + du);
        vs[k] = (uint8_t)(pv[k].v - baseV);
    }
#ifdef POOM_PROFILE
    {
        int32_t span = 0;
        for (int k = 0; k < m; k++) {
            if (pv[k].u + du > span) span = pv[k].u + du;
            if (pv[k].v - baseV > span) span = pv[k].v - baseV;
        }
        poomProfileFlatSpan(span);
    }
#endif
    setWindowCommand(f.window);
    PROF_SURF(g_profSurf = {});
    PROF_SURF(g_profSurf.kind = ProfSurface::FLAT);
    PROF_SURF(g_profSurf.height = f.height);
    PROF_SURF(g_profSurf.uScroll = f.uScroll);
    PROF_SURF(g_profSurf.baseU = baseU);
    PROF_SURF(g_profSurf.baseV = baseV);
    const psyqo::PrimPieces::ClutIndex clut = clutFor(0, lightForW(light255, pv[0].w));
    int i = 1;
    for (; i + 2 < m; i += 2) {
        auto *fr = alloc<psyqo::Prim::TexturedQuad>();
        if (!fr) return;
        auto &p = *new (&fr->prim) psyqo::Prim::TexturedQuad();
        p.pointA = {{.x = xs[i], .y = ys[i]}};
        p.pointB = {{.x = xs[0], .y = ys[0]}};
        p.pointC = {{.x = xs[i + 1], .y = ys[i + 1]}};
        p.pointD = {{.x = xs[i + 2], .y = ys[i + 2]}};
        p.uvA.u = us[i];         p.uvA.v = vs[i];
        p.uvB.u = us[0];         p.uvB.v = vs[0];
        p.uvC.u = us[i + 1];     p.uvC.v = vs[i + 1];
        p.uvD.u = us[i + 2];     p.uvD.v = vs[i + 2];
        p.clutIndex = clut;
        p.tpage = f.tpage;
        submit(fr);
    }
    if (i + 1 < m) {
        auto *fr = alloc<psyqo::Prim::GouraudTexturedTriangle>();
        if (!fr) return;
        auto &p = fr->prim;
        p = psyqo::Prim::GouraudTexturedTriangle();
        p.pointA = {{.x = xs[0], .y = ys[0]}};
        p.pointB = {{.x = xs[i], .y = ys[i]}};
        p.pointC = {{.x = xs[i + 1], .y = ys[i + 1]}};
        p.uvA.u = us[0];         p.uvA.v = vs[0];
        p.uvB.u = us[i];         p.uvB.v = vs[i];
        p.uvC.u = us[i + 1];     p.uvC.v = vs[i + 1];
        p.clutIndex = clut;
        p.tpage = f.tpage;
        p.setColorA({{.r = 128, .g = 128, .b = 128}});
        p.setColorB({{.r = 128, .g = 128, .b = 128}});
        p.setColorC({{.r = 128, .g = 128, .b = 128}});
        submit(fr);
    }
}

void flatSideCorners(const FlatCell &c, bool vertical, int32_t at, bool high,
                     int32_t lo, int32_t hi, int32_t *out, int &n, int depth) {
    if (depth >= FLAT_DEPTH) return;
    int split = flatCellSplit(c);
    if (!split) return;
    bool along = vertical ? split == 2 : split == 1;
    if (!along) {
        int32_t m = vertical ? c.x0 + (1 << (c.lxs - 1)) : c.y0 + (1 << (c.lys - 1));
        int k = high ? (at < m ? 0 : 1) : (at <= m ? 0 : 1);
        flatSideCorners(flatChild(c, split, k), vertical, at, high, lo, hi, out, n, depth + 1);
        return;
    }
    int32_t m = vertical ? c.y0 + (1 << (c.lys - 1)) : c.x0 + (1 << (c.lxs - 1));
    if (lo < m) flatSideCorners(flatChild(c, split, 0), vertical, at, high, lo, hi, out, n, depth + 1);
    if (lo < m && m < hi) {
        if (n < FLAT_SIDE_MAX) out[n++] = m; else FIXED_COUNT(clipDrop);
    }
    if (m < hi) flatSideCorners(flatChild(c, split, 1), vertical, at, high, lo, hi, out, n, depth + 1);
}

FlatCell flatMirror(const FlatCell &c, bool vertical, bool high) {
    FlatCell m = c;
    int32_t k = high ? 1 : -1;
    if (vertical) {
        m.x0 += k << c.lxs;
        m.ax0 += k * (g_cam.ca << c.lxs);
        m.az0 += k * (g_cam.sa << c.lxs);
    } else {
        m.y0 += k << c.lys;
        m.ax0 -= k * (g_cam.sa << c.lys);
        m.az0 += k * (g_cam.ca << c.lys);
    }
    return m;
}

static constexpr int CEIL_QUEUE = 1024, CEIL_PIECES = 256;
FlatPV g_ceilQueue[CEIL_QUEUE];
struct CeilPiece { uint8_t m; int32_t minU, minV; };
CeilPiece g_ceilPiece[CEIL_PIECES];
int g_ceilQueued, g_ceilPieces;

void flatLeafPolygon(const FlatJob &j, const FlatVert *leaf, int m);

void flatLeaf(const FlatJob &j, const FlatCell &cell, int depth, const FlatVert *p, int n) {
    const int32_t lx = 1 << cell.lxs, ly = 1 << cell.lys;
    const fixed_t X0 = intToFixed(cell.x0), X1 = intToFixed(cell.x0 + lx);
    const fixed_t Y0 = intToFixed(cell.y0), Y1 = intToFixed(cell.y0 + ly);
    int32_t pts[2][FLAT_SIDE_MAX];
    int np[2] = {0, 0}, edgeOf[2] = {-1, -1}, found = 0;
    bool vert[2];
    for (int i = 0; i < n && found < 2; i++) {
        const FlatVert &a = p[i], &b = p[i + 1 < n ? i + 1 : 0];
        bool vertical;
        if (a.x == b.x && (a.x == X0 || a.x == X1)) {
            vertical = true;
            bool high = a.x == X1;
            if (high ? g_cam.sa >= 0 : g_cam.sa <= 0) continue;
            flatSideCorners(flatMirror(cell, true, high), true, high ? cell.x0 + lx : cell.x0, high,
                            cell.y0, cell.y0 + ly, pts[found], np[found], depth);
        } else if (a.y == b.y && (a.y == Y0 || a.y == Y1)) {
            vertical = false;
            bool high = a.y == Y1;
            if (high ? g_cam.ca >= 0 : g_cam.ca <= 0) continue;
            flatSideCorners(flatMirror(cell, false, high), false, high ? cell.y0 + ly : cell.y0, high,
                            cell.x0, cell.x0 + lx, pts[found], np[found], depth);
        } else {
            continue;
        }
        if (np[found]) {
            vert[found] = vertical;
            edgeOf[found++] = i;
        }
    }
#ifdef POOM_PROFILE
    g_leafStat[0]++; g_leafStat[1] += n;
    if (n == 4 && !found) g_leafStat[3]++;
    g_leafStat[4] += depth;
    {
        fixed_t zmin = INT32_MAX;
        for (int i = 0; i < n; i++) if (p[i].az < zmin) zmin = p[i].az;
        int b = 0;
        for (fixed_t z = intToFixed(64); zmin >= z && b < 7; z <<= 1) b++;
        g_leafByZ[b]++;
    }
#endif
    if (!found) {
        flatLeafPolygon(j, p, n);
        return;
    }
    FlatVert leaf[MAX_POLY];
    int m = 0;
    for (int i = 0; i < n; i++) {
        const FlatVert &a = p[i], &b = p[i + 1 < n ? i + 1 : 0];
        if (m < MAX_POLY) leaf[m++] = a; else FIXED_COUNT(clipDrop);
        int e = edgeOf[0] == i ? 0 : edgeOf[1] == i ? 1 : -1;
        if (e < 0) continue;
        bool vertical = vert[e];
        fixed_t from = vertical ? a.y : a.x, to = vertical ? b.y : b.x;
        bool up = to > from;
        for (int k = 0; k < np[e]; k++) {
            fixed_t t = intToFixed(pts[e][up ? k : np[e] - 1 - k]);
            if (up ? (t <= from || t >= to) : (t >= from || t <= to)) continue;
            if (m >= MAX_POLY) { FIXED_COUNT(clipDrop); break; }
            FlatVert &o = leaf[m++];
            o.x = vertical ? a.x : t;
            o.y = vertical ? t : a.y;
            toCamera(o.x, o.y, &o.ax, &o.az);
        }
    }
#ifdef POOM_PROFILE
    g_leafStat[2] += m - n;
#endif
    flatLeafPolygon(j, leaf, m);
}

PROFILE_NOINLINE void flatProject(const FlatVert *v, int n, FlatPV *pv, int32_t &minU, int32_t &minV) {
    minU = minV = INT32_MAX;
    for (int k = 0; k < n; k++) {
        pv[k].w = projW(v[k].az);
        pv[k].sx = screenX(v[k].ax, pv[k].w);
        pv[k].u = texelOf(v[k].x);
        pv[k].v = texelOf(v[k].y);
        if (pv[k].u < minU) minU = pv[k].u;
        if (pv[k].v < minV) minV = pv[k].v;
    }
}

PROFILE_NOINLINE void flatLeafPolygon(const FlatJob &j, const FlatVert *leaf, int m) {
    if (m == 4 && leaf[0].az >= j.needZ && leaf[1].az >= j.needZ &&
        leaf[2].az >= j.needZ && leaf[3].az >= j.needZ) {
        FlatPV pv[4];
        int32_t minU, minV;
        flatProject(leaf, 4, pv, minU, minV);
        if (j.floor.on) emitFlatQuad4(j.floor, j.light, pv, minU, minV);
        if (j.ceil.on) {
            if (j.floor.on && g_ceilQueued + 4 <= CEIL_QUEUE && g_ceilPieces < CEIL_PIECES) {
                FlatPV *q = g_ceilQueue + g_ceilQueued;
                q[0] = pv[0]; q[1] = pv[1]; q[2] = pv[2]; q[3] = pv[3];
                g_ceilQueued += 4;
                g_ceilPiece[g_ceilPieces++] = {4, minU, minV};
            } else {
                emitFlatQuad4(j.ceil, j.light, pv, minU, minV);
            }
        }
        return;
    }
    auto project = flatProject;
    auto nearState = [&](const FlatSurf &f) {
        if (!f.on) return 2;
        int in = 0;
        for (int k = 0; k < m; k++) in += leaf[k].az >= f.nearZ;
        return in == m ? 0 : (in == 0 ? 2 : 1);
    };
    auto ceiling = [&](const FlatPV *pv, int n, int32_t minU, int32_t minV) {
        if (j.floor.on && g_ceilQueued + n <= CEIL_QUEUE && g_ceilPieces < CEIL_PIECES) {
            for (int k = 0; k < n; k++) g_ceilQueue[g_ceilQueued + k] = pv[k];
            g_ceilQueued += n;
            g_ceilPiece[g_ceilPieces++] = {(uint8_t)n, minU, minV};
        } else {
            emitFlatPV(j.ceil, j.light, pv, n, minU, minV);
        }
    };
    const int fs = nearState(j.floor), cs = nearState(j.ceil);
    if (fs == 2 && cs == 2) return;
    FlatPV pv[FLAT_FAST_MAX];
    int32_t minU, minV;
    if (m <= FLAT_FAST_MAX && (fs == 0 || cs == 0)) {
        project(leaf, m, pv, minU, minV);
        if (fs == 0) emitFlatPV(j.floor, j.light, pv, m, minU, minV);
        if (cs == 0) ceiling(pv, m, minU, minV);
    }
    for (int s = 0; s < 2; s++) {
        const FlatSurf &f = s ? j.ceil : j.floor;
        int st = s ? cs : fs;
        if (st == 2 || (st == 0 && m <= FLAT_FAST_MAX)) continue;
        FlatVert clipped[MAX_POLY];
        const FlatVert *v = leaf;
        int n = m;
        if (st == 1) {
            n = clipFlatPoly(leaf, m, clipped, ClipPlane{0, 1, -f.nearZ});
            v = clipped;
            if (n < 3) continue;
        }
        if (n <= FLAT_FAST_MAX) {
            project(v, n, pv, minU, minV);
            if (s) ceiling(pv, n, minU, minV); else emitFlatPV(f, j.light, pv, n, minU, minV);
        } else {
            FlatProj own;
            own.valid = false;
            emitFlatSurface(v, n, f.height, f.texIndex, j.light, false, f.uScroll, own);
        }
    }
}

PROFILE_NOINLINE int flatStitchFull(const FlatCell &c, int depth, FlatVert *out) {
    const int32_t lx = 1 << c.lxs, ly = 1 << c.lys;
    const int32_t cx[4] = {c.x0, c.x0 + lx, c.x0 + lx, c.x0};
    const int32_t cy[4] = {c.y0, c.y0, c.y0 + ly, c.y0 + ly};
    const fixed_t dax = g_cam.ca << c.lxs, daz = g_cam.sa << c.lxs;
    const fixed_t eax = -(g_cam.sa << c.lys), eaz = g_cam.ca << c.lys;
    const fixed_t ax[4] = {c.ax0, c.ax0 + dax, c.ax0 + dax + eax, c.ax0 + eax};
    const fixed_t az[4] = {c.az0, c.az0 + daz, c.az0 + daz + eaz, c.az0 + eaz};
    int m = 0;
    for (int s = 0; s < 4; s++) {
        FlatVert &v = out[m++];
        v.x = intToFixed(cx[s]);
        v.y = intToFixed(cy[s]);
        v.ax = ax[s];
        v.az = az[s];
        bool vertical = s & 1;
        bool high = s == 1 || s == 2;
        fixed_t toward = vertical ? g_cam.sa : g_cam.ca;
        if (high ? toward >= 0 : toward <= 0) continue;
        int32_t at = vertical ? (high ? c.x0 + lx : c.x0) : (high ? c.y0 + ly : c.y0);
        int32_t lo = vertical ? c.y0 : c.x0, hi = lo + (vertical ? ly : lx);
        int32_t pts[FLAT_SIDE_MAX];
        int np = 0;
        flatSideCorners(flatMirror(c, vertical, high), vertical, at, high, lo, hi,
                        pts, np, depth);
        bool up = s < 2;
        for (int k = 0; k < np; k++) {
            int32_t t = pts[up ? k : np - 1 - k];
            FlatVert &o = out[m++];
            o.x = intToFixed(vertical ? at : t);
            o.y = intToFixed(vertical ? t : at);
            toCamera(o.x, o.y, &o.ax, &o.az);
        }
    }
    return m;
}

PROFILE_NOINLINE void flatFullLeaf(const FlatJob &j, const FlatCell &c, int depth) {
    FlatVert v[4 + 4 * FLAT_SIDE_MAX];
    int m = flatStitchFull(c, depth, v);
#ifdef POOM_PROFILE
    g_leafStat[0]++; g_leafStat[1] += 4; g_leafStat[2] += m - 4;
    if (m == 4) g_leafStat[3]++;
    g_leafStat[4] += depth;
    {
        fixed_t zmin = INT32_MAX;
        for (int i = 0; i < 4; i++) if (v[i].az < zmin) zmin = v[i].az;
        int b = 0;
        for (fixed_t z = intToFixed(64); zmin >= z && b < 7; z <<= 1) b++;
        g_leafByZ[b]++;
    }
#endif
    flatLeafPolygon(j, v, m);
}

FlatVert g_flatHalf[FLAT_DEPTH][2][MAX_POLY];

inline bool flatFillsCell(const FlatVert *p, int n, const FlatCell &c) {
    if (n != 4) return false;
    const fixed_t X0 = intToFixed(c.x0), X1 = intToFixed(c.x0 + (1 << c.lxs));
    const fixed_t Y0 = intToFixed(c.y0), Y1 = intToFixed(c.y0 + (1 << c.lys));
    for (int k = 0; k < 4; k++) {
        if ((p[k].x != X0 && p[k].x != X1) || (p[k].y != Y0 && p[k].y != Y1)) return false;
    }
    return true;
}

void flatCell(const FlatJob &j, const FlatCell &c, const FlatVert *p, int n, int depth) {
    if (p && n < 3) return;
    int split = depth < FLAT_DEPTH ? flatCellSplit(c) : 0;
    if (!split) {
        if (p) flatLeaf(j, c, depth, p, n);
        else flatFullLeaf(j, c, depth);
        return;
    }
    const FlatCell c0 = flatChild(c, split, 0), c1 = flatChild(c, split, 1);
    if (!p) {
        flatCell(j, c0, nullptr, 0, depth + 1);
        flatCell(j, c1, nullptr, 0, depth + 1);
        return;
    }
    fixed_t at = split == 1 ? intToFixed(c1.x0) : intToFixed(c1.y0);
    int nh[2];
    FlatVert *h0 = g_flatHalf[depth][0], *h1 = g_flatHalf[depth][1];
    int r = splitFlat(p, n, split - 1, at, h0, nh[0], h1, nh[1]);
    if (r == 0) {
        flatCell(j, c0, flatFillsCell(h0, nh[0], c0) ? nullptr : h0, nh[0], depth + 1);
        flatCell(j, c1, flatFillsCell(h1, nh[1], c1) ? nullptr : h1, nh[1], depth + 1);
    } else {
        const FlatCell &cc = r == 1 ? c0 : c1;
        flatCell(j, cc, flatFillsCell(p, n, cc) ? nullptr : p, n, depth + 1);
    }
}

FlatVert g_flatRoot[6][MAX_POLY];

void emitFlatQuad(const FlatVert *base, int n, const SectorDef &sec, int light,
                  bool doFloor, bool doCeil, int32_t uScroll) {
#ifdef POOM_PROFILE
    g_primKind = PK_FLAT;
#endif
    FlatVert a[MAX_POLY], b[MAX_POLY], c[MAX_POLY];
    FlatRing r = {base, n, 0};
    if (!clipFlatSides(r, a, b)) return;
    fixed_t nearZ = doFloor ? flatNearZ(sec.floor) : flatNearZ(sec.ceil);
    if (doFloor && doCeil) {
        fixed_t nc = flatNearZ(sec.ceil);
        if (nc < nearZ) nearZ = nc;
    }
    const ClipPlane nearPlane = {0, 1, -nearZ};
    if (!clipStage(r, countNear(r, nearZ), c, nearPlane)) return;
    FlatVert *poly = g_flatRoot[0];
    for (int k = 0; k < r.n; k++) poly[k] = r[k];
    fixed_t minX = poly[0].x, maxX = minX, minY = poly[0].y, maxY = minY;
    for (int k = 1; k < r.n; k++) {
        if (poly[k].x < minX) minX = poly[k].x;
        if (poly[k].x > maxX) maxX = poly[k].x;
        if (poly[k].y < minY) minY = poly[k].y;
        if (poly[k].y > maxY) maxY = poly[k].y;
    }
    FlatJob j;
    j.sec = &sec;
    j.light = light;
    makeFlatSurf(j.floor, doFloor, sec.floor, sec.floortex, uScroll);
    makeFlatSurf(j.ceil, doCeil, sec.ceil, sec.ceiltex, 0);
    if (!j.floor.on && !j.ceil.on) return;
    j.needZ = j.floor.on ? j.floor.nearZ : 0;
    if (j.ceil.on && j.ceil.nearZ > j.needZ) j.needZ = j.ceil.nearZ;
    g_ceilQueued = g_ceilPieces = 0;
    const int32_t rx0 = (minX >> 16) & ~(FLAT_ROOT - 1), rx1 = (maxX >> 16) & ~(FLAT_ROOT - 1);
    const int32_t ry0 = (minY >> 16) & ~(FLAT_ROOT - 1), ry1 = (maxY >> 16) & ~(FLAT_ROOT - 1);
    const FlatVert *rest = poly;
    int nrest = r.n, rb = 1;
    for (int32_t rx = rx0; rx <= rx1 && nrest >= 3; rx += FLAT_ROOT) {
        const FlatVert *col = rest;
        int ncol = nrest;
        nrest = 0;
        if (rx < rx1) {
            int nlo, nhi;
            int s = splitFlat(col, ncol, 0, intToFixed(rx + FLAT_ROOT),
                              g_flatRoot[2], nlo, g_flatRoot[rb], nhi);
            if (s == 2) {
                nrest = ncol;
                continue;
            }
            if (s == 0) {
                rest = g_flatRoot[rb];
                nrest = nhi;
                rb ^= 1;
                col = g_flatRoot[2];
                ncol = nlo;
            }
        }
        const FlatVert *crest = col;
        int ncrest = ncol, cb = 3;
        for (int32_t ry = ry0; ry <= ry1 && ncrest >= 3; ry += FLAT_ROOT) {
            if (ry < ry1) {
                int nsq, nhi;
                int s = splitFlat(crest, ncrest, 1, intToFixed(ry + FLAT_ROOT),
                                  g_flatRoot[5], nsq, g_flatRoot[cb], nhi);
                if (s == 2) continue;
                if (s == 0) {
                    flatCell(j, flatRootCell(rx, ry), g_flatRoot[5], nsq, 0);
                    crest = g_flatRoot[cb];
                    ncrest = nhi;
                    cb = cb == 3 ? 4 : 3;
                    continue;
                }
            }
            flatCell(j, flatRootCell(rx, ry), crest, ncrest, 0);
            break;
        }
    }
    const FlatPV *q = g_ceilQueue;
    for (int k = 0; k < g_ceilPieces; k++) {
        emitFlatPV(j.ceil, light, q, g_ceilPiece[k].m, g_ceilPiece[k].minU, g_ceilPiece[k].minV);
        q += g_ceilPiece[k].m;
    }
}



struct WallCol {
    int32_t u;
    fixed_t w;
    int32_t sx, ytop, ybot;
};

struct WallJob {
    fixed_t ax0, az0;
    fixed_t kax, kaz;
    fixed_t m0;
    fixed_t topZ, bottomZ;
    int32_t vTop, vBottom;
    TexInfo tex;
    int light255;
    bool masked;
};

void wallColumnAt(const WallJob &j, fixed_t ax, fixed_t az, int32_t u, WallCol &c) {
    if (az < NEAR_Z) az = NEAR_Z;
    c.u = u;
    c.w = projW(az);
    c.sx = screenX(ax, c.w);
    c.ytop = screenY(j.topZ, c.w);
    c.ybot = screenY(j.bottomZ, c.w);
}

void wallColumn(const WallJob &j, int32_t u, WallCol &c, fixed_t *azOut = nullptr) {
    fixed_t dm = intToFixed(2 * u) - j.m0;
    fixed_t ax = j.ax0 + fmul(dm, j.kax);
    fixed_t az = j.az0 + fmul(dm, j.kaz);
    if (azOut) *azOut = az;
    wallColumnAt(j, ax, az, u, c);
}

inline int32_t splitPoint(int32_t lo, int32_t hi) {
    int32_t x = lo + 1, y = hi - 1;
    if (x >= y) return x;
    uint32_t diff = (uint32_t)(x ^ y);
    int bit = 31;
    while (!(diff >> bit)) bit--;
    return (int32_t)((uint32_t)y & ~((1u << bit) - 1));
}

inline bool clipWallEdge(int32_t &y0, int32_t &y1, int32_t &v0, int32_t &v1,
                         int32_t lo, int32_t hi, int32_t limit) {
    if (y1 <= y0) return false;
    if (y1 < lo || y0 > hi) return false;
    const int32_t oy0 = y0, ov0 = v0;
    const int32_t dy = y1 - y0, dv = v1 - v0;
    if (dv <= 0) {
        if (y0 < lo) y0 = lo;
        if (y1 > hi) y1 = hi;
        return true;
    }
    if (oy0 < lo) {
        FIXED_COUNT(div32);
        int32_t vc = ov0 + dv * (lo - oy0) / dy;
        FIXED_COUNT(div32);
        int32_t yc = oy0 + ((vc - ov0) * dy + dv / 2) / dv;
        if (yc >= lo - limit) {
            v0 = vc;
            y0 = yc;
        } else {
            FIXED_COUNT(div32);
            v0 = ov0 + (dv * (lo - oy0) + dy / 2) / dy;
            y0 = lo;
        }
    }
    if (y1 > hi) {
        int32_t num = dv * (hi - oy0);
        FIXED_COUNT(div32);
        int32_t vc = ov0 + (num + dy - 1) / dy;
        if (vc > v1) vc = v1;
        FIXED_COUNT(div32);
        int32_t yc = oy0 + ((vc - ov0) * dy + dv / 2) / dv;
        if (yc <= hi + limit) {
            v1 = vc;
            y1 = yc;
        } else {
            FIXED_COUNT(div32);
            v1 = ov0 + (num + dy / 2) / dy;
            y1 = hi;
        }
    }
    return y1 > y0;
}

static constexpr int32_t WALL_V_SLACK = 112;

void emitWallQuad(const WallJob &j, const WallCol &a, const WallCol &b,
                  int32_t ya0, int32_t ya1, int32_t yb0, int32_t yb1,
                  int32_t v0, int32_t v1) {
#ifdef POOM_PROFILE
    g_primKind = PK_WALL;
#endif
    const TexInfo &tex = j.tex;
    int32_t va0 = v0, va1 = v1, vb0 = v0, vb1 = v1;
    if (!clipWallEdge(ya0, ya1, va0, va1, DRAW_Y_MIN, DRAW_Y_MAX, WALL_V_SLACK)) return;
    if (!clipWallEdge(yb0, yb1, vb0, vb1, DRAW_Y_MIN, DRAW_Y_MAX, WALL_V_SLACK)) return;

    int32_t minV = va0 < vb0 ? va0 : vb0;
    int32_t baseU = alignDown(a.u < b.u ? a.u : b.u, tex.w);
    int32_t baseV = alignDown(minV, tex.h);
    int32_t ua = a.u - baseU, ub = b.u - baseU;
    va0 -= baseV;
    va1 -= baseV;
    vb0 -= baseV;
    vb1 -= baseV;
    if (ua > 255 || ub > 255) return;
    if (va1 > 255 || vb1 > 255) return;
    PROF_SURF(g_profSurf.baseU = baseU);
    PROF_SURF(g_profSurf.baseV = baseV);

    auto *f = alloc<psyqo::Prim::TexturedQuad>();
    if (!f) return;
    auto &p = f->prim;
    p = psyqo::Prim::TexturedQuad();
    p.pointA = {{.x = gpuX(a.sx), .y = (int16_t)ya0}};
    p.pointB = {{.x = gpuX(b.sx), .y = (int16_t)yb0}};
    p.pointC = {{.x = gpuX(a.sx), .y = (int16_t)ya1}};
    p.pointD = {{.x = gpuX(b.sx), .y = (int16_t)yb1}};
    setUV(p.uvA, ua, va0);
    setUV(p.uvB, ub, vb0);
    setUV(p.uvC, ua, va1);
    setUV(p.uvD, ub, vb1);
    int light = (lightForW(j.light255, a.w) + lightForW(j.light255, b.w)) >> 1;
    p.clutIndex = clutFor(j.masked ? 1 : 0, light);
    p.tpage.setPageX(tex.pageX).setPageY(tex.pageY)
        .set(psyqo::Prim::TPageAttr::Tex4Bits)
        .setDithering(false);
    p.setColor({{.r = 128, .g = 128, .b = 128}});
    submit(f);
}

inline bool wallPieceTooWarped(const WallCol &a, const WallCol &b, int32_t ya0, int32_t ya1,
                               int32_t yb0, int32_t yb1) {
    int32_t dx = a.sx - b.sx;
    if (dx < 0) dx = -dx;
    int32_t ta = clampi(ya0, DRAW_Y_MIN, DRAW_Y_MAX), ba = clampi(ya1, DRAW_Y_MIN, DRAW_Y_MAX);
    int32_t tb = clampi(yb0, DRAW_Y_MIN, DRAW_Y_MAX), bb = clampi(yb1, DRAW_Y_MIN, DRAW_Y_MAX);
    int32_t d1 = bb - ta, d2 = ba - tb;
    if (d1 < 0) d1 = -d1;
    if (d2 < 0) d2 = -d2;
    int32_t len = dx + (d1 > d2 ? d1 : d2);
    int32_t dw = a.w > b.w ? a.w - b.w : b.w - a.w;
    int32_t sw = a.w + b.w;
    return ((int64_t)len * dw) * 8 > (int64_t)(2 * WALL_E8) * sw;
}

inline int32_t wallRowY(const WallJob &j, const WallCol &c, int32_t v) {
    int32_t dv = j.vBottom - j.vTop;
    if (dv <= 0) return c.ytop;
    int32_t num = (c.ybot - c.ytop) * (v - j.vTop);
    FIXED_COUNT(div32);
    return c.ytop + (2 * num + dv) / (2 * dv);
}

void wallRows(const WallJob &j, const WallCol &a, const WallCol &b, int32_t v0, int32_t v1) {
    int32_t ya0 = wallRowY(j, a, v0), ya1 = wallRowY(j, a, v1);
    int32_t yb0 = wallRowY(j, b, v0), yb1 = wallRowY(j, b, v1);
    if (ya1 < DRAW_Y_MIN && yb1 < DRAW_Y_MIN) return;
    if (ya0 > DRAW_Y_MAX && yb0 > DRAW_Y_MAX) return;
    if (v1 - v0 >= 2 && wallPieceTooWarped(a, b, ya0, ya1, yb0, yb1)) {
        int32_t m = splitPoint(v0, v1);
        wallRows(j, a, b, v0, m);
        wallRows(j, a, b, m, v1);
        return;
    }
    emitWallQuad(j, a, b, ya0, ya1, yb0, yb1, v0, v1);
}

void wallSpan(const WallJob &j, const WallCol &a, const WallCol &b) {
    int32_t lo = a.u < b.u ? a.u : b.u, hi = a.u < b.u ? b.u : a.u;
    if (hi - lo >= 2) {
        int32_t dx = a.sx - b.sx;
        if (dx < 0) dx = -dx;
        if (hi - lo > 128 ||
            (dx >= 2 && wallPieceTooWarped(a, b, a.ytop, a.ybot, b.ytop, b.ybot))) {
            WallCol c;
            wallColumn(j, splitPoint(lo, hi), c);
            wallSpan(j, a, c);
            wallSpan(j, c, b);
            return;
        }
    }
    if (wallPieceTooWarped(a, b, a.ytop, a.ybot, b.ytop, b.ybot)) {
        wallRows(j, a, b, j.vTop, j.vBottom);
    } else {
        emitWallQuad(j, a, b, a.ytop, a.ybot, b.ytop, b.ybot, j.vTop, j.vBottom);
    }
}

void drawWall(const SegDef &seg, const CamVert &c0, const CamVert &c1,
              fixed_t topZ, fixed_t bottomZ, int32_t vTopWorld,
              uint16_t texIndex, int light255, bool masked,
              fixed_t wx0, fixed_t wy0, fixed_t wx1, fixed_t wy1, unsigned cut) {
    if (topZ <= bottomZ) return;
    WallJob j;
    if (!texInfo(texIndex, &j.tex)) return;

    fixed_t m0 = seg.major ? wy0 : wx0, m1 = seg.major ? wy1 : wx1;
    j.vTop = texelOf(vTopWorld - topZ);
    j.vBottom = texelOf(vTopWorld - bottomZ);
    if (j.vBottom - j.vTop > 255) return;
    j.topZ = topZ;
    j.bottomZ = bottomZ;
    j.light255 = light255;
    j.masked = masked;

    j.m0 = m0;
    j.ax0 = c0.ax;
    j.az0 = c0.az;
    fixed_t dm = m1 - m0;
    if (dm == 0) {
        j.kax = j.kaz = 0;
    } else {
        j.kax = fdiv(c1.ax - c0.ax, dm);
        j.kaz = fdiv(c1.az - c0.az, dm);
    }

    WallCol a, b;
    auto endColumn = [&](const CamVert &c, fixed_t m, bool clipped, bool first, WallCol &out) {
        int32_t ur = texelOf(m);
        if (clipped && dm != 0) {
            bool up = first ? (dm < 0) : (dm > 0);
            int32_t uo = up ? ((m + 2 * FRACUNIT - 1) >> 17) : (m >> 17);
            fixed_t az;
            wallColumn(j, uo, out, &az);
            bool outward = first ? out.sx <= c.sx : out.sx >= c.sx;
            if (az >= NEAR_Z && outward && out.sx >= DRAW_X_MIN - 256 &&
                out.sx <= DRAW_X_MAX + 256) return;
        }
        wallColumnAt(j, c.ax, c.az, ur, out);
    };
    endColumn(c0, m0, cut & 1, true, a);
    endColumn(c1, m1, cut & 2, false, b);

    setWindow(j.tex);
    PROF_SURF(g_profSurf = {});
    PROF_SURF(g_profSurf.kind = ProfSurface::WALL);
    PROF_SURF((g_profSurf.ax0 = c0.ax, g_profSurf.az0 = c0.az));
    PROF_SURF((g_profSurf.ax1 = c1.ax, g_profSurf.az1 = c1.az));
    PROF_SURF(g_profSurf.m0 = m0);
    PROF_SURF(g_profSurf.m1 = m1);
    PROF_SURF(g_profSurf.vTopWorld = vTopWorld);

    wallSpan(j, a, b);
}


struct PendingSprite {
    const Thing *thing;
    bool drawn;
};

static constexpr int MAX_PENDING = 96;
PendingSprite g_pending[MAX_PENDING];
int g_numPending;

void drawThing(const Thing *t) {
    fixed_t ax, az;
    toCamera(t->x, t->y, &ax, &az);
    if (az <= NEAR_Z || az > FAR_Z) return;
    if ((int64_t)ax * FOV_DEN > (int64_t)az * FOV_NUM * 2) return;
    if (-(int64_t)ax * FOV_DEN > (int64_t)az * FOV_NUM * 2) return;
    const StateDef *st = t->state;
    if (!st || st->numSides == 0) return;

    int side = 0;
    bool flip = false;
    if (st->numSides > 1) {
        angle_t view = fatan2(g_cam.x - t->x, t->y - g_cam.y);
        uint32_t rel = ((uint32_t)(uint16_t)(view - t->angle) + 0x1000) & 0xFFFF;
        side = (int)((rel & 0xFFFF) * st->numSides >> 16);
        if (side >= st->numSides) side = st->numSides - 1;
        flip = (st->flipbits >> side) & 1;
    }
    const FrameDef *fr = g_as->frame(st->sides[side]);

    fixed_t w = projW(az);
    fixed_t scale = w * 2;

    int sw = fixedToInt(scale * (fr->w + 1)) >> 0;
    int sh = fixedToInt(scale * (fr->h + 1)) >> 0;
    if (sw <= 0 || sh <= 0) return;

    int cx = screenX(ax, w);
    int cy = screenY(t->z, w);
    int x0 = cx - fixedToInt(fmul(scale, intToFixed(fr->xoffset)));
    int y0 = cy - fixedToInt(fmul(scale, intToFixed(fr->yoffset)));
    if (x0 > SCREEN_W || x0 + sw < 0 || y0 > SCREEN_H || y0 + sh < 0) return;

    const SectorDef &sec = g_lvl->sectors[t->sector];
    int light = st->bright ? 8 : lightForW(lightForSector(sec), w);

#ifdef POOM_PROFILE
    g_primKind = PK_SPRITE;
    g_profSurf.kind = ProfSurface::NONE;
#endif
    resetWindow();
    auto *f = alloc<psyqo::Prim::TexturedQuad>();
    if (!f) return;
    auto &p = f->prim;
    p = psyqo::Prim::TexturedQuad();
    int32_t sx0 = x0, sx1 = x0 + sw;
    int32_t u0 = fr->u, u1 = fr->u + fr->w;
    if (flip) {
        int32_t tmp = u0;
        u0 = u1;
        u1 = tmp;
    }
    if (!clipSpan(sx0, sx1, u0, u1, DRAW_X_MIN, DRAW_X_MAX)) return;
    int32_t sy0 = y0, sy1 = y0 + sh;
    int32_t v0 = fr->v, v1 = fr->v + fr->h;
    if (!clipSpan(sy0, sy1, v0, v1, DRAW_Y_MIN, DRAW_Y_MAX)) return;

    p.pointA = {{.x = (int16_t)sx0, .y = (int16_t)sy0}};
    p.pointB = {{.x = (int16_t)sx1, .y = (int16_t)sy0}};
    p.pointC = {{.x = (int16_t)sx0, .y = (int16_t)sy1}};
    p.pointD = {{.x = (int16_t)sx1, .y = (int16_t)sy1}};
    setUV(p.uvA, u0, v0);
    setUV(p.uvB, u1, v0);
    setUV(p.uvC, u0, v1);
    setUV(p.uvD, u1, v1);
    p.clutIndex = clutFor(1 + fr->variant, light);
    p.tpage.setPageX(g_as->spritePages[fr->page].px)
        .setPageY(g_as->spritePages[fr->page].py)
        .set(psyqo::Prim::TPageAttr::Tex4Bits)
        .setDithering(false);
    p.setColor({{.r = 128, .g = 128, .b = 128}});
    submit(f);
}


void drawSubSector(const SubSectorDef &ss) {
    if (ss.sector >= g_lvl->numSectors) return;
#ifdef POOM_PROFILE
    g_profSub = (int)(&ss - g_lvl->subs);
#endif
    const SectorDef &sec = g_lvl->sectors[ss.sector];
    int light = lightForSector(sec);

    bool drawFloor = sec.floortex != NO_INDEX && (sec.floor < g_cam.z);
    bool drawCeil = sec.ceil > g_cam.z;
    if (drawFloor || drawCeil) {
        bool skyCeil = drawCeil && sec.ceiltex == NO_INDEX;
        bool texCeil = drawCeil && !skyCeil;
        int32_t uScroll = gameSectorScroll(ss.sector);
        FlatVert base[MAX_POLY];
        int n = ss.numSegs;
        if (n > MAX_FLAT_VERTS) {
            FIXED_COUNT(outlineCut);
            n = MAX_FLAT_VERTS;
        }
        for (int i = 0; i < n; i++) {
            const VertexDef &vd = g_lvl->verts[g_lvl->segs[ss.firstSeg + i].v];
            base[i].x = vd.x;
            base[i].y = vd.y;
            toCamera(vd.x, vd.y, &base[i].ax, &base[i].az);
        }
        if (skyCeil) emitFlatWhole(base, n, sec, light);
        if (drawFloor || texCeil) emitFlatQuad(base, n, sec, light, drawFloor, texCeil, uScroll);
    }

    for (int i = 0; i < ss.numSegs; i++) {
        const SegDef &seg = g_lvl->segs[ss.firstSeg + i];
        if (seg.line == NO_INDEX) continue;
        const SegDef &next = g_lvl->segs[ss.firstSeg + ((i + 1) % ss.numSegs)];
        const CamVert *c0 = projectVertex(seg.v);
        CamVert v0 = *c0;
        const CamVert *c1 = projectVertex(next.v);
        CamVert v1 = *c1;
        fixed_t wx0 = g_lvl->verts[seg.v].x, wy0 = g_lvl->verts[seg.v].y;
        fixed_t wx1 = g_lvl->verts[next.v].x, wy1 = g_lvl->verts[next.v].y;

        if (v0.az <= NEAR_Z && v1.az <= NEAR_Z) continue;
        if (v0.az > NEAR_Z && v1.az > NEAR_Z && v0.sx >= v1.sx) continue;

        WallEnd e0 = {v0.ax, v0.az, wx0, wy0};
        WallEnd e1 = {v1.ax, v1.az, wx1, wy1};
        if (!clipWallPlane(e0, e1, kNearPlane)) continue;
        if (!clipWallPlane(e0, e1, kLeftPlane)) continue;
        if (!clipWallPlane(e0, e1, kRightPlane)) continue;

        unsigned cut = (e0.ax != v0.ax || e0.az != v0.az ? 1u : 0u) |
                       (e1.ax != v1.ax || e1.az != v1.az ? 2u : 0u);
        v0.ax = e0.ax;
        v0.az = e0.az;
        v0.w = projW(e0.az);
        v0.sx = screenX(e0.ax, v0.w);
        wx0 = e0.wx;
        wy0 = e0.wy;
        v1.ax = e1.ax;
        v1.az = e1.az;
        v1.w = projW(e1.az);
        v1.sx = screenX(e1.ax, v1.w);
        wx1 = e1.wx;
        wy1 = e1.wy;
        if (v0.sx >= v1.sx) continue;

        const LineDef &ld = g_lvl->lines[seg.line];
        uint16_t sideIndex = seg.side ? ld.front : ld.back;
        if (sideIndex == NO_INDEX) continue;
        const SideDef &sd = g_lvl->mutSides[sideIndex];
        uint16_t otherIndex = seg.side ? ld.back : ld.front;

        fixed_t top = sec.ceil, bottom = sec.floor;

        if (otherIndex == NO_INDEX) {
            if (sd.midtex != NO_INDEX) {
                fixed_t vTop = (ld.flags & LF_DONTPEGTOP) ? top : bottom;
                drawWall(seg, v0, v1, top, bottom, vTop, sd.midtex, light,
                         false, wx0, wy0, wx1, wy1, cut);
            }
            continue;
        }

        const SideDef &os = g_lvl->mutSides[otherIndex];
        const SectorDef &osec = g_lvl->sectors[os.sector];
        fixed_t otop = osec.ceil, obottom = osec.floor;
        if (obottom > top) obottom = top;
        if (otop < bottom) otop = bottom;

        if (top > otop && sd.toptex != NO_INDEX) {
            fixed_t vTop = (ld.flags & LF_DONTPEGTOP) ? otop : bottom;
            drawWall(seg, v0, v1, top, otop, vTop, sd.toptex, light, false,
                     wx0, wy0, wx1, wy1, cut);
        }
        if (bottom < obottom && sd.bottomtex != NO_INDEX) {
            drawWall(seg, v0, v1, obottom, bottom, obottom, sd.bottomtex,
                     light, false, wx0, wy0, wx1, wy1, cut);
        }
        if (sd.midtex != NO_INDEX) {
            fixed_t mt = otop < top ? otop : top;
            fixed_t mb = obottom > bottom ? obottom : bottom;
            drawWall(seg, v0, v1, mt, mb, mt, sd.midtex, light,
                     g_lvl->isTransparent(sd.midtex), wx0, wy0, wx1, wy1, cut);
        }
    }

    uint16_t index = (uint16_t)(&ss - g_lvl->subs);
    int order[MAX_PENDING];
    fixed_t depth[MAX_PENDING];
    int n = 0;
    for (int i = 0; i < g_numPending && n < MAX_PENDING; i++) {
        if (g_pending[i].thing->ssector != index || g_pending[i].drawn) continue;
        fixed_t ax, az;
        toCamera(g_pending[i].thing->x, g_pending[i].thing->y, &ax, &az);
        int k = n++;
        while (k > 0 && depth[k - 1] < az) {
            depth[k] = depth[k - 1];
            order[k] = order[k - 1];
            k--;
        }
        depth[k] = az;
        order[k] = i;
    }
    for (int i = 0; i < n; i++) {
        g_pending[order[i]].drawn = true;
        drawThing(g_pending[order[i]].thing);
    }
}


uint16_t g_viewSub;

bool boxVisible(const int16_t bbox[4]) {
    const int16_t t = bbox[0], b = bbox[1], l = bbox[2], r = bbox[3];
    const int16_t xs[4] = {l, l, r, r};
    const int16_t ys[4] = {b, t, t, b};
    uint8_t outcode = 0xFF;
    for (int i = 0; i < 4; i++) {
        fixed_t ax, az;
        toCamera(intToFixed(xs[i]), intToFixed(ys[i]), &ax, &az);
        uint8_t code = 2;
        if (az > NEAR_Z) code = 0;
        if (az > FAR_Z) code |= 1;
        int64_t la = (int64_t)ax * FOV_DEN, lz = (int64_t)az * FOV_NUM;
        if (la > lz) code |= 4;
        if (-la > lz) code |= 8;
        outcode &= code;
        if (outcode == 0) return true;
    }
    return false;
}

void visitNode(const NodeDef &node) {
    fixed_t dist = fmul(node.nx, g_cam.x) + fmul(node.ny, g_cam.y);
    int side = (dist <= node.d) ? 0 : 1;
    for (int pass = 0; pass < 2; pass++) {
        int s = pass == 0 ? (1 - side) : side;
        if (node.child[s].isLeaf) {
            uint16_t si = node.child[s].index;
            if (si < g_lvl->numSubs && g_lvl->inPVS(si, g_viewSub)) {
                drawSubSector(g_lvl->subs[si]);
            }
        } else {
            if (boxVisible(node.bbox[s])) {
                visitNode(g_lvl->nodes[node.child[s].index]);
            }
        }
    }
}

}


uint16_t findSubSector(const Level &lvl, fixed_t x, fixed_t y) {
    if (lvl.numNodes == 0) return 0;
    const NodeDef *node = &lvl.nodes[lvl.numNodes - 1];
    for (;;) {
        fixed_t dist = fmul(node->nx, x) + fmul(node->ny, y);
        int side = (dist <= node->d) ? 0 : 1;
        if (node->child[side].isLeaf) return node->child[side].index;
        node = &lvl.nodes[node->child[side].index];
    }
}

void renderSetLevel(Level *level, Assets *assets) {
    g_lvl = level;
    g_as = assets;
    for (int i = 0; i < MAX_VERTS; i++) g_vcacheStamp[i] = -1;
    g_stamp = 0;
}

void renderInit(psyqo::GPU &gpu) {
    g_rb = &g_buffers[0];
    g_stamp = 0;
}

void renderResetTextureWindowNow(psyqo::GPU &gpu) {
    TexWindow w;
    gpu.sendPrimitive(w);
    g_lastWindow = w.command;
}

void renderAddSprite(const Thing *t) {
    if (g_numPending >= MAX_PENDING) return;
    g_pending[g_numPending].thing = t;
    g_pending[g_numPending].drawn = false;
    g_numPending++;
}

void renderScene(psyqo::GPU &gpu, const Camera &cam) {
    g_cam = cam;
    g_cam.ca = fsin(cam.angle);
    g_cam.sa = fcos(cam.angle);
    updateFlatNearScales();
    updateFlatSplit();

    g_rb = &g_buffers[gpu.getParity()];
    g_rb->used = 0;
    clearOT(g_rb->ot);
    g_z = OT_SIZE - 1;
    g_lastWindow = 0xFFFFFFFF;
    if (++g_stamp < 0) g_stamp = 0;

    if (!g_lvl || g_lvl->numNodes == 0) return;
    g_viewSub = findSubSector(*g_lvl, cam.x, cam.y);

    visitNode(g_lvl->nodes[g_lvl->numNodes - 1]);

    for (int i = 0; i < g_numPending; i++) {
        if (!g_pending[i].drawn) drawThing(g_pending[i].thing);
    }
    g_numPending = 0;

    gpu.chain(g_rb->ot);
    gpu.sendChain();
}
