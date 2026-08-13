
#include "assets.hh"
#include "poom.hh"
#include "psyqo/application.hh"
#include "psyqo/cdrom-device.hh"
#include "psyqo/font.hh"
#include "psyqo/gpu.hh"
#include "psyqo/iso9660-parser.hh"
#include "psyqo/primitives/rectangles.hh"
#include "psyqo/scene.hh"
#include "psyqo/simplepad.hh"
#include "psyqo/xprintf.h"

namespace {

static constexpr uint32_t LEVEL_ARENA = 288 * 1024;
static constexpr uint32_t SHARED_ARENA = 24 * 1024;

alignas(4) uint8_t g_levelArena[LEVEL_ARENA];
alignas(4) uint8_t g_actorArena[SHARED_ARENA];
alignas(4) uint8_t g_frameArena[4 * 1024];
alignas(4) uint8_t g_texTable[2 * 1024];
alignas(4) uint8_t g_fontTable[2048];

class Poom final : public psyqo::Application {
    void prepare() override;
    void createScene() override;

  public:
    psyqo::Font<> m_font;
    psyqo::CDRomDevice m_cdrom;
    psyqo::ISO9660Parser m_iso = psyqo::ISO9660Parser(&m_cdrom);
    psyqo::SimplePad m_pad;
    bool m_ready = false;
    const char *m_error = nullptr;
};

class PlayScene final : public psyqo::Scene {
    void start(StartReason reason) override;
    void frame() override;
};

Poom g_app;
PlayScene g_playScene;

struct LevelEntry { const char *name; const char *episode; };
const LevelEntry kLevels[] = {
    {"E1M1", "E1"}, {"E1M2", "E1"}, {"E1M3", "E1"},
    {"E2M1", "E2"}, {"E2M2", "E2"}, {"E2M3", "E2"},
};
constexpr int kNumLevels = sizeof(kLevels) / sizeof(kLevels[0]);
int g_levelIndex = 0;
const char *kLevelName = kLevels[0].name;
const char *kEpisode = kLevels[0].episode;

bool pageSlotToVRAM(int slot, int *px, int *py) {
    if (slot >= MAX_VRAM_PAGES) return false;
    *px = PAGE_COL_FIRST + (slot % PAGE_COLS);
    *py = slot / PAGE_COLS;
    return true;
}

int g_nextPageSlot = 0;
const char *g_loadedEpisode = nullptr;

bool loadEpisodeTextures() {
    char name[8];
    g_loadedEpisode = kEpisode;
    for (int i = 0; i < MAX_PAGES; i++) {
        sprintf(name, "%sTX%d", kEpisode, i);
        if (!assetsFind(name)) break;
        int px, py;
        if (!pageSlotToVRAM(g_nextPageSlot++, &px, &py)) return false;
        if (!assetsUploadPage(name, g_app.gpu(), px, py)) return false;
        g_assets.texPages[i].px = (uint8_t)px;
        g_assets.texPages[i].py = (uint8_t)py;
    }
    sprintf(name, "%sTBL", kEpisode);
    const LumpEntry *tbl = assetsFind(name);
    if (!assetsRead(tbl, g_texTable, sizeof(g_texTable))) return false;
    g_assets.sky = *(const TexDef *)g_texTable;
    g_assets.textures = (const TexDef *)(g_texTable + sizeof(TexDef));
    g_assets.numTextures = (tbl->size - sizeof(TexDef)) / sizeof(TexDef);
    return true;
}

bool loadSprites() {
    char name[8];
    for (int i = 0; i < MAX_PAGES; i++) {
        sprintf(name, "SPR%d", i);
        if (!assetsFind(name)) break;
        int px, py;
        if (!pageSlotToVRAM(g_nextPageSlot++, &px, &py)) return false;
        if (!assetsUploadPage(name, g_app.gpu(), px, py)) return false;
        g_assets.spritePages[i].px = (uint8_t)px;
        g_assets.spritePages[i].py = (uint8_t)py;
    }
    return true;
}

bool loadFont() {
    if (!assetsLoad("FONT", g_fontTable, sizeof(g_fontTable))) return false;
    int px, py;
    if (!pageSlotToVRAM(g_nextPageSlot++, &px, &py)) return false;
    if (!assetsUploadPage("FONTPG", g_app.gpu(), px, py)) return false;
    fontSet(g_fontTable, px, py);
    return true;
}

bool loadLevel(const char *name) {
    const LumpEntry *l = assetsFind(name);
    if (!assetsRead(l, g_levelArena, sizeof(g_levelArena))) return false;

    const LevelHeader *h = (const LevelHeader *)g_levelArena;
    if (h->magic[0] != 'P' || h->magic[1] != 'L') return false;
    Level &lv = g_level;
    lv.header = h;
    lv.pvsStride = h->pvsStride;
    auto blk = [&](int i) { return g_levelArena + h->blocks[i].offset; };

    lv.sectors = (SectorDef *)blk(LB_SECTORS);
    lv.numSectors = h->blocks[LB_SECTORS].count;
    lv.mutSides = (SideDef *)blk(LB_SIDES);
    lv.sides = lv.mutSides;
    lv.numSides = h->blocks[LB_SIDES].count;
    lv.verts = (const VertexDef *)blk(LB_VERTS);
    lv.numVerts = h->blocks[LB_VERTS].count;
    lv.lines = (const LineDef *)blk(LB_LINES);
    lv.numLines = h->blocks[LB_LINES].count;
    lv.segs = (const SegDef *)blk(LB_SEGS);
    lv.numSegs = h->blocks[LB_SEGS].count;
    lv.subs = (const SubSectorDef *)blk(LB_SUBS);
    lv.numSubs = h->blocks[LB_SUBS].count;
    lv.flats = (const FlatPoly *)blk(LB_FLATS);
    lv.numFlats = h->blocks[LB_FLATS].count;
    lv.flatVerts = (const VertexDef *)blk(LB_FLATVERTS);
    lv.numFlatVerts = h->blocks[LB_FLATVERTS].count;
    lv.pvs = (const uint8_t *)blk(LB_PVS);
    lv.nodes = (const NodeDef *)blk(LB_NODES);
    lv.numNodes = h->blocks[LB_NODES].count;
    lv.things = (const ThingDef *)blk(LB_THINGS);
    lv.numThings = h->blocks[LB_THINGS].count;
    lv.onoff = (const uint16_t *)blk(LB_ONOFF);
    lv.numOnOff = h->blocks[LB_ONOFF].count;
    lv.transparent = (const uint16_t *)blk(LB_TRANSPARENT);
    lv.numTransparent = h->blocks[LB_TRANSPARENT].count;
    lv.specialOfs = (const uint32_t *)blk(LB_SPECIALOFS);
    lv.numSpecials = h->blocks[LB_SPECIALOFS].count;
    lv.specials = (const uint8_t *)blk(LB_SPECIALS);
    return true;
}

bool restartLevel() {
    if (!loadLevel(kLevelName)) return false;
    renderSetLevel(&g_level, &g_assets);
    gameInit(&g_level, &g_assets);
    return true;
}

bool advanceLevel() {
    g_levelIndex++;
    kLevelName = kLevels[g_levelIndex].name;
    const char *episode = kLevels[g_levelIndex].episode;

    bool episodeChanged = (g_loadedEpisode == nullptr) ||
                          (g_loadedEpisode[1] != episode[1]);
    kEpisode = episode;
    if (episodeChanged) {
        g_nextPageSlot = 0;
        if (!loadEpisodeTextures()) return false;
        if (!loadSprites()) return false;
        if (!loadFont()) return false;
    }
    if (!loadLevel(kLevelName)) return false;
    renderSetLevel(&g_level, &g_assets);
    gameInit(&g_level, &g_assets);
    return true;
}

}

