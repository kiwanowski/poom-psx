
#include "poom.hh"

#include "psyqo/simplepad.hh"

Level g_level;
Assets g_assets;
int g_ambientLight;

void pickupThing(Thing *item, Thing *who);
void triggerLine(uint16_t lineIndex, Thing *who);
void runSpecial(uint16_t index, Thing *who);
void gameRequestExit();

namespace {

Thing g_things[MAX_THINGS];
int g_numThings;
Thing *g_player;
uint8_t g_intersectId;

int g_kills, g_monsters, g_secrets;
int g_drag;
bool g_prevUse, g_prevSwitch, g_prevDeadFire;
bool g_dead;
int g_deathTicks;
fixed_t g_deathHeight;
bool g_restartRequested;
uint32_t g_tick;
uint32_t g_rndState = 0x12345678;

inline uint32_t rnd() {
    g_rndState = g_rndState * 1664525u + 1013904223u;
    return g_rndState >> 16;
}
inline fixed_t rndFixed(fixed_t max) { return (fixed_t)((uint64_t)rnd() * max >> 16); }


struct Player {
    Thing *thing;
    int health, armor;
    int weaponSlot;
    int weaponState[6];
    int weaponTicks[6];
    const ActorDef *weapon[6];
    int hitFlash;
    fixed_t bobX, bobY;
};

Player g_ply;


static constexpr int MAX_SUBS = 512;
static constexpr int THINGS_PER_SUB = 12;
uint8_t g_subThings[MAX_SUBS][THINGS_PER_SUB];
uint8_t g_subCount[MAX_SUBS];

void unregisterThing(Thing *t) {
    for (int i = 0; i < t->numSubs; i++) {
        uint16_t s = t->subs[i];
        if (s >= MAX_SUBS) continue;
        for (int j = 0; j < g_subCount[s]; j++) {
            if (g_subThings[s][j] == (t - g_things)) {
                g_subThings[s][j] = g_subThings[s][g_subCount[s] - 1];
                g_subCount[s]--;
                break;
            }
        }
    }
    t->numSubs = 0;
}

void registerNode(const NodeDef &node, Thing *t, fixed_t radius) {
    fixed_t dist = fmul(node.nx, t->x) + fmul(node.ny, t->y);
    for (int side = 0; side < 2; side++) {
        bool inside;
        if (side == 0) {
            inside = dist <= node.d + radius;
        } else {
            inside = dist > node.d - radius;
        }
        if (!inside) continue;
        if (node.child[side].isLeaf) {
            uint16_t s = node.child[side].index;
            if (s < MAX_SUBS && t->numSubs < MAX_SUBS_PER_THING &&
                g_subCount[s] < THINGS_PER_SUB) {
                t->subs[t->numSubs++] = s;
                g_subThings[s][g_subCount[s]++] = (uint8_t)(t - g_things);
            }
        } else {
            registerNode(g_level.nodes[node.child[side].index], t, radius);
        }
    }
}

void registerThing(Thing *t) {
    t->numSubs = 0;
    if (g_level.numNodes == 0) return;
    registerNode(g_level.nodes[g_level.numNodes - 1], t, t->actor->radius / 2);
}


void actionFunction(Thing *t, const StateDef *st);

void jumpTo(Thing *t, int label, int fallback = -1) {
    const ActorDef *a = t->actor;
    uint16_t s = NO_INDEX;
    if (label >= 0 && label < SL_COUNT) s = a->labels[label];
    if (s == NO_INDEX && fallback >= 0 && fallback < SL_COUNT) s = a->labels[fallback];
    if (s == NO_INDEX) return;
    t->stateIndex = (int16_t)s;
    t->ticks = -2;
}

void removeThing(Thing *t) {
    unregisterThing(t);
    t->active = 0;
}

bool tickVM(Thing *t) {
    const ActorDef *a = t->actor;
    int guard = 0;
    while (t->ticks != -1) {
        if (++guard > 64) return true;
        if (t->ticks > 0) {
            t->ticks += t->delay - 1;
            t->delay = 0;
            return true;
        }
        if (t->ticks == 0) t->stateIndex++;
        for (;;) {
            if (t->stateIndex < 0 || t->stateIndex >= a->numStates) {
                removeThing(t);
                return false;
            }
            const StateDef *st = &g_assets.states[a->firstState + t->stateIndex];
            if (st->kind == SK_STOP) {
                removeThing(t);
                return false;
            }
            if (st->kind == SK_GOTO) {
                int prev = t->stateIndex;
                jumpTo(t, st->gotoLabel);
                if (t->stateIndex == prev) {
                    removeThing(t);
                    return false;
                }
                continue;
            }
            t->state = st;
            t->ticks = st->ticks;
            if (st->func) actionFunction(t, st);
            break;
        }
    }
    return true;
}


struct Hit {
    fixed_t ti;
    fixed_t t;
    fixed_t dist;
    fixed_t nx, ny;
    const SegDef *seg;
    Thing *thing;
};

typedef bool (*HitFn)(const Hit &hit, void *ctx);

void intersectSubSector(uint16_t subIndex, fixed_t px, fixed_t py,
                        fixed_t dx, fixed_t dy, fixed_t tmin, fixed_t tmax,
                        fixed_t radius, HitFn cb, void *ctx, bool skipThings) {
    if (subIndex >= g_level.numSubs) return;
    const SubSectorDef &ss = g_level.subs[subIndex];
    const fixed_t origTmax = tmax;
    int otherSector = -1;

    if (!skipThings && subIndex < MAX_SUBS) {
        for (int i = 0; i < g_subCount[subIndex]; i++) {
            Thing *o = &g_things[g_subThings[subIndex][i]];
            if (!o->active || o->dead || o->intersectId == g_intersectId) continue;
            if (o->actor->flags & AF_MISSILE) continue;
            o->intersectId = g_intersectId;
            fixed_t mx = (px - o->x) >> 8;
            fixed_t my = (py - o->y) >> 8;
            fixed_t r = (o->actor->radius + radius) >> 8;
            fixed_t b = fmul(mx, dx) + fmul(my, dy);
            fixed_t c = fmul(mx, mx) + fmul(my, my) - fmul(r, r);
            if (c > 0 && b > 0) continue;
            fixed_t discr = fmul(b, b) - c;
            if (discr < 0) continue;
            fixed_t tt = (-b - fsqrt(discr)) << 8;
            if (tt < tmin) tt = tmin;
            if (tt < tmin || tt >= tmax) continue;
            Hit hit;
            hit.ti = tt;
            hit.t = radius ? FRACUNIT - fdiv(tt, radius) : 0;
            hit.dist = 0;
            hit.seg = nullptr;
            hit.thing = o;
            hit.nx = hit.ny = 0;
            if (cb(hit, ctx)) return;
        }
    }

    for (int i = 0; i < ss.numSegs; i++) {
        const SegDef &s = g_level.segs[ss.firstSeg + i];
        fixed_t denom = fmul(s.nx, dx) + fmul(s.ny, dy);
        if (denom <= 0) continue;
        fixed_t distA = s.ndist - (fmul(s.nx, px) + fmul(s.ny, py));
        fixed_t t = fdiv(distA, denom);
        fixed_t hx = px + fmul(t, dx);
        fixed_t hy = py + fmul(t, dy);
        fixed_t d = fmul(s.dirx, hx) + fmul(s.diry, hy) - s.ddist;
        if (d < -radius || d >= s.len + radius) continue;
        fixed_t ex = px + fmul(origTmax, dx);
        fixed_t ey = py + fmul(origTmax, dy);
        fixed_t distB = s.ndist - (fmul(s.nx, ex) + fmul(s.ny, ey));
        bool inSeg = d >= 0 && d < s.len;
        if (inSeg && t < tmax) {
            tmax = t;
            otherSector = (s.partner == NO_INDEX) ? -1 : s.partner;
        }
        if (s.line != NO_INDEX && (distA < radius || distB < radius)) {
            Hit hit;
            hit.ti = t;
            fixed_t den = distA - distB;
            hit.t = den ? clampi(fdiv(distA, den), 0, FRACUNIT) : 0;
            hit.dist = (distA < radius && inSeg) ? (radius - distA) : 0;
            hit.nx = s.nx;
            hit.ny = s.ny;
            hit.seg = &s;
            hit.thing = nullptr;
            if (cb(hit, ctx)) return;
        }
    }

    if (tmin <= tmax && tmax < origTmax && otherSector >= 0) {
        intersectSubSector((uint16_t)otherSector, px, py, dx, dy, tmax,
                           origTmax, radius, cb, ctx, skipThings);
    }
}

bool blockedByLine(const SegDef *seg, fixed_t h, fixed_t height,
                   fixed_t clearance, bool isMissile, bool isDropoff) {
    const LineDef &ld = g_level.lines[seg->line];
    uint16_t other = seg->side ? ld.back : ld.front;
    if (other == NO_INDEX) return true;
    if (!isMissile && (ld.flags & LF_BLOCKING)) return true;
    const SectorDef &os = g_level.sectors[g_level.mutSides[other].sector];
    if (h + height > os.ceil) return true;
    if (h + clearance < os.floor) return true;
    if (!isDropoff && h - os.floor > clearance) return true;
    return false;
}

bool blockedByThing(const Thing *o, fixed_t h, fixed_t radius) {
    if (!(o->actor->flags & AF_SOLID)) return false;
    return h >= o->z - radius && h < o->z + o->actor->height + radius;
}


struct MoveCtx {
    Thing *self;
    fixed_t dirx, diry;
    fixed_t h;
    fixed_t stairH;
    bool isMissile;
    bool isPlayer;
    bool stop;
};

void applyForces(Thing *t, fixed_t x, fixed_t y, fixed_t mag) {
    fixed_t mass = intToFixed(t->actor->mass ? t->actor->mass * 2 : 200);
    mag <<= 6;
    t->forcex += fdiv(fmul(mag, x), mass);
    t->forcey += fdiv(fmul(mag, y), mass);
}

void damageThing(Thing *t, int dmg, fixed_t dirx, fixed_t diry, Thing *instigator);

bool moveHit(const Hit &hit, void *ctxp) {
    MoveCtx *ctx = (MoveCtx *)ctxp;
    Thing *self = ctx->self;
    bool fix = false;
    if (hit.seg) {
        fix = blockedByLine(hit.seg, ctx->h, self->actor->height, ctx->stairH,
                            ctx->isMissile, (self->actor->flags & AF_DROPOFF) != 0);
        const LineDef &ld = g_level.lines[hit.seg->line];
        if (ctx->isPlayer && (ld.flags & LF_SPECIAL) && (ld.flags & LF_PLAYERCROSS)) {
            triggerLine(hit.seg->line, self);
        }
    } else if (hit.thing) {
        if (ctx->isPlayer && hit.thing->actor->kind <= AK_ARMOR) {
            pickupThing(hit.thing, self);
        } else if (self->owner != hit.thing) {
            fix = blockedByThing(hit.thing, ctx->h, self->actor->radius);
        }
    }
    if (!fix) return false;

    if (ctx->isMissile) {
        self->x += fmul(ctx->dirx, hit.ti);
        self->y += fmul(ctx->diry, hit.ti);
        self->velx = self->vely = 0;
        jumpTo(self, SL_DEATH);
        if (hit.thing && (hit.thing->actor->flags & AF_SHOOTABLE)) {
            damageThing(hit.thing, (1 + (int)(rnd() % 7)) * self->actor->damage,
                        ctx->dirx, ctx->diry, self->owner);
        }
        ctx->stop = true;
        return true;
    }

    fixed_t nx = hit.nx, ny = hit.ny;
    if (!hit.seg && hit.thing) {
        v2Normal(hit.thing->x - self->x, hit.thing->y - self->y, &nx, &ny);
    }
    fixed_t fixAmount = -fmul(hit.t, fmul(nx, self->velx) + fmul(ny, self->vely));
    if (fixAmount < 0) {
        self->velx += fmul(nx, fixAmount);
        self->vely += fmul(ny, fixAmount);
    }
    if (hit.dist) {
        self->x -= fmul(nx, hit.dist);
        self->y -= fmul(ny, hit.dist);
    }
    if (hit.thing && self->actor->damage && (hit.thing->actor->flags & AF_SHOOTABLE)) {
        damageThing(hit.thing, (1 + (int)(rnd() % 7)) * self->actor->damage,
                    nx, ny, self);
    }
    return false;
}

void thingPhysics(Thing *t) {
    const ActorDef *a = t->actor;
    bool isMissile = (a->flags & AF_MISSILE) != 0;
    bool isPlayer = t->isPlayer != 0;

    t->velx += t->forcex;
    t->vely += t->forcey;
    t->forcex = t->forcey = 0;

    if (!t->dead && (a->flags & AF_FLOAT)) {
        t->velz += rndFixed(FRACUNIT + FRACUNIT / 2) - (FRACUNIT * 9 / 10);
        if (t->target) {
            fixed_t d = t->target->z - t->z;
            d = clampi(d, -intToFixed(512), intToFixed(512));
            t->velz += d >> 8;
        }
        t->velz = fmul(t->velz, 59426);
    }
    if (!(a->flags & AF_NOGRAVITY) || (t->dead && !(a->flags & AF_DONTFALL))) {
        t->velz -= FRACUNIT;
    }

    fixed_t friction = isMissile ? 65320 : 59392;
    if (isPlayer && g_drag) {
        friction = fmul(friction, FRACUNIT - g_drag);
    }
    t->velx = fmul(t->velx, friction);
    t->vely = fmul(t->vely, friction);

    fixed_t dirx, diry;
    fixed_t moveLen = v2Normal(t->velx, t->vely, &dirx, &diry);

    if (moveLen > FRACUNIT / 16) {
        unregisterThing(t);
        MoveCtx ctx;
        ctx.self = t;
        ctx.dirx = dirx;
        ctx.diry = diry;
        ctx.h = t->z;
        ctx.stairH = isMissile ? 0 : intToFixed(24);
        ctx.isMissile = isMissile;
        ctx.isPlayer = isPlayer;
        ctx.stop = false;
        g_intersectId++;
        intersectSubSector(t->ssector, t->x, t->y, dirx, diry, 0, moveLen,
                           a->radius, moveHit, &ctx, false);
        if (!t->active) return;
        if (!ctx.stop) {
            t->x += t->velx;
            t->y += t->vely;
        }
        t->ssector = findSubSector(g_level, t->x, t->y);
        t->sector = g_level.subs[t->ssector].sector;
        registerThing(t);
    } else {
        t->velx = t->vely = 0;
    }

    if (!isMissile) {
        const SectorDef &sec = g_level.sectors[t->sector];
        fixed_t h = t->z + t->velz;
        if (h < sec.floor) {
            if (!(a->flags & (AF_FLOAT | AF_NOSECTORDMG)) && (a->flags & AF_SHOOTABLE)) {
                int32_t vz = fixedToInt(-t->velz);
                int dmg = ((vz * vz * 11) >> 7) / 2 - 15;
                if (dmg > 0) damageThing(t, dmg, 0, 0, nullptr);
            }
            t->velz = 0;
            h = sec.floor;
        }
        if (h + a->height > sec.ceil) {
            t->velz = 0;
            h = sec.ceil - a->height;
        }
        t->z = h;
    }
}


void damageThing(Thing *t, int dmg, fixed_t dirx, fixed_t diry, Thing *instigator) {
    if (t->dead || !(t->actor->flags & AF_SHOOTABLE)) return;
    if (instigator && instigator->actor->id == t->actor->id) return;
    if (t == g_player || instigator == g_player || (rnd() & 3) == 0) {
        t->target = instigator;
    }
    int hp = dmg;
    if (t->armor > 0) {
        hp = (dmg * 3) / 10;
        t->armor -= dmg;
        if (t->armor < 0) t->armor = 0;
    }
    t->health -= hp;
    if (t->health <= 0) {
        t->health = 0;
        if (t->actor->flags & AF_COUNTKILL) g_kills++;
        t->dead = 1;
        t->target = nullptr;
        if (t->special != NO_INDEX) {
            runSpecial(t->special, t);
            t->special = NO_INDEX;
        }
        jumpTo(t, SL_DEATH);
    }
    if (t->isPlayer) {
        g_ply.health = t->health;
        g_ply.armor = t->armor;
        int f = hp;
        if (f > 15) f = 15;
        if (f > g_ply.hitFlash) g_ply.hitFlash = f;
    }
    if (dirx || diry) applyForces(t, dirx, diry, intToFixed(hp));
}


Thing *spawnThing(const ActorDef *a, fixed_t x, fixed_t y, fixed_t z,
                  angle_t angle) {
    if (!a) return nullptr;
    Thing *t = nullptr;
    for (int i = 0; i < MAX_THINGS; i++) {
        if (!g_things[i].active) {
            t = &g_things[i];
            break;
        }
    }
    if (!t) return nullptr;
    for (unsigned i = 0; i < sizeof(Thing); i++) ((uint8_t *)t)[i] = 0;
    t->actor = a;
    t->x = x;
    t->y = y;
    t->angle = angle;
    t->active = 1;
    t->special = NO_INDEX;
    t->ssector = findSubSector(g_level, x, y);
    t->sector = g_level.subs[t->ssector].sector;
    t->z = (z == INT32_MIN) ? g_level.sectors[t->sector].floor : z;
    t->health = a->health;
    t->armor = a->armor;
    t->stateIndex = (a->labels[SL_SPAWN] == NO_INDEX) ? 0 : a->labels[SL_SPAWN];
    t->ticks = -2;
    t->delay = (a->flags & AF_MONSTER) ? (int16_t)(rnd() % 30) : 0;
    if (a->flags & AF_RANDOMIZE) t->delay = (int16_t)(rnd() % 4);
    t->state = a->numStates ? &g_assets.states[a->firstState + t->stateIndex]
                            : nullptr;
    registerThing(t);
    if (g_numThings <= (t - g_things)) g_numThings = (t - g_things) + 1;
    return t;
}


struct ScanCtx {
    Thing *owner;
    fixed_t h;
    fixed_t dirx, diry;
    int dmg;
    const ActorDef *puff;
    bool done;
};

bool scanHit(const Hit &hit, void *ctxp) {
    ScanCtx *ctx = (ScanCtx *)ctxp;
    bool ok = false;
    if (hit.seg) {
        ok = blockedByLine(hit.seg, ctx->h, 0, 0, true, true);
    } else if (hit.thing && hit.thing != ctx->owner) {
        ok = blockedByThing(hit.thing, ctx->h, 0);
    }
    if (!ok) return false;
    fixed_t hx = ctx->owner->x + fmul(ctx->dirx, hit.ti);
    fixed_t hy = ctx->owner->y + fmul(ctx->diry, hit.ti);
    Thing *puff = spawnThing(ctx->puff, hx, hy, ctx->h, ctx->owner->angle);
    if (hit.thing && (hit.thing->actor->flags & AF_SHOOTABLE)) {
        if (puff && !(hit.thing->actor->flags & AF_NOBLOOD)) {
            jumpTo(puff, SL_PAIN);
        }
        damageThing(hit.thing, ctx->dmg, ctx->dirx, ctx->diry, ctx->owner);
    }
    ctx->done = true;
    return true;
}

void hitscanAttack(Thing *owner, angle_t angle, fixed_t range, int dmg,
                   const ActorDef *puff) {
    ScanCtx ctx;
    ctx.owner = owner;
    ctx.h = owner->z + intToFixed(32);
    headingVector(angle, &ctx.dirx, &ctx.diry);
    ctx.dmg = dmg;
    ctx.puff = puff;
    ctx.done = false;
    g_intersectId++;
    intersectSubSector(owner->ssector, owner->x, owner->y, ctx.dirx, ctx.diry,
                       owner->actor->radius / 2, range, 0, scanHit, &ctx, false);
}


struct SightCtx {
    fixed_t h;
    bool blocked;
};

bool sightHit(const Hit &hit, void *ctxp) {
    SightCtx *ctx = (SightCtx *)ctxp;
    if (hit.seg && blockedByLine(hit.seg, ctx->h + intToFixed(24), 0, 0, true, true)) {
        ctx->blocked = true;
        return true;
    }
    return false;
}

fixed_t lineOfSight(Thing *t, Thing *other, fixed_t maxdist,
                    fixed_t *nx, fixed_t *ny) {
    fixed_t d = v2Normal(other->x - t->x, other->y - t->y, nx, ny);
    if (!g_level.inPVS(t->ssector, other->ssector)) return -1;
    d -= t->actor->radius;
    if (d < 0) d = 0;
    if (d >= maxdist) return -1;
    SightCtx ctx;
    ctx.h = (t->actor->flags & AF_FLOAT) ? other->z : t->z;
    ctx.blocked = false;
    g_intersectId++;
    intersectSubSector(t->ssector, t->x, t->y, *nx, *ny, 0, d, 0, sightHit,
                       &ctx, true);
    return ctx.blocked ? -1 : d;
}


int weaponSlotOf(const ActorDef *a) { return a ? a->slot : 0; }

void giveWeapon(const ActorDef *w, bool autoSwitch);

void actionFunction(Thing *t, const StateDef *st) {
    const ActorDef *a = t->actor;
    switch (st->func) {
        case 1: {
            int bullets = st->args[2];
            int dmg = st->args[3];
            const ActorDef *puff = g_assets.actorByIndex((uint16_t)st->args[4]);
            fixed_t xspread = st->args[0];
            for (int i = 0; i < bullets; i++) {
                int32_t spread = fixedToInt(xspread);
                int32_t off = spread ? ((int32_t)(rnd() % (2 * spread + 1)) - spread) : 0;
                angle_t ang = t->angle + (angle_t)((off * 65536) / 360);
                hitscanAttack(t, ang, intToFixed(1024), dmg, puff);
            }
            break;
        }
        case 2:
            break;
        case 3: {
            const ActorDef *proj = g_assets.actorByIndex((uint16_t)st->args[0]);
            if (!proj) break;
            fixed_t ca, sa;
            headingVector(t->angle, &ca, &sa);
            fixed_t r = a->radius / 2;
            Thing *m = spawnThing(proj, t->x + fmul(r, ca), t->y + fmul(r, sa),
                                  t->z + intToFixed(32), t->angle);
            if (m) {
                m->owner = t;
                applyForces(m, ca, sa, intToFixed(proj->speed));
            }
            break;
        }
        case 4:
            break;
        case 5: {
            int dmg = st->args[0];
            fixed_t maxrange = intToFixed(st->args[1]);
            for (int i = 0; i < g_numThings; i++) {
                Thing *o = &g_things[i];
                if (!o->active || o == t) continue;
                if (!(o->actor->flags & AF_SHOOTABLE)) continue;
                fixed_t nx, ny;
                fixed_t d = lineOfSight(t, o, maxrange, &nx, &ny);
                if (d < 0) continue;
                int scaled = dmg - (int)(((int64_t)dmg * d) / maxrange);
                damageThing(o, scaled, nx, ny, nullptr);
            }
            break;
        }
        case 6: {
            if (!t->target) break;
            angle_t want = fatan2(t->target->x - t->x, t->y - t->target->y);
            int32_t speed = st->args[0];
            int16_t delta = (int16_t)(want - t->angle);
            t->angle += (angle_t)((delta * speed) / 255);
            break;
        }
        case 7: {
            Thing *other = t->target ? t->target : g_player;
            if (!other || other->dead) other = g_player;
            if (!other || other->dead) {
                t->target = nullptr;
                break;
            }
            fixed_t nx, ny;
            if (lineOfSight(t, other, intToFixed(1024), &nx, &ny) >= 0) {
                t->target = other;
                jumpTo(t, SL_SEE);
            }
            break;
        }
        case 8: {
            Thing *other = t->target;
            if (other && !other->dead) {
                fixed_t range = a->meleerange ? intToFixed(a->meleerange) : intToFixed(64);
                fixed_t maxrange = a->maxtargetrange ? intToFixed(a->maxtargetrange)
                                                     : intToFixed(1024);
                fixed_t nx, ny;
                fixed_t d = lineOfSight(t, other, maxrange, &nx, &ny);
                if (d >= 0 && (rnd() % 10) < 4) {
                    if (d < range) {
                        jumpTo(t, SL_MELEE, SL_MISSILE);
                    } else {
                        jumpTo(t, SL_MISSILE);
                    }
                    return;
                }
                if (d >= 0) {
                    fixed_t hx = nx >> 1, hy = ny >> 1;
                    int dir = (rnd() & 1) ? 1 : -1;
                    fixed_t mx = hy * dir + hx;
                    fixed_t my = hx * -dir + hy;
                    angle_t want = fatan2(mx, -my);
                    int16_t delta = (int16_t)(want - t->angle);
                    t->angle += (angle_t)(delta / 2);
                    applyForces(t, mx, my, intToFixed(a->speed));
                    return;
                }
            }
            t->target = nullptr;
            jumpTo(t, SL_SPAWN);
            break;
        }
        case 9:
            g_ambientLight = clampi(st->args[0], 0, 255);
            break;
        case 10: {
            const ActorDef *puff = g_assets.actorByIndex((uint16_t)st->args[1]);
            fixed_t range = a->meleerange ? intToFixed(a->meleerange) : intToFixed(64);
            hitscanAttack(t, t->angle, range, st->args[0], puff);
            break;
        }
        case 11:
            if (t->target) {
                fixed_t dx, dy;
                headingVector(t->angle, &dx, &dy);
                applyForces(t, dx, dy, intToFixed(st->args[0]));
            }
            break;
        default:
            break;
    }
}


static constexpr int MAX_INVENTORY = 64;
int g_inventory[MAX_INVENTORY];

int inventoryOf(uint16_t actorIndex) {
    if (actorIndex == NO_INDEX || actorIndex >= MAX_INVENTORY) return 0;
    return g_inventory[actorIndex];
}

void giveInventory(uint16_t actorIndex, int amount, int max) {
    if (actorIndex == NO_INDEX || actorIndex >= MAX_INVENTORY) return;
    int v = g_inventory[actorIndex] + amount;
    if (max > 0 && v > max) v = max;
    g_inventory[actorIndex] = v;
}

}

