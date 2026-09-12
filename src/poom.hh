#pragma once

#include <stdint.h>

#include "data.hh"
#include "fixed.hh"
#include "psyqo/font.hh"
#include "psyqo/gpu.hh"
#include "psyqo/ordering-table.hh"
#include "psyqo/advancedpad.hh"


static constexpr int SCREEN_W = 320;
static constexpr int SCREEN_H = 240;
static constexpr int CENTER_X = SCREEN_W / 2;
static constexpr int CENTER_Y = SCREEN_H / 2;
static constexpr fixed_t FOCAL = intToFixed(240);

static constexpr fixed_t NEAR_Z = intToFixed(8);
static constexpr fixed_t FAR_Z = intToFixed(854);
static constexpr fixed_t FOV_NUM = 2;
static constexpr fixed_t FOV_DEN = 3;

static constexpr fixed_t VIEW_HEIGHT = intToFixed(45);


static constexpr int PAGE_COL_FIRST = 5;
static constexpr int PAGE_COL_LAST = 14;
static constexpr int PAGE_COLS = PAGE_COL_LAST - PAGE_COL_FIRST + 1;
static constexpr int MAX_VRAM_PAGES = PAGE_COLS * 2;

static constexpr int VRAM_CLUT_X = 960;
static constexpr int VRAM_CLUT_Y = 0;
static constexpr int NUM_CLUTS = 112;
static constexpr int FONT_CLUT_BASE = 96;

static constexpr int MAX_PAGES = 16;

static constexpr int clutRow(int variant, int light) { return variant * 16 + light; }


struct Assets {
    const FrameDef *frames = nullptr;
    const uint16_t *frameMap = nullptr;
    uint32_t numFrameMap = 0;
    const ActorDef *actors = nullptr;
    uint32_t numActors = 0;
    const StateDef *states = nullptr;
    uint32_t numStates = 0;

    const TexDef *textures = nullptr;
    uint32_t numTextures = 0;
    TexDef sky;

    struct PageLoc { uint8_t px, py; };
    PageLoc spritePages[MAX_PAGES];
    PageLoc texPages[MAX_PAGES];

    const ActorDef *actorByIndex(uint16_t i) const {
        return i < numActors ? &actors[i] : nullptr;
    }
    const FrameDef *frame(uint16_t uniqueIndex) const { return &frames[uniqueIndex]; }
};

struct Level {
    const LevelHeader *header = nullptr;
    const SectorDef *sectorDefs = nullptr;
    SectorDef *sectors = nullptr;
    const SideDef *sides = nullptr;
    SideDef *mutSides = nullptr;
    const VertexDef *verts = nullptr;
    const LineDef *lines = nullptr;
    const SegDef *segs = nullptr;
    const SubSectorDef *subs = nullptr;
    const FlatPoly *flats = nullptr;
    const VertexDef *flatVerts = nullptr;
    const uint8_t *pvs = nullptr;
    uint32_t pvsStride = 0;
    const NodeDef *nodes = nullptr;
    const ThingDef *things = nullptr;
    const uint16_t *onoff = nullptr;
    const uint16_t *transparent = nullptr;
    const uint32_t *specialOfs = nullptr;
    const uint8_t *specials = nullptr;

    uint32_t numSectors = 0, numSides = 0, numVerts = 0, numLines = 0;
    uint32_t numSegs = 0, numSubs = 0, numFlats = 0, numFlatVerts = 0;
    uint32_t numNodes = 0, numThings = 0, numOnOff = 0, numTransparent = 0;
    uint32_t numSpecials = 0;

    bool inPVS(uint16_t from, uint16_t to) const {
        return (pvs[from * pvsStride + (to >> 3)] >> (to & 7)) & 1;
    }
    bool isTransparent(uint16_t tex) const {
        for (uint32_t i = 0; i < numTransparent; i++)
            if (transparent[i] == tex) return true;
        return false;
    }
    uint16_t switchTexture(uint16_t tex) const {
        for (uint32_t i = 0; i < numOnOff; i++)
            if (onoff[i * 2] == tex) return onoff[i * 2 + 1];
        return tex;
    }
    const SpecialHeader *special(uint16_t index) const {
        if (index == NO_INDEX) return nullptr;
        return (const SpecialHeader *)(specials + specialOfs[index]);
    }
};


static constexpr int MAX_THINGS = 512;
static constexpr int MAX_SUBS_PER_THING = 8;

struct Thing {
    fixed_t x, y, z;
    angle_t angle;
    const ActorDef *actor;
    int16_t health;
    int16_t armor;
    uint16_t sector;
    uint16_t ssector;
    int16_t stateIndex;
    int16_t ticks;
    int16_t delay;
    const StateDef *state;
    fixed_t velx, vely, velz;
    fixed_t forcex, forcey;
    Thing *target;
    Thing *owner;
    uint16_t special;
    uint8_t dead;
    uint8_t active;
    uint8_t isPlayer;
    uint8_t intersectId;
    int16_t dmgTtl;
    int16_t chaseTtl;
    uint16_t subs[MAX_SUBS_PER_THING];
    uint8_t numSubs;
};


struct Camera {
    fixed_t x, y, z;
    angle_t angle;
    int pitch;
    fixed_t ca, sa;
    fixed_t m4, m12;
};

void renderInit(psyqo::GPU &gpu);
void renderScene(psyqo::GPU &gpu, const Camera &cam);
void renderSetLevel(Level *level, Assets *assets);
void renderAddSprite(const Thing *t);
void renderResetTextureWindowNow(psyqo::GPU &gpu);

uint16_t findSubSector(const Level &lvl, fixed_t x, fixed_t y);


void gameSetSkill(int skill);
int gameSkill();
void gameInit(Level *level, Assets *assets);
void gameUpdate(psyqo::AdvancedPad &pad);
Camera gameCamera();
void gameDrawHud(psyqo::GPU &gpu);
void gameCollectSprites();
bool gameExitRequested();
bool gameIsDead();
int gameDeathTicks();
bool gameRestartRequested();
int gameHeldKeys(uint8_t *slots, uint8_t *colors, uint8_t *icons, int max);
int gameSecrets();
const char *gameMessage();
int gameWeaponAmmoIcon();
int gameSectorScroll(uint16_t sector);
int gameWeaponBobX();
int gameWeaponBobY();
int gameWeaponY();

extern Level g_level;
extern Assets g_assets;
extern int g_ambientLight;


void fontSet(const void *table, int pageX, int pageY);
bool fontReady();
int fontLineHeight(int scaleNum, int scaleDen);
int fontTextWidth(const char *text, int scaleNum, int scaleDen);
int fontDrawGlyph(psyqo::GPU &gpu, uint8_t code, int x, int y, int colour,
                  int scaleNum, int scaleDen);
void fontPrint(psyqo::GPU &gpu, const char *text, int x, int y, int colour,
               int scaleNum, int scaleDen);
void fontPrintShadowed(psyqo::GPU &gpu, const char *text, int x, int y,
                       int colour, int scaleNum, int scaleDen);

void hudToggleFps();
