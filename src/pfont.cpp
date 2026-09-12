
#include "poom.hh"

#include "psyqo/primitives/quads.hh"
#include "psyqo/primitives/sprites.hh"

namespace {

#pragma pack(push, 1)
struct FontTable {
    uint8_t lineHeight;
    uint8_t cellH;
    uint8_t numGlyphs;
    uint8_t pad;
    uint8_t charmap[256];
    struct Glyph {
        uint8_t u, v, w, advance;
    } glyphs[1];
};
#pragma pack(pop)

const FontTable *g_font;
uint8_t g_fontPageX, g_fontPageY;

constexpr int kPageScale = 2;

}

void fontSet(const void *table, int pageX, int pageY) {
    g_font = (const FontTable *)table;
    g_fontPageX = (uint8_t)pageX;
    g_fontPageY = (uint8_t)pageY;
}

bool fontReady() { return g_font != nullptr; }

int fontLineHeight(int scaleNum, int scaleDen) {
    if (!g_font) return 0;
    return g_font->lineHeight * scaleNum / scaleDen;
}

int fontTextWidth(const char *text, int scaleNum, int scaleDen) {
    if (!g_font) return 0;
    int w = 0;
    for (const uint8_t *p = (const uint8_t *)text; *p; p++) {
        uint8_t g = g_font->charmap[*p];
        if (g == 0xFF) continue;
        w += g_font->glyphs[g].advance;
    }
    return w * scaleNum / scaleDen;
}

int fontDrawGlyph(psyqo::GPU &gpu, uint8_t code, int x, int y, int colour,
                  int scaleNum, int scaleDen) {
    if (!g_font) return 0;
    uint8_t gi = g_font->charmap[code];
    if (gi == 0xFF) return 0;
    const auto &g = g_font->glyphs[gi];
    int w = g.w * scaleNum / scaleDen;
    int h = g_font->cellH * scaleNum / scaleDen;
    int pu = g.u * kPageScale, pv = g.v * kPageScale;
    int pw = g.w * kPageScale, ph = g_font->cellH * kPageScale;
    const psyqo::PrimPieces::ClutIndex clut(
        VRAM_CLUT_X >> 4, VRAM_CLUT_Y + FONT_CLUT_BASE + (colour & 15));

    psyqo::Prim::TPage tp;
    tp.attr.setPageX(g_fontPageX).setPageY(g_fontPageY)
        .set(psyqo::Prim::TPageAttr::Tex4Bits)
        .setDithering(false);
    gpu.sendPrimitive(tp);

    if (scaleNum == kPageScale * scaleDen) {
        psyqo::Prim::Sprite sprite;
        sprite.position = {{.x = (int16_t)x, .y = (int16_t)y}};
        sprite.texInfo.u = (uint8_t)pu;
        sprite.texInfo.v = (uint8_t)pv;
        sprite.texInfo.clut = clut;
        sprite.size = {{.w = (int16_t)pw, .h = (int16_t)ph}};
        gpu.sendPrimitive(sprite);
    } else {
        psyqo::Prim::TexturedQuad q;
        q.pointA = {{.x = (int16_t)x, .y = (int16_t)y}};
        q.pointB = {{.x = (int16_t)(x + w), .y = (int16_t)y}};
        q.pointC = {{.x = (int16_t)x, .y = (int16_t)(y + h)}};
        q.pointD = {{.x = (int16_t)(x + w), .y = (int16_t)(y + h)}};
        q.uvA.u = (uint8_t)pu;
        q.uvA.v = (uint8_t)pv;
        q.uvB.u = (uint8_t)(pu + pw);
        q.uvB.v = (uint8_t)pv;
        q.uvC.u = (uint8_t)pu;
        q.uvC.v = (uint8_t)(pv + ph);
        q.uvD.u = (uint8_t)(pu + pw);
        q.uvD.v = (uint8_t)(pv + ph);
        q.clutIndex = clut;
        q.tpage.copy(tp.attr);
        q.setColor({{.r = 128, .g = 128, .b = 128}});
        gpu.sendPrimitive(q);
    }
    return g.advance * scaleNum / scaleDen;
}

void fontPrint(psyqo::GPU &gpu, const char *text, int x, int y, int colour,
               int scaleNum, int scaleDen) {
    if (!g_font) return;
    for (const uint8_t *p = (const uint8_t *)text; *p; p++) {
        x += fontDrawGlyph(gpu, *p, x, y, colour, scaleNum, scaleDen);
    }
}

void fontPrintShadowed(psyqo::GPU &gpu, const char *text, int x, int y,
                       int colour, int scaleNum, int scaleDen) {
    fontPrint(gpu, text, x, y + scaleNum / scaleDen, 0, scaleNum, scaleDen);
    fontPrint(gpu, text, x, y, colour, scaleNum, scaleDen);
}
