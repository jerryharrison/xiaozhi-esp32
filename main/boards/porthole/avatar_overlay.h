// Skipper Porthole: overlays drawn over the rendered avatar (the volume arc on the rim) and the
// small pixel helpers they share with the text. Plain C, shared by AvatarDisplay and the host
// check in tools/dirty_check.c, so the host test exercises the exact code that runs on the device.
#pragma once
#include <math.h>
#include <stdint.h>

#define PORTHOLE_RIM_FRAC 0.026f   // spec.json geometry.rim
#define PORTHOLE_PI 3.14159265f

static const float kPhBone[3] = {237, 230, 214};
static const float kPhAmber[3] = {242, 181, 68};
static const float kPhSlate[3] = {47, 84, 104};

static inline int ph_clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float ph_clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// Multiply, don't divide: the S3 has no FPU divide (these run per pixel under text and the volume arc).
static inline uint16_t ph_pack(const float c[3]) {
    int r = (int)(c[0] * (31.0f / 255) + 0.5f), g = (int)(c[1] * (63.0f / 255) + 0.5f), b = (int)(c[2] * (31.0f / 255) + 0.5f);
    uint16_t v = (uint16_t)((ph_clampi(r, 0, 31) << 11) | (ph_clampi(g, 0, 63) << 5) | ph_clampi(b, 0, 31));
    return (uint16_t)((v >> 8) | (v << 8));
}

// RGB565 (byte-swapped, as the panel takes it) to 0..255 floats.
static inline void ph_unpack(uint16_t px, float rgb[3]) {
    uint16_t v = (uint16_t)((px >> 8) | (px << 8));
    rgb[0] = ((v >> 11) & 31) * (255.0f / 31);
    rgb[1] = ((v >> 5) & 63) * (255.0f / 63);
    rgb[2] = (v & 31) * (255.0f / 31);
}

static inline void ph_blend(uint16_t* px, const float c[3], float a) {
    if (a <= 0) return;
    if (a > 1) a = 1;
    float rgb[3];
    ph_unpack(*px, rgb);
    for (int i = 0; i < 3; i++) rgb[i] += (c[i] - rgb[i]) * a;
    *px = ph_pack(rgb);
}

// sqrt without libm (flash) or a divide: one rsqrt estimate and three Newton steps.
static inline float ph_sqrt(float x) {
    if (x <= 0) return 0;
    union { float f; uint32_t i; } u;
    u.f = x;
    u.i = 0x5f375a86u - (u.i >> 1);
    float y = u.f, h = 0.5f * x;
    for (int i = 0; i < 3; i++) y = y * (1.5f - h * y * y);
    return x * y;
}

#ifndef PORTHOLE_ARC_LEGACY
#define PORTHOLE_ARC_LEGACY 0   // 1: the pre-2026-11-24 span bug, for the regression check only
#endif

// The volume level in the avatar's own language: the bone rim turns amber clockwise from 12 o'clock
// up to `level` (0..100), like the timer's tide sweep; the rest of the rim dims to slate. Draws rows
// [y0, y1) of a band that holds full rows of `width` pixels (row y0 at band[0]).
//
// Every pixel of the rim ring is visited on both sides of the centre column. (Before 2026-11-24 the
// rows above and below the rim's inner edge, 12 and 6 o'clock, only got their left half, so the bone
// rim showed through just right of 12 and 6 o'clock.)
static inline void porthole_volume_arc(uint16_t* band, int width, int y0, int y1, int level) {
    const float R = width / 2.0f, rim = width * PORTHOLE_RIM_FRAC, rc = R - rim / 2;
    const float ro = R + 0.5f, ri = R - rim - 1;
    const float sweep = 2 * PORTHOLE_PI * ph_clampi(level, 0, 100) / 100.0f;
    // Clockwise from 12 o'clock up to `sweep`, by cross products with the sweep's direction.
    const float wx = sinf(sweep), wy = -cosf(sweep);
    const int big = sweep > PORTHOLE_PI;
    for (int y = y0; y < y1; y++) {
        float dy = y + 0.5f - R;
        if (fabsf(dy) >= ro) continue;
        float xo = ph_sqrt(ro * ro - dy * dy), xi = fabsf(dy) < ri ? ph_sqrt(ri * ri - dy * dy) : 0;
        uint16_t* row = band + (y - y0) * width;
        // The ring's columns in this row: two spans, or one across the centre where the row passes
        // above/below the inner edge.
        int spans[2][2] = {{(int)(R - xo), (int)(R - xi) + 1}, {(int)(R + xi), (int)(R + xo) + 1}};
        int n = 2;
        if (xi == 0) { if (!PORTHOLE_ARC_LEGACY) spans[0][1] = spans[1][1]; n = 1; }
        for (int s = 0; s < n; s++) {
            int xa = spans[s][0] < 0 ? 0 : spans[s][0], xb = spans[s][1] > width ? width : spans[s][1];
            for (int x = xa; x < xb; x++) {
                float dx = x + 0.5f - R, d = ph_sqrt(dx * dx + dy * dy);
                float cov = ph_clampf(rim / 2 - fabsf(d - rc) + 0.5f, 0.0f, 1.0f);
                if (cov <= 0) continue;
                float cr = dx * wy - dy * wx;   // > 0: the sweep direction is clockwise of (dx, dy)
                int lit = big ? !(dx < 0 && cr < 0) : (dx >= 0 && cr >= 0);
                if (!PORTHOLE_ARC_LEGACY) lit = level >= 100 ? 1 : level <= 0 ? 0 : lit;
                if (lit) ph_blend(&row[x], kPhAmber, cov);
                else ph_blend(&row[x], kPhSlate, cov * 0.8f);
            }
        }
    }
}

// Panel rows of the rim sector between angles a and b (radians clockwise from 12 o'clock).
static inline void porthole_rim_arc_rows(int width, int height, float a, float b, int* y0, int* y1) {
    const float R = width / 2.0f, rim = width * PORTHOLE_RIM_FRAC, ri = R - rim - 1, ro = R + 0.5f;
    const float lo = a < b ? a : b, hi = a < b ? b : a;
    float ymin = 1e9f, ymax = -1e9f;
    const float th[2] = {lo, hi}, rr[2] = {ri, ro};
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            float y = R - rr[j] * cosf(th[i]);
            if (y < ymin) ymin = y;
            if (y > ymax) ymax = y;
        }
    if (lo <= 0 || hi >= 2 * PORTHOLE_PI) ymin = R - ro;
    if (lo <= PORTHOLE_PI && hi >= PORTHOLE_PI) ymax = R + ro;
    int i0 = (int)floorf(ymin) - 2, i1 = (int)ceilf(ymax) + 3;
    *y0 = i0 < 0 ? 0 : i0;
    *y1 = i1 > height ? height : i1;
}