void Poom::prepare() {
    psyqo::GPU::Configuration config;
    config.set(psyqo::GPU::Resolution::W320)
        .set(psyqo::GPU::VideoMode::AUTO)
        .set(psyqo::GPU::ColorMode::C15BITS)
        .set(psyqo::GPU::Interlace::PROGRESSIVE);
    gpu().initialize(config);
    m_cdrom.prepare();
    m_cdrom.resetBlocking(gpu());
}

void Poom::createScene() {
    m_font.uploadSystemFont(gpu());
    m_pad.initialize();

    switch (assetsMount(m_cdrom, m_iso, gpu())) {
        case MOUNT_OK:
            break;
        case MOUNT_NO_ISO:
            m_error = "no ISO9660 filesystem";
            pushScene(&g_playScene);
            return;
        case MOUNT_NO_FILE:
            m_error = "POOM.DAT not on the disc";
            pushScene(&g_playScene);
            return;
        case MOUNT_READ_FAILED:
            m_error = "POOM.DAT read failed";
            pushScene(&g_playScene);
            return;
        default:
            m_error = "POOM.DAT is not an archive";
            pushScene(&g_playScene);
            return;
    }

    if (!assetsLoad("ACTR", g_actorArena, sizeof(g_actorArena))) {
        m_error = "ACTR";
        pushScene(&g_playScene);
        return;
    }
    g_assets.actors = (const ActorDef *)g_actorArena;
    g_assets.numActors = assetsSize("ACTR") / sizeof(ActorDef);

    alignas(4) static uint8_t stateArena[16 * 1024];
    if (!assetsLoad("STAT", stateArena, sizeof(stateArena))) {
        m_error = "STAT";
        pushScene(&g_playScene);
        return;
    }
    g_assets.states = (const StateDef *)stateArena;
    g_assets.numStates = assetsSize("STAT") / sizeof(StateDef);

    if (!assetsLoad("FRAM", g_frameArena, sizeof(g_frameArena))) {
        m_error = "FRAM";
        pushScene(&g_playScene);
        return;
    }
    g_assets.frames = (const FrameDef *)g_frameArena;

    alignas(4) static uint8_t frameMap[2048];
    if (!assetsLoad("FRMX", frameMap, sizeof(frameMap))) {
        m_error = "FRMX";
        pushScene(&g_playScene);
        return;
    }
    g_assets.frameMap = (const uint16_t *)frameMap;
    g_assets.numFrameMap = assetsSize("FRMX") / 2;

    assetsUploadCluts(gpu(), VRAM_CLUT_X, VRAM_CLUT_Y, NUM_CLUTS);

    if (!loadEpisodeTextures()) {
        m_error = "textures";
        pushScene(&g_playScene);
        return;
    }
    if (!loadSprites()) {
        m_error = "sprites";
        pushScene(&g_playScene);
        return;
    }

    if (!loadFont()) {
        m_error = "font";
        pushScene(&g_playScene);
        return;
    }

    if (!loadLevel(kLevelName)) {
        m_error = "level";
        pushScene(&g_playScene);
        return;
    }

    renderInit(gpu());
    renderSetLevel(&g_level, &g_assets);
    gameInit(&g_level, &g_assets);
    m_ready = true;
    pushScene(&g_playScene);
}