void pickupThing(Thing *item, Thing *who) {
    const ActorDef *a = item->actor;
    bool taken = false;
    switch (a->kind) {
        case AK_INVENTORY: {
            uint16_t idx = (uint16_t)(a - g_assets.actors);
            if (inventoryOf(idx) < a->maxamount) {
                giveInventory(idx, a->amount ? a->amount : 1, a->maxamount);
                taken = true;
            }
            break;
        }
        case AK_AMMO: {
            uint16_t ref = (a->ammotype != NO_INDEX)
                               ? a->ammotype
                               : (uint16_t)(a - g_assets.actors);
            if (inventoryOf(ref) < a->maxamount) {
                giveInventory(ref, a->amount, a->maxamount);
                taken = true;
            }
            break;
        }
        case AK_WEAPON: {
            giveWeapon(a, true);
            if (a->ammotype != NO_INDEX) {
                const ActorDef *at = g_assets.actorByIndex(a->ammotype);
                giveInventory(a->ammotype, a->ammogive, at ? at->maxamount : 0);
            }
            taken = true;
            break;
        }
        case AK_HEALTH:
            if (g_ply.health < a->maxamount) {
                g_ply.health = clampi(g_ply.health + a->amount, 0, a->maxamount);
                who->health = g_ply.health;
                taken = true;
            }
            break;
        case AK_ARMOR:
            if (g_ply.armor < a->maxamount) {
                g_ply.armor = clampi(g_ply.armor + a->amount, 0, a->maxamount);
                who->armor = g_ply.armor;
                taken = true;
            }
            break;
        default:
            break;
    }
    if (taken) removeThing(item);
}

