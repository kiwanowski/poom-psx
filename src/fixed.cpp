#include "fixed.hh"

#ifdef POOM_PROFILE
FixedOpCounts g_fixedOps;
#endif

const fixed_t g_sinTable[1024] = {
#include "sintable.inc"
};

fixed_t fsqrt(fixed_t x) {
    FIXED_COUNT(sqrt);
    if (x <= 0) return 0;
    uint64_t n = (uint64_t)(uint32_t)x << 32;
    uint64_t rem = 0;
    uint32_t root = 0;
    for (int i = 0; i < 24; i++) {
        rem = (rem << 2) | (n >> 62);
        n <<= 2;
        root <<= 1;
        uint64_t trial = 2ull * root + 1;
        if (trial <= rem) {
            rem -= trial;
            root++;
        }
    }
    return (fixed_t)root;
}

angle_t fatan2(fixed_t x, fixed_t y) {
    FIXED_COUNT(atan2);
    if (x == 0 && y == 0) return 0;
    fixed_t ax = fabsf_(x), ay = fabsf_(y);
    bool swapped = ax < ay;
    fixed_t t = swapped ? fdiv(ax, ay) : fdiv(ay, ax);
    int32_t r = fmul(t, 8192) -
                fmul(fmul(t, t - FRACUNIT), 2552 + fmul(t, 692));
    uint32_t ang = (uint32_t)r;
    if (swapped) ang = 0x4000u - ang;
    if (x < 0) ang = 0x8000u - ang;
    if (y > 0) ang = (uint32_t)(-(int32_t)ang);
    return (angle_t)ang;
}

fixed_t v2Normal(fixed_t dx, fixed_t dy, fixed_t *nx, fixed_t *ny) {
    int32_t sx = dx >> 8, sy = dy >> 8;
    int64_t sq = (int64_t)sx * sx + (int64_t)sy * sy;
    if (sq == 0) {
        *nx = FRACUNIT;
        *ny = 0;
        return 0;
    }
    fixed_t len;
    if (sq <= (int64_t)INT32_MAX) {
        len = fsqrt((fixed_t)sq);
    } else {
        len = fsqrt((fixed_t)(sq >> 16)) << 8;
    }
    if (len == 0) {
        *nx = FRACUNIT;
        *ny = 0;
        return 0;
    }
    *nx = fdiv(dx, len);
    *ny = fdiv(dy, len);
    return len;
}
