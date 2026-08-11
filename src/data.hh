#pragma once

#include <stddef.h>
#include <stdint.h>

#include "fixed.hh"

static constexpr uint16_t NO_INDEX = 0xFFFF;

#pragma pack(push, 1)

struct LumpEntry {
    char name[12];
    uint32_t offset;
    uint32_t size;
};

struct ArchiveHeader {
    char magic[4];
    uint32_t version;
    uint32_t numLumps;
    uint32_t pad;
    LumpEntry lumps[1];
};


struct FrameDef {
    uint8_t page;
    uint8_t u, v;
    uint8_t w, h;
    uint8_t variant;
    int16_t xoffset;
    int16_t yoffset;
};


enum ActorFlags {
    AF_SOLID = 0x0001, AF_SHOOTABLE = 0x0002, AF_MISSILE = 0x0004,
    AF_MONSTER = 0x0008, AF_NOGRAVITY = 0x0010, AF_FLOAT = 0x0020,
    AF_DROPOFF = 0x0040, AF_DONTFALL = 0x0080, AF_RANDOMIZE = 0x0100,
    AF_COUNTKILL = 0x0200, AF_NOSECTORDMG = 0x0400, AF_NOBLOOD = 0x0800,
};

enum StateLabel {
    SL_SPAWN = 0, SL_IDLE, SL_SEE, SL_MELEE, SL_MISSILE, SL_DEATH,
    SL_XDEATH, SL_READY, SL_HOLD, SL_FIRE, SL_PICKUP, SL_PAIN,
    SL_COUNT
};

enum ActorKind {
    AK_INVENTORY = 0, AK_AMMO, AK_WEAPON, AK_HEALTH, AK_ARMOR, AK_MONSTER,
};

struct ActorDef {
    uint16_t id;
    uint16_t kind;
    fixed_t radius;
    fixed_t height;
    uint16_t flags;
    uint16_t firstState;
    uint16_t numStates;
    uint16_t ammotype;
    uint16_t trailtype;
    uint16_t health;
    uint16_t armor;
    uint16_t amount;
    uint16_t maxamount;
    uint16_t ammouse;
    uint16_t speed;
    uint16_t damage;
    uint16_t mass;
    uint16_t ammogive;
    uint16_t meleerange;
    uint16_t maxtargetrange;
    uint8_t slot;
    uint8_t icon;
    uint8_t hudcolor;
    uint8_t pad0;
    uint8_t pickupsound;
    uint8_t attacksound;
    uint8_t deathsound;
    uint8_t numStartItems;
    fixed_t drag;
    struct { uint16_t actor; uint16_t amount; } startItems[8];
    uint16_t labels[16];
};

enum StateKind { SK_STOP = 0, SK_GOTO = 1, SK_NORMAL = 2 };

struct StateDef {
    uint8_t kind;
    int8_t ticks;
    uint8_t gotoLabel;
    uint8_t bright;
    uint8_t flipbits;
    uint8_t numSides;
    uint16_t sides[8];
    uint8_t func;
    uint8_t pad;
    int32_t args[5];
};


struct TexDef {
    uint8_t page;
    uint8_t u, v;
    uint8_t w, h;
    uint8_t pad;
};


struct SectorDef {
    fixed_t floor;
    fixed_t ceil;
    uint16_t floortex;
    uint16_t ceiltex;
    uint8_t lightlevel;
    uint8_t special;
    uint16_t pad;
};

struct SideDef {
    uint16_t sector;
    uint16_t toptex;
    uint16_t midtex;
    uint16_t bottomtex;
};

struct VertexDef {
    fixed_t x, y;
};

enum LineFlags {
    LF_TWOSIDED = 0x01, LF_SPECIAL = 0x02, LF_DONTPEGTOP = 0x04,
    LF_PLAYERUSE = 0x08, LF_PLAYERCROSS = 0x10, LF_REPEAT = 0x20,
    LF_BLOCKING = 0x40,
};