namespace {

void giveWeapon(const ActorDef *w, bool autoSwitch) {
    int slot = weaponSlotOf(w);
    if (slot < 1 || slot > 5) return;
    if (g_ply.weapon[slot]) return;
    g_ply.weapon[slot] = w;
    g_ply.weaponState[slot] = (w->labels[SL_READY] == NO_INDEX)
                                  ? 0
                                  : w->labels[SL_READY];
    g_ply.weaponTicks[slot] = 0;
    if (autoSwitch) g_ply.weaponSlot = slot;
}

struct MovingSector {
    uint16_t sector;
    fixed_t target;
    fixed_t init;
    fixed_t speed;
    int16_t waitTicks;
    int16_t delay;
    uint8_t what;
    uint8_t phase;
    uint8_t active;
};

static constexpr int MAX_MOVING = 32;
MovingSector g_moving[MAX_MOVING];

void startMove(uint16_t sector, fixed_t target, fixed_t speed, int delay,
               int triggerDelay, int what) {
    MovingSector *m = nullptr;
    for (int i = 0; i < MAX_MOVING; i++) {
        if (g_moving[i].active && g_moving[i].sector == sector) {
            m = &g_moving[i];
            break;
        }
    }
    if (!m) {
        for (int i = 0; i < MAX_MOVING; i++) {
            if (!g_moving[i].active) {
                m = &g_moving[i];
                break;
            }
        }
    }
    if (!m) return;
    SectorDef &s = g_level.sectors[sector];
    m->sector = sector;
    m->target = target;
    m->init = (what == 1) ? s.ceil : s.floor;
    m->speed = speed;
    m->delay = (int16_t)delay;
    m->waitTicks = (int16_t)triggerDelay;
    m->what = (uint8_t)what;
    m->phase = 1;
    m->active = 1;
}

void updateMovingSectors() {
    for (int i = 0; i < MAX_MOVING; i++) {
        MovingSector &m = g_moving[i];
        if (!m.active) continue;
        if (m.waitTicks > 0) {
            m.waitTicks--;
            continue;
        }
        SectorDef &s = g_level.sectors[m.sector];
        fixed_t *h = (m.what == 1) ? &s.ceil : &s.floor;
        fixed_t goal = (m.phase == 3) ? m.init : m.target;
        fixed_t speed = (m.phase == 3) ? -m.speed : m.speed;
        if (m.phase == 1 || m.phase == 3) {
            fixed_t nh = *h + speed;
            if ((speed > 0 && nh >= goal) || (speed < 0 && nh <= goal)) {
                *h = goal;
                if (m.phase == 3) {
                    m.active = 0;
                } else if (m.delay > 0) {
                    m.phase = 2;
                    m.waitTicks = m.delay;
                } else {
                    m.active = 0;
                }
            } else {
                *h = nh;
            }
        } else if (m.phase == 2) {
            m.phase = 3;
        }
    }
}

}

