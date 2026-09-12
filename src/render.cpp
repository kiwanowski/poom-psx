
#include "poom.hh"

#include "psyqo/primitives/common.hh"
#include "psyqo/primitives/quads.hh"
#include "psyqo/primitives/triangles.hh"

enum PrimKind {
    PK_WALL, PK_FLAT_FINE, PK_FLAT_COARSE, PK_FLAT_STRIP, PK_SKY, PK_SPRITE, PK_COUNT
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
#endif

namespace {


static constexpr int OT_SIZE = 4096;
static constexpr int ARENA_WORDS = 24 * 1024;
static constexpr int MIN_FRAG_WORDS = 10;
static_assert(OT_SIZE > ARENA_WORDS / MIN_FRAG_WORDS,
              "the arena must run out before the ordering table does, or "
              "submit() clamps z to 0 and the painter's order breaks");
static constexpr int MAX_WALL_STEPS = 12;
#ifndef POOM_FLAT_LOD
#define POOM_FLAT_LOD 192
#endif
static constexpr fixed_t FLAT_LOD_DIST = intToFixed(POOM_FLAT_LOD);

struct RenderBuffer {
    psyqo::OrderingTable<OT_SIZE, psyqo::Safe::No> ot;
    uint32_t arena[ARENA_WORDS];
    uint32_t used;
};

RenderBuffer g_buffers[2];
RenderBuffer *g_rb;
int32_t g_z;

template <typename Prim>
struct Frag {
    uint32_t head;
    Prim prim;
    size_t getActualFragmentSize() const { return sizeof(Prim) / sizeof(uint32_t); }
};

template <typename Prim>
Frag<Prim> *alloc() {
    constexpr size_t words = 1 + sizeof(Prim) / sizeof(uint32_t);
    if (g_rb->used + words > ARENA_WORDS) return nullptr;
    Frag<Prim> *f = (Frag<Prim> *)(g_rb->arena + g_rb->used);
    g_rb->used += words;
    return f;
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

inline bool clipEdgeY(int32_t &y0, int32_t &y1, int32_t &v0, int32_t &v1) {
    return clipSpan(y0, y1, v0, v1, DRAW_Y_MIN, DRAW_Y_MAX);
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
    int maxlight = (light255 * 15) / 255;
    int32_t wc = fixedToInt(w * 8) + (fixedToInt(w) >> 1);
    int v = (light255 * wc * 2) / 255;
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


struct FlatVert {
    fixed_t ax, az, w;
    int32_t sx;
    int32_t u, v;
};

static constexpr int MAX_POLY = 32;
static constexpr int MAX_FLAT_VERTS = MAX_POLY - 3;

struct ClipPlane { int a, b; fixed_t c; };
static constexpr ClipPlane kNearPlane = {0, 1, -NEAR_Z};
static constexpr ClipPlane kLeftPlane = {FOV_DEN, FOV_NUM, 0};
static constexpr ClipPlane kRightPlane = {-FOV_DEN, FOV_NUM, 0};

inline int64_t planeDist(fixed_t ax, fixed_t az, const ClipPlane &p) {
    return (int64_t)p.a * ax + (int64_t)p.b * az + p.c;
}

int clipFlatPoly(const FlatVert *in, int n, FlatVert *out, const ClipPlane &p) {
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
            if (m < MAX_POLY) {
                FIXED_COUNT(div32);
                fixed_t t = (fixed_t)((dp << 16) / (dp - dc));
                FlatVert &o = out[m++];
                o.ax = prev->ax + fmul(cur->ax - prev->ax, t);
                o.az = prev->az + fmul(cur->az - prev->az, t);
                o.u = prev->u + fixedToInt(fmul(intToFixed(cur->u - prev->u), t));
                o.v = prev->v + fixedToInt(fmul(intToFixed(cur->v - prev->v), t));
            } else {
                FIXED_COUNT(clipDrop);
            }
        }
        prev = cur;
        dp = dc;
    }
#ifdef POOM_PROFILE
    poomProfileClipOut(m);
#endif
    return m;
}

int clipFlatAll(FlatVert *a, int n, FlatVert *b, fixed_t nearZ) {
    ClipPlane near = {0, 1, -nearZ};
    n = clipFlatPoly(a, n, b, near);
    if (n < 3) return 0;
    n = clipFlatPoly(b, n, a, kLeftPlane);
    if (n < 3) return 0;
    n = clipFlatPoly(a, n, b, kRightPlane);
    if (n < 3) return 0;
    for (int i = 0; i < n; i++) a[i] = b[i];
    return n;
}

struct WallEnd {
    fixed_t ax, az;
    fixed_t wx, wy;
};

bool clipWallPlane(WallEnd &p0, WallEnd &p1, const ClipPlane &p) {
    int64_t d0 = planeDist(p0.ax, p0.az, p);
    int64_t d1 = planeDist(p1.ax, p1.az, p);
    if (d0 < 0 && d1 < 0) return false;
    if (d0 >= 0 && d1 >= 0) return true;
    FIXED_COUNT(div32);
    fixed_t t = (fixed_t)((d0 << 16) / (d0 - d1));
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

void emitFlatSurface(const FlatVert *in, int n, fixed_t height,
                     uint16_t texIndex, int light255, bool isSky,
                     int32_t uScroll) {
#ifdef POOM_PROFILE
    if (isSky) g_primKind = PK_SKY;
#endif
    FlatVert c[MAX_POLY];
    const ClipPlane nearPlane = {0, 1, -flatNearZ(height)};
    int m = clipFlatPoly(in, n, c, nearPlane);
    if (m < 3) return;
    if (uScroll) {
        for (int i = 0; i < m; i++) c[i].u += uScroll;
    }

    for (int i = 0; i < m; i++) {
        c[i].w = projW(c[i].az);
        c[i].sx = screenX(c[i].ax, c[i].w);
    }
    int ys[MAX_POLY];
    for (int i = 0; i < m; i++) ys[i] = screenY(height, c[i].w);

    uint8_t pageX, pageY;
    int32_t baseU = 0, baseV = 0;
    int light = 15;
    TexInfo tex;
    if (isSky) {
        const TexDef &sky = g_as->sky;
        pageX = g_as->texPages[sky.page].px;
        pageY = g_as->texPages[sky.page].py;
        resetWindow();
    } else {
        if (!texInfo(texIndex, &tex)) return;
        pageX = tex.pageX;
        pageY = tex.pageY;
        int32_t minU = c[0].u, minV = c[0].v;
        for (int i = 1; i < m; i++) {
            if (c[i].u < minU) minU = c[i].u;
            if (c[i].v < minV) minV = c[i].v;
        }
        baseU = alignDown(minU, tex.w);
        baseV = alignDown(minV, tex.h);
#ifdef POOM_PROFILE
        {
            int32_t span = 0;
            for (int i = 0; i < m; i++) {
                if (c[i].u - baseU > span) span = c[i].u - baseU;
                if (c[i].v - baseV > span) span = c[i].v - baseV;
            }
            poomProfileFlatSpan(span);
        }
#endif
        setWindow(tex);
        light = lightForW(light255, c[0].w);
    }

    for (int i = 1; i + 1 < m; i++) {
        auto *f = alloc<psyqo::Prim::GouraudTexturedTriangle>();
        if (!f) return;
        auto &p = f->prim;
        p = psyqo::Prim::GouraudTexturedTriangle();
        const int idx[3] = {0, i, i + 1};
        p.pointA = {{.x = gpuX(c[0].sx), .y = gpuY(ys[0])}};
        p.pointB = {{.x = gpuX(c[i].sx), .y = gpuY(ys[i])}};
        p.pointC = {{.x = gpuX(c[i + 1].sx), .y = gpuY(ys[i + 1])}};
        if (isSky) {
            const TexDef &sky = g_as->sky;
            setUV(p.uvA, sky.u + 2, sky.v + clampi(ys[idx[0]] - g_cam.pitch, 0, 255));
            setUV(p.uvB, sky.u + 2, sky.v + clampi(ys[idx[1]] - g_cam.pitch, 0, 255));
            setUV(p.uvC, sky.u + 2, sky.v + clampi(ys[idx[2]] - g_cam.pitch, 0, 255));
        } else {
            setUV(p.uvA, c[0].u - baseU, c[0].v - baseV);
            setUV(p.uvB, c[i].u - baseU, c[i].v - baseV);
            setUV(p.uvC, c[i + 1].u - baseU, c[i + 1].v - baseV);
        }
        p.clutIndex = clutFor(0, light);
        p.tpage.setPageX(pageX).setPageY(pageY)
            .set(psyqo::Prim::TPageAttr::Tex4Bits)
            .setDithering(false);
        p.setColorA({{.r = 128, .g = 128, .b = 128}});
        p.setColorB({{.r = 128, .g = 128, .b = 128}});
        p.setColorC({{.r = 128, .g = 128, .b = 128}});
        submit(f);
    }
}

void emitFlatPair(FlatVert *base, int n, const SectorDef &sec, int light,
                  bool doFloor, bool doCeil, int32_t uScroll) {
    FlatVert a[MAX_POLY], b[MAX_POLY];
    n = clipFlatPoly(base, n, a, kLeftPlane);
    if (n < 3) return;
    n = clipFlatPoly(a, n, b, kRightPlane);
    if (n < 3) return;
    if (doFloor) {
        emitFlatSurface(b, n, sec.floor, sec.floortex, light, false, uScroll);
    }
    if (doCeil) {
        emitFlatSurface(b, n, sec.ceil, sec.ceiltex, light,
                        sec.ceiltex == NO_INDEX, 0);
    }
}

int buildFlatVerts(const VertexDef *src, int n, FlatVert *out, int32_t uScroll) {
    if (n > MAX_POLY) {
        FIXED_COUNT(pieceCap);
        n = MAX_POLY;
    }
    for (int i = 0; i < n; i++) {
        toCamera(src[i].x, src[i].y, &out[i].ax, &out[i].az);
        out[i].u = fixedToInt(src[i].x) / 2 + uScroll;
        out[i].v = fixedToInt(src[i].y) / 2;
    }
    return n;
}

bool outlineFitsUV(const FlatVert *v, int n, uint16_t texIndex) {
    if (n < 3) return true;
    TexInfo tex;
    if (!texInfo(texIndex, &tex)) return true;
    int32_t minU = v[0].u, maxU = v[0].u, minV = v[0].v, maxV = v[0].v;
    for (int i = 1; i < n; i++) {
        if (v[i].u < minU) minU = v[i].u;
        if (v[i].u > maxU) maxU = v[i].u;
        if (v[i].v < minV) minV = v[i].v;
        if (v[i].v > maxV) maxV = v[i].v;
    }
    return (maxU - minU) + (tex.w - 1) <= 255 && (maxV - minV) + (tex.h - 1) <= 255;
}

void emitFlatPieces(uint16_t first, int count, const SectorDef &sec, int light,
                    bool doFloor, bool doCeil, int32_t uScroll, PrimKind kind) {
#ifndef POOM_PROFILE
    (void)kind;
#endif
    if ((uint32_t)first + count > g_lvl->numFlats) return;
    for (int i = 0; i < count; i++) {
        const FlatPoly &poly = g_lvl->flats[first + i];
        FlatVert piece[MAX_POLY];
        int pn = buildFlatVerts(&g_lvl->flatVerts[poly.firstVert], poly.numVerts, piece, 0);
#ifdef POOM_PROFILE
        g_primKind = kind;
#endif
        emitFlatPair(piece, pn, sec, light, doFloor, doCeil, uScroll);
    }
}


struct WallSpan {
    fixed_t ax, az, w;
    int32_t sx;
    int32_t u;
};

void emitWallQuad(const WallSpan &a, const WallSpan &b, fixed_t topZ,
                  fixed_t bottomZ, int32_t vTop, int32_t vBottom,
                  const TexInfo &tex, int light, bool masked) {
#ifdef POOM_PROFILE
    g_primKind = PK_WALL;
#endif
    int32_t ya0 = screenY(topZ, a.w), ya1 = screenY(bottomZ, a.w);
    int32_t yb0 = screenY(topZ, b.w), yb1 = screenY(bottomZ, b.w);
    int32_t va0 = vTop, va1 = vBottom, vb0 = vTop, vb1 = vBottom;
    if (!clipEdgeY(ya0, ya1, va0, va1)) return;
    if (!clipEdgeY(yb0, yb1, vb0, vb1)) return;

    int32_t minV = va0;
    if (va1 < minV) minV = va1;
    if (vb0 < minV) minV = vb0;
    if (vb1 < minV) minV = vb1;
    int32_t baseU = alignDown(a.u < b.u ? a.u : b.u, tex.w);
    int32_t baseV = alignDown(minV, tex.h);
    int32_t ua = a.u - baseU, ub = b.u - baseU;
    va0 -= baseV;
    va1 -= baseV;
    vb0 -= baseV;
    vb1 -= baseV;
    if (ua > 255 || ub > 255) return;
    if (va0 > 255 || va1 > 255 || vb0 > 255 || vb1 > 255) return;

    auto *f = alloc<psyqo::Prim::TexturedQuad>();
    if (!f) return;
    auto &p = f->prim;
    p = psyqo::Prim::TexturedQuad();
    p.pointA = {{.x = gpuX(a.sx), .y = gpuY(ya0)}};
    p.pointB = {{.x = gpuX(b.sx), .y = gpuY(yb0)}};
    p.pointC = {{.x = gpuX(a.sx), .y = gpuY(ya1)}};
    p.pointD = {{.x = gpuX(b.sx), .y = gpuY(yb1)}};
    setUV(p.uvA, ua, va0);
    setUV(p.uvB, ub, vb0);
    setUV(p.uvC, ua, va1);
    setUV(p.uvD, ub, vb1);
    p.clutIndex = clutFor(masked ? 1 : 0, light);
    p.tpage.setPageX(tex.pageX).setPageY(tex.pageY)
        .set(psyqo::Prim::TPageAttr::Tex4Bits)
        .setDithering(false);
    p.setColor({{.r = 128, .g = 128, .b = 128}});
    submit(f);
}

void drawWall(const SegDef &seg, const CamVert &c0, const CamVert &c1,
              fixed_t topZ, fixed_t bottomZ, int32_t vTopWorld,
              uint16_t texIndex, int light255, bool masked,
              fixed_t wx0, fixed_t wy0, fixed_t wx1, fixed_t wy1) {
    if (topZ <= bottomZ) return;
    TexInfo tex;
    if (!texInfo(texIndex, &tex)) return;

    int32_t u0 = fixedToInt(seg.major ? wy0 : wx0) / 2;
    int32_t u1 = fixedToInt(seg.major ? wy1 : wx1) / 2;

    int32_t vTop = fixedToInt(vTopWorld - topZ) / 2;
    int32_t vBottom = fixedToInt(vTopWorld - bottomZ) / 2;
    if (vBottom - vTop > 255) return;

    int screenSpan = c1.sx - c0.sx;
    if (screenSpan < 0) screenSpan = -screenSpan;
    fixed_t zlo = c0.az < c1.az ? c0.az : c1.az;
    fixed_t zhi = c0.az < c1.az ? c1.az : c0.az;
    int steps = 1;
    while (steps < MAX_WALL_STEPS && (int64_t)zhi * 5 > (int64_t)zlo * 8) {
        zlo = (fixed_t)(((int64_t)zlo * 8) / 5);
        steps++;
    }
    int spanSteps = screenSpan / 64 + 1;
    if (spanSteps > steps) steps = spanSteps;
    int uSpan = u1 > u0 ? u1 - u0 : u0 - u1;
    int uSteps = uSpan / 200 + 1;
    if (uSteps > steps) steps = uSteps;
    if (steps > MAX_WALL_STEPS) steps = MAX_WALL_STEPS;

    setWindow(tex);

    WallSpan prev;
    prev.ax = c0.ax;
    prev.az = c0.az;
    prev.w = c0.w;
    prev.sx = c0.sx;
    prev.u = u0;
    int light = lightForW(light255, c0.w);

    for (int k = 1; k <= steps; k++) {
        WallSpan cur;
        if (k == steps) {
            cur.ax = c1.ax;
            cur.az = c1.az;
            cur.w = c1.w;
            cur.sx = c1.sx;
            cur.u = u1;
        } else {
            FIXED_COUNT(div32);
            fixed_t s = (fixed_t)(((uint32_t)k << 16) / (uint32_t)steps);
            fixed_t num = fmul(s, c0.az);
            fixed_t den = num + fmul(FRACUNIT - s, c1.az);
            fixed_t t = den ? fdiv(num, den) : s;
            cur.ax = c0.ax + fmul(c1.ax - c0.ax, t);
            cur.az = c0.az + fmul(c1.az - c0.az, t);
            cur.w = projW(cur.az);
            cur.sx = screenX(cur.ax, cur.w);
            cur.u = u0 + fixedToInt(fmul(intToFixed(u1 - u0), t));
        }
        int l = lightForW(light255, cur.w);
        emitWallQuad(prev, cur, topZ, bottomZ, vTop, vBottom, tex,
                     (light + l) >> 1, masked);
        light = l;
        prev = cur;
    }
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
        FlatVert base[MAX_POLY];
        int n = ss.numSegs;
        bool outlineWhole = n <= MAX_FLAT_VERTS;
        fixed_t nearest = INT32_MAX;
        if (outlineWhole) {
            for (int i = 0; i < n; i++) {
                const VertexDef &vd = g_lvl->verts[g_lvl->segs[ss.firstSeg + i].v];
                toCamera(vd.x, vd.y, &base[i].ax, &base[i].az);
                base[i].u = fixedToInt(vd.x) / 2;
                base[i].v = fixedToInt(vd.y) / 2;
                if (base[i].az < nearest) nearest = base[i].az;
            }
        }

        int32_t uScroll = gameSectorScroll(ss.sector);
        bool outlineFloor = false, outlineCeil = false;
        bool stripFloor = false, stripCeil = false;
        if (outlineWhole && nearest > FLAT_LOD_DIST) {
            bool floorFits = outlineFitsUV(base, n, sec.floortex);
            bool ceilFits = sec.ceiltex == NO_INDEX || outlineFitsUV(base, n, sec.ceiltex);
            outlineFloor = drawFloor && floorFits;
            outlineCeil = drawCeil && ceilFits;
            if (outlineFloor || outlineCeil) {
                if (ss.numSegs > n) FIXED_COUNT(outlineCut);
#ifdef POOM_PROFILE
                g_primKind = PK_FLAT_COARSE;
#endif
                emitFlatPair(base, n, sec, light, outlineFloor, outlineCeil, uScroll);
            }
            if (ss.numStrips) {
                stripFloor = drawFloor && !floorFits;
                stripCeil = drawCeil && !ceilFits;
                if (stripFloor || stripCeil) {
                    emitFlatPieces(ss.firstFlat + ss.numFlats, ss.numStrips, sec, light,
                                   stripFloor, stripCeil, uScroll, PK_FLAT_STRIP);
                }
            }
        }
        bool gridFloor = drawFloor && !outlineFloor && !stripFloor;
        bool gridCeil = drawCeil && !outlineCeil && !stripCeil;
        if (gridFloor || gridCeil) {
            emitFlatPieces(ss.firstFlat, ss.numFlats, sec, light, gridFloor, gridCeil,
                           uScroll, PK_FLAT_FINE);
        }
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
                         false, wx0, wy0, wx1, wy1);
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
                     wx0, wy0, wx1, wy1);
        }
        if (bottom < obottom && sd.bottomtex != NO_INDEX) {
            drawWall(seg, v0, v1, obottom, bottom, obottom, sd.bottomtex,
                     light, false, wx0, wy0, wx1, wy1);
        }
        if (sd.midtex != NO_INDEX) {
            fixed_t mt = otop < top ? otop : top;
            fixed_t mb = obottom > bottom ? obottom : bottom;
            drawWall(seg, v0, v1, mt, mb, mt, sd.midtex, light,
                     g_lvl->isTransparent(sd.midtex), wx0, wy0, wx1, wy1);
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

    g_rb = &g_buffers[gpu.getParity()];
    g_rb->used = 0;
    g_rb->ot.clear();
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