struct LineDef {
    uint16_t front;
    uint16_t back;
    uint16_t flags;
    uint16_t special;
};

struct SegDef {
    uint16_t v;
    uint16_t line;
    uint16_t partner;
    uint8_t side;
    uint8_t major;
    fixed_t dirx, diry, ddist, len, nx, ny, ndist;
};

struct SubSectorDef {
    uint16_t firstSeg;
    uint16_t numSegs;
    uint16_t sector;
    uint16_t firstFlat;
    uint16_t numFlats;
    uint16_t pad;
};

struct FlatPoly {
    uint16_t firstVert;
    uint16_t numVerts;
};

struct NodeDef {
    fixed_t nx, ny, d;
    struct { uint16_t index; uint16_t isLeaf; } child[2];
    int16_t bbox[2][4];
};

struct ThingDef {
    uint16_t actor;
    uint16_t pad;
    fixed_t x, y;
    uint8_t angle;
    uint8_t skills;
    uint16_t special;
};

enum SpecialKind { SP_MOVE = 0, SP_LIGHT = 1, SP_EXIT = 2 };

struct SpecialHeader {
    uint8_t kind;
};

struct MoveTarget {
    uint16_t sector;
    uint16_t pad;
    fixed_t target;
};

struct MoveSpecial {
    uint8_t kind;
    uint8_t what;
    uint16_t triggerDelay;
    uint16_t delay;
    uint16_t numTargets;
    fixed_t speed;
    uint16_t lock;
    uint8_t startClosed;
    uint8_t pad;
    MoveTarget targets[1];
};

struct LightSpecial {
    uint8_t kind;
    uint8_t level;
    uint16_t numTargets;
    uint16_t targets[1];
};

struct ExitSpecial {
    uint8_t kind;
    uint8_t pad;
    uint16_t delay;
};


enum LevelBlock {
    LB_SECTORS = 0, LB_SIDES, LB_VERTS, LB_LINES, LB_SEGS, LB_SUBS,
    LB_FLATS, LB_FLATVERTS, LB_PVS, LB_NODES, LB_THINGS, LB_ONOFF,
    LB_TRANSPARENT, LB_SPECIALOFS, LB_SPECIALS, LB_COUNT
};

struct LevelHeader {
    char magic[4];
    uint32_t pvsStride;
    struct { uint32_t offset; uint32_t count; } blocks[LB_COUNT];
};

#pragma pack(pop)

static_assert(sizeof(FrameDef) == 10, "FrameDef layout changed");
static_assert(sizeof(ActorDef) == 120, "ActorDef layout changed");
static_assert(sizeof(StateDef) == 44, "StateDef layout changed");
static_assert(sizeof(TexDef) == 6, "TexDef layout changed");
static_assert(sizeof(SectorDef) == 16, "SectorDef layout changed");
static_assert(sizeof(SideDef) == 8, "SideDef layout changed");
static_assert(sizeof(VertexDef) == 8, "VertexDef layout changed");
static_assert(sizeof(LineDef) == 8, "LineDef layout changed");
static_assert(sizeof(SegDef) == 36, "SegDef layout changed");
static_assert(sizeof(SubSectorDef) == 12, "SubSectorDef layout changed");
static_assert(sizeof(FlatPoly) == 4, "FlatPoly layout changed");
static_assert(sizeof(NodeDef) == 36, "NodeDef layout changed");
static_assert(sizeof(ThingDef) == 16, "ThingDef layout changed");
static_assert(sizeof(MoveTarget) == 8, "MoveTarget layout changed");
static_assert(offsetof(MoveSpecial, targets) == 16, "MoveSpecial header is 16 bytes");
static_assert(offsetof(LightSpecial, targets) == 4, "LightSpecial header is 4 bytes");
static_assert(sizeof(LumpEntry) == 20, "LumpEntry layout changed");