void runSpecial(uint16_t index, Thing *who) {
    const SpecialHeader *sp = g_level.special(index);
    if (!sp) return;
    if (sp->kind == SP_MOVE) {
        const MoveSpecial *ms = (const MoveSpecial *)sp;
        for (int i = 0; i < ms->numTargets; i++) {
            if (ms->targets[i].sector >= g_level.numSectors) continue;
            startMove(ms->targets[i].sector, ms->targets[i].target, ms->speed,
                      ms->delay, ms->triggerDelay, ms->what);
        }
    } else if (sp->kind == SP_LIGHT) {
        const LightSpecial *ls = (const LightSpecial *)sp;
        for (int i = 0; i < ls->numTargets; i++) {
            if (ls->targets[i] < g_level.numSectors) {
                g_level.sectors[ls->targets[i]].lightlevel = ls->level;
            }
        }
    } else if (sp->kind == SP_EXIT) {
        gameRequestExit();
    }
}

namespace {
uint8_t g_lineUsed[2048];
}

void triggerLine(uint16_t lineIndex, Thing *who) {
    if (lineIndex >= g_level.numLines) return;
    const LineDef &ld = g_level.lines[lineIndex];
    if (ld.special == NO_INDEX) return;
    if (lineIndex < sizeof(g_lineUsed) && g_lineUsed[lineIndex]) return;

    const SpecialHeader *sp = g_level.special(ld.special);
    if (sp && sp->kind == SP_MOVE) {
        const MoveSpecial *ms = (const MoveSpecial *)sp;
        if (ms->lock != NO_INDEX && inventoryOf(ms->lock) <= 0) {
            return;
        }
    }
    if (lineIndex < sizeof(g_lineUsed) && !(ld.flags & LF_REPEAT)) {
        g_lineUsed[lineIndex] = 1;
    }
    if (ld.front != NO_INDEX) {
        SideDef &sd = g_level.mutSides[ld.front];
        sd.midtex = g_level.switchTexture(sd.midtex);
    }
    runSpecial(ld.special, who);
}