namespace {
static constexpr uint32_t TICK_US = 1000000 / 30;
uint32_t g_lastTickTime;
bool g_haveTickTime;
}

void PlayScene::start(StartReason reason) {
    g_haveTickTime = false;
}

void PlayScene::frame() {
    auto &gpu = g_app.gpu();

    if (!g_app.m_ready) {
        gpu.clear({{.r = 32, .g = 0, .b = 0}});
        g_app.m_font.print(gpu, "poom: load failed", {{.x = 16, .y = 100}},
                           {{.r = 255, .g = 255, .b = 255}});
        if (g_app.m_error) {
            g_app.m_font.print(gpu, g_app.m_error, {{.x = 16, .y = 116}},
                               {{.r = 255, .g = 200, .b = 200}});
        }
        return;
    }

    uint32_t now = gpu.now();
    if (!g_haveTickTime) {
        g_lastTickTime = now;
        g_haveTickTime = true;
    }
    uint32_t elapsed = now - g_lastTickTime;
    int ticks = elapsed / TICK_US;
    if (ticks > 3) {
        ticks = 3;
        g_lastTickTime = now;
    } else {
        g_lastTickTime += ticks * TICK_US;
    }
    for (int i = 0; i < ticks; i++) gameUpdate(g_app.m_pad);

    if (gameExitRequested() && g_levelIndex + 1 < kNumLevels) {
        if (advanceLevel()) g_haveTickTime = false;
    } else if (gameRestartRequested()) {
        if (restartLevel()) g_haveTickTime = false;
    }

    gameCollectSprites();

    gpu.clear({{.r = 0, .g = 0, .b = 0}});
    renderScene(gpu, gameCamera());
    gameDrawHud(gpu);
}

int main() { return g_app.run(); }
