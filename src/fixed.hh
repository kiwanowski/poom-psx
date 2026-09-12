#pragma once

#include <stdint.h>

typedef int32_t fixed_t;

#ifdef POOM_PROFILE
struct FixedOpCounts {
    uint64_t mul, div, sqrt, atan2, normal, sin, div64, div32, segTest, segSolve;
    uint64_t outlineCut, pieceCap, clipDrop;
};
extern FixedOpCounts g_fixedOps;
#define FIXED_COUNT(which) (++g_fixedOps.which)
#else
#define FIXED_COUNT(which) ((void)0)
#endif

static constexpr fixed_t FRACUNIT = 1 << 16;
static constexpr int FRACBITS = 16;

typedef uint16_t angle_t;

static constexpr angle_t ANG90 = 0x4000;
static constexpr angle_t ANG180 = 0x8000;

static inline fixed_t fmul(fixed_t a, fixed_t b) {
    FIXED_COUNT(mul);
    return (fixed_t)(((int64_t)a * (int64_t)b) >> FRACBITS);
}

static inline fixed_t fdiv(fixed_t a, fixed_t b) {
    FIXED_COUNT(div);
    if (b == 0) return a < 0 ? INT32_MIN : INT32_MAX;
    return (fixed_t)((((int64_t)a) << FRACBITS) / b);
}

static constexpr fixed_t fabsf_(fixed_t a) { return a < 0 ? -a : a; }

static constexpr int32_t fixedToInt(fixed_t a) { return a >> FRACBITS; }
static constexpr fixed_t intToFixed(int32_t a) {
    return (fixed_t)((uint32_t)a << FRACBITS);
}

extern const fixed_t g_sinTable[1024];

static inline fixed_t fsin(angle_t a) { FIXED_COUNT(sin); return g_sinTable[a >> 6]; }
static inline fixed_t fcos(angle_t a) { FIXED_COUNT(sin); return g_sinTable[((a >> 6) + 256) & 1023]; }

static inline void headingVector(angle_t a, fixed_t *dx, fixed_t *dy) {
    *dx = fcos(a);
    *dy = fsin(a);
}

fixed_t fsqrt(fixed_t x);
angle_t fatan2(fixed_t x, fixed_t y);

fixed_t v2Normal(fixed_t dx, fixed_t dy, fixed_t *nx, fixed_t *ny);

static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