namespace {
bool g_exitRequested;
}

void gameRequestExit() { g_exitRequested = true; }
bool gameExitRequested() { return g_exitRequested; }

void gameInit(Level *level, Assets *assets) {
    g_numThings = 0;
    g_player = nullptr;
    g_kills = g_monsters = g_secrets = 0;
    g_ambientLight = 0;
    g_exitRequested = false;
    for (int i = 0; i < MAX_THINGS; i++) g_things[i].active = 0;
    for (int i = 0; i < MAX_SUBS; i++) g_subCount[i] = 0;
    for (unsigned i = 0; i < sizeof(g_lineUsed); i++) g_lineUsed[i] = 0;
    for (int i = 0; i < MAX_MOVING; i++) g_moving[i].active = 0;
    for (int i = 0; i < MAX_INVENTORY; i++) g_inventory[i] = 0;
    for (int i = 0; i < 6; i++) {
        g_ply.weapon[i] = nullptr;
        g_ply.weaponState[i] = -1;
    }
    g_ply.health = 100;
    g_ply.armor = 0;
    g_ply.weaponSlot = 0;
    g_ply.hitFlash = 0;
    g_ply.bobX = g_ply.bobY = 0;
    g_prevUse = g_prevSwitch = g_prevDeadFire = false;
    g_tick = 0;
    g_dead = false;
    g_deathTicks = 0;
    g_deathHeight = VIEW_HEIGHT;
    g_restartRequested = false;

    for (uint32_t i = 0; i < level->numSpecials; i++) {
        const SpecialHeader *sp = level->special((uint16_t)i);
        if (!sp || sp->kind != SP_MOVE) continue;
        const MoveSpecial *ms = (const MoveSpecial *)sp;
        if (!ms->startClosed) continue;
        for (int k = 0; k < ms->numTargets; k++) {
            uint16_t si = ms->targets[k].sector;
            if (si < level->numSectors) {
                level->sectors[si].ceil = level->sectors[si].floor;
            }
        }
    }

    const int skill = 2;
    for (uint32_t i = 0; i < level->numThings; i++) {
        const ThingDef &td = level->things[i];
        if (!(td.skills & (1 << (skill - 1)))) continue;
        const ActorDef *a = assets->actorByIndex(td.actor);
        if (!a) continue;
        Thing *t = spawnThing(a, td.x, td.y, INT32_MIN,
                              (angle_t)(td.angle << 8));
        if (!t) continue;
        t->special = td.special;
        if (a->flags & AF_COUNTKILL) g_monsters++;
        if (a->id == 1) {
            g_player = t;
            t->isPlayer = 1;
            t->health = g_ply.health;
            for (int k = 0; k < a->numStartItems; k++) {
                const ActorDef *si = assets->actorByIndex(a->startItems[k].actor);
                if (!si) continue;
                if (si->kind == AK_WEAPON) {
                    giveWeapon(si, g_ply.weaponSlot == 0);
                } else {
                    giveInventory(a->startItems[k].actor, a->startItems[k].amount, 0);
                }
            }
        }
    }
}

