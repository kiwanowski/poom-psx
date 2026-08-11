
#include "poom.hh"

#include "psyqo/font.hh"
#include "psyqo/primitives/quads.hh"
#include "psyqo/primitives/rectangles.hh"
#include "psyqo/xprintf.h"

int gameHealth();
int gameArmor();
int gameHitFlash();
int gameWeaponAmmo();
int gameSectorLight();
bool gameIsDead();
int gameDeathTicks();
const StateDef *gameWeaponState();

namespace {

psyqo::Font<> *g_font;

const psyqo::Color kPico8[16] = {
    {{.r = 0x00, .g = 0x00, .b = 0x00}}, {{.r = 0x1d, .g = 0x2b, .b = 0x53}},
    {{.r = 0x7e, .g = 0x25, .b = 0x53}}, {{.r = 0x00, .g = 0x87, .b = 0x51}},
    {{.r = 0xab, .g = 0x52, .b = 0x36}}, {{.r = 0x5f, .g = 0x57, .b = 0x4f}},
    {{.r = 0xc2, .g = 0xc3, .b = 0xc7}}, {{.r = 0xff, .g = 0xf1, .b = 0xe8}},
    {{.r = 0xff, .g = 0x00, .b = 0x4d}}, {{.r = 0xff, .g = 0xa3, .b = 0x00}},
    {{.r = 0xff, .g = 0xec, .b = 0x27}}, {{.r = 0x00, .g = 0xe4, .b = 0x36}},
    {{.r = 0x29, .g = 0xad, .b = 0xff}}, {{.r = 0x83, .g = 0x76, .b = 0x9c}},
    {{.r = 0xff, .g = 0x77, .b = 0xa8}}, {{.r = 0xff, .g = 0xcc, .b = 0xaa}},
};

constexpr int COL_HEALTH = 12;
constexpr int COL_ARMOR = 3;
constexpr int COL_AMMO = 9;
constexpr int COL_SHADOW = 0;

constexpr int scaleX(int v) { return v * 5 / 2; }
constexpr int scaleY(int v) { return v * 15 / 8; }

void fillRect(psyqo::GPU &gpu, int x, int y, int w, int h, psyqo::Color c) {
    psyqo::Prim::Rectangle r;
    r.position = {{.x = (int16_t)x, .y = (int16_t)y}};
    r.size = {{.w = (int16_t)w, .h = (int16_t)h}};
    r.setColor(c);
    r.setOpaque();
    gpu.sendPrimitive(r);
}

void drawHeart(psyqo::GPU &gpu, int x, int y, psyqo::Color c) {
    fillRect(gpu, x + 1, y + 1, 4, 4, c);
    fillRect(gpu, x + 7, y + 1, 4, 4, c);
    fillRect(gpu, x, y + 4, 12, 3, c);
    fillRect(gpu, x + 1, y + 7, 10, 2, c);
    fillRect(gpu, x + 3, y + 9, 6, 2, c);
    fillRect(gpu, x + 5, y + 11, 2, 2, c);
}

void drawShield(psyqo::GPU &gpu, int x, int y, psyqo::Color c) {
    fillRect(gpu, x, y + 1, 11, 7, c);
    fillRect(gpu, x + 1, y + 8, 9, 2, c);
    fillRect(gpu, x + 3, y + 10, 5, 2, c);
    fillRect(gpu, x + 4, y + 12, 3, 1, c);
}

void drawBullet(psyqo::GPU &gpu, int x, int y, psyqo::Color c) {
    fillRect(gpu, x + 2, y + 1, 4, 2, c);
    fillRect(gpu, x + 1, y + 3, 6, 8, c);
}

void drawKey(psyqo::GPU &gpu, int x, int y, psyqo::Color c) {
    fillRect(gpu, x, y + 2, 6, 6, c);
    fillRect(gpu, x + 6, y + 4, 6, 2, c);
    fillRect(gpu, x + 9, y + 6, 2, 3, c);
}

void printShadowed(psyqo::GPU &gpu, const char *text, int x, int y, int colour) {
    if (!g_font) return;
    g_font->print(gpu, text, {{.x = (int16_t)x, .y = (int16_t)(y + 1)}},
                  kPico8[COL_SHADOW]);
    g_font->print(gpu, text, {{.x = (int16_t)x, .y = (int16_t)y}},
                  kPico8[colour & 15]);
}

}

void hudSetFont(psyqo::Font<> *f) { g_font = f; }

