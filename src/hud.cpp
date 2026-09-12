
#include "poom.hh"

#include "psyqo/primitives/quads.hh"
#include "psyqo/primitives/rectangles.hh"
#include "psyqo/xprintf.h"

int gameHealth();
int gameArmor();
int gameHitFlash();
int gameWeaponAmmo();
int gameWeaponAmmoIcon();
int gameSectorLight();
int gameWeaponY();
const char *gameMessage();
bool gameIsDead();
int gameDeathTicks();
const StateDef *gameWeaponState();

namespace {


uint32_t g_fpsLast;
bool g_fpsStarted;
int g_fpsFrames;
int g_fps = -1;
bool g_showFps;

void tickFps(psyqo::GPU &gpu) {
    uint32_t now = gpu.now();
    if (!g_fpsStarted) {
        g_fpsStarted = true;
        g_fpsLast = now;
        return;
    }
    g_fpsFrames++;
    uint32_t elapsed = now - g_fpsLast;
    if (elapsed >= 1000000) {
        g_fps = (int)(((uint64_t)g_fpsFrames * 1000000 + elapsed / 2) / elapsed);
        g_fpsFrames = 0;
        g_fpsLast = now;
    }
}

constexpr int COL_HEALTH = 12;
constexpr int COL_ARMOR = 3;
constexpr int COL_AMMO = 9;
constexpr int COL_MSG = 15;
constexpr int COL_FPS = 11;

constexpr uint8_t GLYPH_HEART = 136;
constexpr uint8_t GLYPH_FIGURE = 138;

constexpr int FONT_NUM = 2, FONT_DEN = 1;

constexpr int HUD_LIFT = 8;
constexpr int scaleX(int v) { return v * 5 / 2; }
constexpr int scaleY(int v) { return v * 15 / 8; }
constexpr int hudY(int v) { return scaleY(v) - HUD_LIFT; }

void printIconValue(psyqo::GPU &gpu, uint8_t glyph, int value, int x, int y,
                    int colour, bool shadowed) {
    char buf[12];
    sprintf(buf, "%d", value);
    if (shadowed) {
        int adv = fontDrawGlyph(gpu, glyph, x, y + 1, 0, FONT_NUM, FONT_DEN);
        fontPrint(gpu, buf, x + adv, y + 1, 0, FONT_NUM, FONT_DEN);
        adv = fontDrawGlyph(gpu, glyph, x, y, colour, FONT_NUM, FONT_DEN);
        fontPrint(gpu, buf, x + adv, y, colour, FONT_NUM, FONT_DEN);
    } else {
        int adv = fontDrawGlyph(gpu, glyph, x, y, colour, FONT_NUM, FONT_DEN);
        fontPrint(gpu, buf, x + adv, y, colour, FONT_NUM, FONT_DEN);
    }
}

}

void hudToggleFps() { g_showFps = !g_showFps; }

void gameDrawHud(psyqo::GPU &gpu) {
    renderResetTextureWindowNow(gpu);
    if (!fontReady()) return;

    tickFps(gpu);
    if (g_showFps && g_fps >= 0) {
        char buf[16];
        sprintf(buf, "%d fps", g_fps);
        fontPrint(gpu, buf, 6, 6, COL_FPS, FONT_NUM, FONT_DEN);
    }

    if (gameIsDead()) {
        int ticks = gameDeathTicks();
        if ((ticks / 30) % 4 < 2) {
            const char *msg = "you died";
            fontPrint(gpu, msg,
                      CENTER_X - fontTextWidth(msg, FONT_NUM, FONT_DEN) / 2,
                      scaleY(108), COL_HEALTH, FONT_NUM, FONT_DEN);
            if (ticks > 30) {
                const char *hint = "fire" "\x17" "restart";
                fontPrint(gpu, hint,
                          CENTER_X - fontTextWidth(hint, FONT_NUM, FONT_DEN) / 2,
                          scaleY(120), COL_HEALTH, FONT_NUM, FONT_DEN);
            }
        }
        return;
    }

    const StateDef *st = gameWeaponState();
    if (st && st->numSides > 0) {
        const FrameDef *fr = g_assets.frame(st->sides[0]);
        constexpr int WS = 2;
        int w = (fr->w + 1) * WS;
        int h = (fr->h + 1) * WS;
        int x = CENTER_X - (fr->xoffset + gameWeaponBobX()) * WS;
        int y = scaleY(132) -
                (fr->yoffset - gameWeaponBobY() + gameWeaponY()) * WS;

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

    printIconValue(gpu, GLYPH_HEART, gameHealth(), scaleX(2), hudY(110),
                   COL_HEALTH, false);
    printIconValue(gpu, GLYPH_FIGURE, gameArmor(), scaleX(2), hudY(120),
                   COL_ARMOR, false);

    int ammo = gameWeaponAmmo();
    int ammoIcon = gameWeaponAmmoIcon();
    if (ammo >= 0 && ammoIcon > 0) {
        printIconValue(gpu, (uint8_t)ammoIcon, ammo, scaleX(106), hudY(120),
                       COL_AMMO, true);
    }

    uint8_t slots[8], colors[8], icons[8];
    int keys = gameHeldKeys(slots, colors, icons, 8);
    for (int i = 0; i < keys; i++) {
        int kx = scaleX(102 + slots[i] * 7);
        int ky = hudY(112);
        fontDrawGlyph(gpu, icons[i], kx, ky + 1, 0, FONT_NUM, FONT_DEN);
        fontDrawGlyph(gpu, icons[i], kx, ky, colors[i] & 15, FONT_NUM, FONT_DEN);
    }

    if (const char *msg = gameMessage()) {
        fontPrintShadowed(gpu, msg,
                          CENTER_X - fontTextWidth(msg, FONT_NUM, FONT_DEN) / 2,
                          scaleY(50), COL_MSG, FONT_NUM, FONT_DEN);
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