Camera gameCamera() {
    Camera c;
    if (g_player) {
        c.x = g_player->x;
        c.y = g_player->y;
        c.z = g_player->z + (g_dead ? g_deathHeight : VIEW_HEIGHT);
        c.angle = g_player->angle;
    } else {
        c.x = c.y = 0;
        c.z = VIEW_HEIGHT;
        c.angle = 0;
    }
    return c;
}

int gameHealth() { return g_ply.health; }
int gameArmor() { return g_ply.armor; }
int gameKills() { return g_kills; }
int gameMonsters() { return g_monsters; }
int gameHitFlash() { return g_ply.hitFlash; }
bool gameIsDead() { return g_dead; }
int gameDeathTicks() { return g_deathTicks; }
bool gameRestartRequested() { return g_restartRequested; }

int gameHeldKeys(uint8_t *slots, uint8_t *colors, int max) {
    int n = 0;
    for (uint32_t i = 0; i < g_assets.numActors && n < max; i++) {
        if (i >= MAX_INVENTORY || g_inventory[i] <= 0) continue;
        const ActorDef *a = &g_assets.actors[i];
        if (a->kind != AK_INVENTORY || a->slot == 0) continue;
        slots[n] = a->slot;
        colors[n] = a->hudcolor;
        n++;
    }
    return n;
}