void gameDrawHud(psyqo::GPU &gpu) {
    renderResetTextureWindowNow(gpu);

    if (gameIsDead()) {
        if (!g_font) return;
        int ticks = gameDeathTicks();
        if ((ticks / 30) % 4 < 2) {
            g_font->print(gpu, "YOU DIED",
                          {{.x = (int16_t)(CENTER_X - 32), .y = 196}},
                          kPico8[COL_HEALTH]);
            if (ticks > 30) {
                g_font->print(gpu, "FIRE - RESTART",
                              {{.x = (int16_t)(CENTER_X - 56), .y = 218}},
                              kPico8[COL_HEALTH]);
            }
        }
        return;
    }

    const StateDef *st = gameWeaponState();
    if (st && st->numSides > 0) {
        const FrameDef *fr = g_assets.frame(st->sides[0]);
        int w = (fr->w + 1) * 5 / 2;
        int h = (fr->h + 1) * 5 / 2;
        int x = CENTER_X - (fr->xoffset + gameWeaponBobX()) * 5 / 2;
        int y = scaleY(132) - (fr->yoffset - gameWeaponBobY()) * 5 / 2;

        psyqo::Prim::TexturedQuad q;
        q.pointA = {{.x = (int16_t)x, .y = (int16_t)y}};
        q.pointB = {{.x = (int16_t)(x + w), .y = (int16_t)y}};
        q.pointC = {{.x = (int16_t)x, .y = (int16_t)(y + h)}};
        q.pointD = {{.x = (int16_t)(x + w), .y = (int16_t)(y + h)}};
        q.uvA.u = fr->u;
        q.uvA.v = fr->v;
        q.uvB.u = (uint8_t)(fr->u + fr->w);
        q.uvB.v = fr->v;
        q.uvC.u = fr->u;
        q.uvC.v = (uint8_t)(fr->v + fr->h);
        q.uvD.u = (uint8_t)(fr->u + fr->w);
        q.uvD.v = (uint8_t)(fr->v + fr->h);
        int light = st->bright ? 8 : clampi(gameSectorLight() * 15 / 255, 0, 15);
        q.clutIndex = psyqo::PrimPieces::ClutIndex(
            VRAM_CLUT_X >> 4, VRAM_CLUT_Y + clutRow(1 + fr->variant, light));
        q.tpage.setPageX(g_assets.spritePages[fr->page].px)
            .setPageY(g_assets.spritePages[fr->page].py)
            .set(psyqo::Prim::TPageAttr::Tex4Bits)
            .setDithering(false);
        q.setColor({{.r = 128, .g = 128, .b = 128}});
        psyqo::Prim::TPage tp;
        tp.attr.copy(q.tpage);
        gpu.sendPrimitive(tp);
        gpu.sendPrimitive(q);
    }

    if (!g_font) return;
    char buf[16];

    const int leftX = scaleX(2);
    const int healthY = scaleY(110) - 4;
    const int armorY = scaleY(120) - 4;

    drawHeart(gpu, leftX, healthY + 2, kPico8[COL_HEALTH]);
    sprintf(buf, "%d", gameHealth());
    g_font->print(gpu, buf, {{.x = (int16_t)(leftX + 14), .y = (int16_t)healthY}},
                  kPico8[COL_HEALTH]);

    drawShield(gpu, leftX, armorY + 2, kPico8[COL_ARMOR]);
    sprintf(buf, "%d", gameArmor());
    g_font->print(gpu, buf, {{.x = (int16_t)(leftX + 14), .y = (int16_t)armorY}},
                  kPico8[COL_ARMOR]);

    int ammo = gameWeaponAmmo();
    if (ammo >= 0) {
        const int ammoX = scaleX(106);
        drawBullet(gpu, ammoX, armorY + 2, kPico8[COL_AMMO]);
        sprintf(buf, "%d", ammo);
        printShadowed(gpu, buf, ammoX + 10, armorY, COL_AMMO);
    }

    uint8_t slots[8], colors[8];
    int keys = gameHeldKeys(slots, colors, 8);
    for (int i = 0; i < keys; i++) {
        int kx = scaleX(102 + slots[i] * 7);
        if (kx > SCREEN_W - 14) kx = SCREEN_W - 14;
        drawKey(gpu, kx, scaleY(112), kPico8[colors[i] & 15]);
    }

    int flash = gameHitFlash();
    if (flash > 0) {
        psyqo::Prim::Rectangle r;
        r.position = {{.x = 0, .y = 0}};
        r.size = {{.w = SCREEN_W, .h = SCREEN_H}};
        r.setColor({{.r = (uint8_t)(flash * 10), .g = 0, .b = 0}});
        r.setSemiTrans();
        psyqo::Prim::TPage tp;
        tp.attr.set(psyqo::Prim::TPageAttr::HalfBackAndHalfFront);
        gpu.sendPrimitive(tp);
        gpu.sendPrimitive(r);
    }
}