int gameWeaponBobX() { return fixedToInt(g_ply.bobX); }
int gameWeaponBobY() { return fixedToInt(g_ply.bobY); }

int gameSectorLight() {
    if (!g_player || g_player->sector >= g_level.numSectors) return 255;
    return g_level.sectors[g_player->sector].lightlevel;
}

const StateDef *gameWeaponState() {
    int s = g_ply.weaponSlot;
    if (s < 1 || s > 5 || !g_ply.weapon[s] || g_ply.weaponState[s] < 0) return nullptr;
    const ActorDef *w = g_ply.weapon[s];
    if (g_ply.weaponState[s] >= w->numStates) return nullptr;
    return &g_assets.states[w->firstState + g_ply.weaponState[s]];
}

int gameWeaponAmmo() {
    int s = g_ply.weaponSlot;
    if (s < 1 || s > 5 || !g_ply.weapon[s]) return -1;
    const ActorDef *w = g_ply.weapon[s];
    if (w->ammotype == NO_INDEX) return -1;
    return inventoryOf(w->ammotype);
}

namespace {

void tickWeapon(bool firePressed) {
    int slot = g_ply.weaponSlot;
    if (slot < 1 || slot > 5) return;
    const ActorDef *w = g_ply.weapon[slot];
    if (!w) return;
    int guard = 0;
    while (g_ply.weaponTicks[slot] <= 0) {
        if (++guard > 32) break;
        if (g_ply.weaponTicks[slot] == 0) g_ply.weaponState[slot]++;
        if (g_ply.weaponState[slot] < 0 || g_ply.weaponState[slot] >= w->numStates) {
            g_ply.weaponState[slot] = (w->labels[SL_READY] == NO_INDEX)
                                          ? 0
                                          : w->labels[SL_READY];
        }
        const StateDef *st = &g_assets.states[w->firstState + g_ply.weaponState[slot]];
        if (st->kind == SK_STOP) {
            g_ply.weaponState[slot] = w->labels[SL_READY];
            g_ply.weaponTicks[slot] = 1;
            break;
        }
        if (st->kind == SK_GOTO) {
            uint16_t dest = w->labels[st->gotoLabel];
            if (dest == NO_INDEX) dest = w->labels[SL_READY];
            if (dest == NO_INDEX) dest = 0;
            g_ply.weaponState[slot] = dest;
            g_ply.weaponTicks[slot] = -2;
            continue;
        }
        g_ply.weaponTicks[slot] = st->ticks;
        if (st->func == 4) {
            if (firePressed && g_player && !g_player->dead) {
                int need = w->ammouse;
                int have = inventoryOf(w->ammotype);
                bool ok = (w->ammotype == NO_INDEX) || (have >= need);
                if (ok) {
                    if (w->ammotype != NO_INDEX) {
                        g_inventory[w->ammotype] = have - need;
                    }
                    g_drag = w->drag;
                    uint16_t fire = w->labels[SL_FIRE];
                    if (fire != NO_INDEX) {
                        g_ply.weaponState[slot] = fire;
                        g_ply.weaponTicks[slot] = -2;
                        continue;
                    }
                }
            }
        } else if (st->func && g_player) {
            actionFunction(g_player, st);
        }
        if (g_ply.weaponTicks[slot] <= 0) g_ply.weaponTicks[slot] = 1;
        break;
    }
    g_ply.weaponTicks[slot]--;
}

}

void gameUpdate(psyqo::SimplePad &pad) {
    using Pad = psyqo::SimplePad;
    const auto P = Pad::Pad1;

    g_tick++;
    g_ambientLight = (g_ambientLight * 4) / 5;
    g_drag = fmul(g_drag, 54395);

    if (g_player && !g_player->dead) {
        Thing *p = g_player;
        int turn = 0;
        if (pad.isButtonPressed(P, Pad::Left)) turn -= 1;
        if (pad.isButtonPressed(P, Pad::Right)) turn += 1;
        if (pad.isButtonPressed(P, Pad::L2)) turn -= 1;
        if (pad.isButtonPressed(P, Pad::R2)) turn += 1;
        p->angle -= (angle_t)(turn * 380);

        int fwd = 0, strafe = 0;
        if (pad.isButtonPressed(P, Pad::Up)) fwd += 1;
        if (pad.isButtonPressed(P, Pad::Down)) fwd -= 1;
        if (pad.isButtonPressed(P, Pad::L1)) strafe += 1;
        if (pad.isButtonPressed(P, Pad::R1)) strafe -= 1;

        {
            fixed_t speed = intToFixed(p->actor->speed ? p->actor->speed : 4);
            fixed_t targetX = intToFixed(turn * 2);
            angle_t phase = (angle_t)((g_tick * 3 * 65536) / 30);
            fixed_t targetY = 0;
            if (fwd) {
                fixed_t amp = fmul(speed, intToFixed(2));
                targetY = fmul(fcos(phase), fwd < 0 ? -amp : amp);
            }
            g_ply.bobX += fmul(targetX - g_ply.bobX, 19661);
            g_ply.bobY += fmul(targetY - g_ply.bobY, 13107);
        }

        if (fwd || strafe) {
            fixed_t ca, sa;
            headingVector(p->angle, &ca, &sa);
            fixed_t dx = intToFixed(fwd), dz = intToFixed(strafe);
            fixed_t mx = fmul(dx, ca) - fmul(dz, sa);
            fixed_t my = fmul(dx, sa) + fmul(dz, ca);
            applyForces(p, mx, my, intToFixed(p->actor->speed ? p->actor->speed : 4));
        }

        bool useNow = pad.isButtonPressed(P, Pad::Triangle);
        bool usePressed = useNow && !g_prevUse;
        g_prevUse = useNow;
        if (usePressed) {
            struct UseCtx { bool done; } uc = {false};
            g_intersectId++;
            fixed_t dirx, diry;
            headingVector(p->angle, &dirx, &diry);
            intersectSubSector(
                p->ssector, p->x, p->y, dirx, diry, 0,
                p->actor->radius + intToFixed(48), 0,
                [](const Hit &hit, void *ctxp) -> bool {
                    if (!hit.seg || hit.seg->line == NO_INDEX) return false;
                    const LineDef &ld = g_level.lines[hit.seg->line];
                    if ((ld.flags & LF_SPECIAL) && (ld.flags & LF_PLAYERUSE)) {
                        triggerLine(hit.seg->line, g_player);
                        return true;
                    }
                    return false;
                },
                &uc, true);
        }

        bool switchNow = pad.isButtonPressed(P, Pad::Square);
        bool switchPressed = switchNow && !g_prevSwitch;
        g_prevSwitch = switchNow;
        if (switchPressed) {
            for (int i = 1; i <= 5; i++) {
                int s = (g_ply.weaponSlot + i) % 6;
                if (s >= 1 && g_ply.weapon[s]) {
                    g_ply.weaponSlot = s;
                    break;
                }
            }
        }

        tickWeapon(pad.isButtonPressed(P, Pad::Cross));
    } else if (g_player && g_player->dead) {
        if (!g_dead) {
            g_dead = true;
            g_deathTicks = 0;
            g_deathHeight = VIEW_HEIGHT;
            g_prevDeadFire = true;
        }
        g_deathHeight += fmul(intToFixed(10) - g_deathHeight, 13107);
        g_deathTicks++;
        bool fire = pad.isButtonPressed(P, Pad::Cross);
        if (g_deathTicks > 30 && fire && !g_prevDeadFire) g_restartRequested = true;
        g_prevDeadFire = fire;
    }

    updateMovingSectors();

    for (int i = 0; i < g_numThings; i++) {
        Thing *t = &g_things[i];
        if (!t->active) continue;
        if (!t->isPlayer && !tickVM(t)) continue;
        thingPhysics(t);
    }

    if (g_ply.hitFlash > 0) g_ply.hitFlash--;
}

void gameCollectSprites() {
    if (!g_player) return;
    for (int i = 0; i < g_numThings; i++) {
        Thing *t = &g_things[i];
        if (!t->active || t->isPlayer) continue;
        if (!t->state) continue;
        if (!g_level.inPVS(t->ssector, g_player->ssector)) continue;
        renderAddSprite(t);
    }
}
