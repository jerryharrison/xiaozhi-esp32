// Skipper Porthole: which rows of the avatar can change this frame. Shared by AvatarDisplay and the
// host check in tools/dirty_check.c (renders every frame both ways and compares).
#pragma once
#include "avatar.h"
#include <math.h>

// Rows [*y0, *y1) (in avatar pixels) that can differ from plain sky/sea/rim this frame: the horizon
// band with the live swell and shimmer, and everything within reach of the light (glow, listening
// rings, tap ripples). Everything else only changes with the sea/rim colours or the timer sweep.
static inline void avatar_active_rows(const avatar_t* av, int* y0, int* y1) {
    const float D = (float)av->size;
    float ymin = 1e9f, ymax = -1e9f;
    for (int x = 0; x <= av->size; x++) {
        if (av->ys[x] < ymin) ymin = av->ys[x];
        if (av->ys[x] > ymax) ymax = av->ys[x];
    }
    float m = D * 0.02f * 2.5f + 6;   // line width plus slope and antialiasing margin
    float a0 = ymin - m, a1 = ymax + m;
    if (av->cur.shimmer > 0.01f) a1 += D * 0.13f;
    float l0 = av->ly - av->fx_reach - 3, l1 = av->ly + av->fx_reach + 3;
    if (l0 < a0) a0 = l0;
    if (l1 > a1) a1 = l1;
    int i0 = (int)floorf(a0), i1 = (int)ceilf(a1);
    *y0 = i0 < 0 ? 0 : i0 > av->size ? av->size : i0;
    *y1 = i1 < 0 ? 0 : i1 > av->size ? av->size : i1;
}

// The rows outside the active span depend only on the sea colour and the rim strength
// (lineAlpha), plus the timer sweep (avatar_sweep_rows). Their glides are exponential, so finish them once they are
// within a fraction of a colour step: after that they hold exactly still, and so does the
// signature below (a full redraw happens on any change, so antialiased edges never go stale).
static inline void avatar_settle(avatar_t* av) {
    for (int i = 0; i < 3; i++)
        if (fabsf(av->cur.sea[i] - av->tgt.sea[i]) < 0.4f) av->cur.sea[i] = av->tgt.sea[i];
    if (fabsf(av->cur.lineAlpha - av->tgt.lineAlpha) < 0.003f) av->cur.lineAlpha = av->tgt.lineAlpha;
}

static inline uint32_t avatar_static_sig(const avatar_t* av) {
    float v[5] = {av->cur.sea[0], av->cur.sea[1], av->cur.sea[2], av->cur.lineAlpha, av->timer_on ? 1.0f : 0.0f};
    const uint8_t* p = (const uint8_t*)v;
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < sizeof v; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// Rows (avatar pixels) of the rim where the timer's amber sweep differs between sweep angles a and b
// (radians clockwise from 12 o'clock, as in avatar_frame_t.sweep): the rim sector between them.
static inline void avatar_sweep_rows(const avatar_frame_t* f, float a, float b, int* y0, int* y1) {
    const float lo = a < b ? a : b, hi = a < b ? b : a;
    const float ri = f->R - f->rim - 1.5f, ro = f->R + 0.5f;
    float ymin = 1e9f, ymax = -1e9f;
    const float ends[2] = {lo, hi}, rads[2] = {ri, ro};
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            float y = f->R - rads[j] * cosf(ends[i]);   // the point at that angle: dx = r sin, dy = -r cos
            if (y < ymin) ymin = y;
            if (y > ymax) ymax = y;
        }
    const float pi = 3.14159265f;
    if (lo <= 0 || hi >= 2 * pi) ymin = f->R - ro;   // the sector crosses 12 o'clock
    if (lo <= pi && hi >= pi) ymax = f->R + ro;     // ... or 6 o'clock
    int i0 = (int)floorf(ymin) - 2, i1 = (int)ceilf(ymax) + 3;
    const int n = (int)f->D;
    *y0 = i0 < 0 ? 0 : i0 > n ? n : i0;
    *y1 = i1 < 0 ? 0 : i1 > n ? n : i1;
}

// Where this frame can differ from plain sky/sea/rim, as shapes: the horizon rows (all columns),
// the light's own reach (a circle: glow, body, star, listening rings) and each tap ripple's ring (an
// annulus). All circles share the light's centre. Rows outside the horizon band only change there.
#define AVATAR_DIRTY_MARGIN 3.0f
typedef struct {
    float hz0, hz1;                 // horizon rows (avatar pixels)
    float lx, ly, reach;            // the light's reach circle, without the ripples
    int nrip;
    float rlo[AVATAR_MAX_RIPPLES], rhi[AVATAR_MAX_RIPPLES];   // ripple rings: radii [rlo, rhi]
} avatar_dirty_t;

// After avatar_prepare (the ripple radii come from the prepared frame).
static inline void avatar_dirty_shape(const avatar_t* av, const avatar_frame_t* f, avatar_dirty_t* d) {
    const float D = (float)av->size;
    float ymin = 1e9f, ymax = -1e9f;
    for (int x = 0; x <= av->size; x++) {
        if (av->ys[x] < ymin) ymin = av->ys[x];
        if (av->ys[x] > ymax) ymax = av->ys[x];
    }
    float m = D * 0.02f * 2.5f + 6;   // as avatar_active_rows
    d->hz0 = ymin - m;
    d->hz1 = ymax + m + (av->cur.shimmer > 0.01f ? D * 0.13f : 0);
    d->lx = av->lx;
    d->ly = av->ly;
    // avatar_step's fx_reach without its ripple term (avatar.c: "how far from the light ...").
    float reach = av->gr > av->r * 1.7f ? av->gr : av->r * 1.7f;
    if (av->cur.listen > 0.01f && av->r * 4.4f + D * 0.006f > reach) reach = av->r * 4.4f + D * 0.006f;
    d->reach = reach + 2 + AVATAR_DIRTY_MARGIN;
    if (d->reach > av->fx_reach + AVATAR_DIRTY_MARGIN) d->reach = av->fx_reach + AVATAR_DIRTY_MARGIN;
    d->nrip = f->nrip;
    for (int i = 0; i < f->nrip; i++) {
        float lo = f->rr[i] - f->r_w - 1 - AVATAR_DIRTY_MARGIN;   // the ring and its antialiasing
        d->rlo[i] = lo < 0 ? 0 : lo;
        d->rhi[i] = f->rr[i] + f->r_w + 1 + AVATAR_DIRTY_MARGIN;
    }
}

// Column spans of rows [y0, y1) that can differ from the last frame, given this frame's and the
// last frame's shapes: one full-width span on horizon rows, else the light circle's chord plus each
// ripple ring's chords (left and right of the light). Writes up to 4 spans [x0, x1), even-aligned
// for the CO5300, sorted and disjoint; returns how many (0 = nothing to redraw).
static inline int avatar_band_spans(const avatar_dirty_t* c, const avatar_dirty_t* p, int y0, int y1, int n,
                                    int spans[4][2]) {
    const avatar_dirty_t* s[2] = {c, p};
    const float yc0 = y0 + 0.5f, yc1 = y1 - 0.5f;
    // Intervals of |x + 0.5 - lx| (all shapes are centred on the light's column).
    float iv[2 * (1 + AVATAR_MAX_RIPPLES)][2];
    int k = 0;
    for (int i = 0; i < 2; i++) {
        if (y1 > s[i]->hz0 && y0 < s[i]->hz1) { spans[0][0] = 0; spans[0][1] = n; return 1; }
        if (s[i]->lx != c->lx) { spans[0][0] = 0; spans[0][1] = n; return 1; }   // (lx is fixed today)
        float ly = s[i]->ly;
        float dmin = ly < yc0 ? yc0 - ly : ly > yc1 ? ly - yc1 : 0;   // nearest row centre to the light
        float dmax = fabsf(yc0 - ly) > fabsf(yc1 - ly) ? fabsf(yc0 - ly) : fabsf(yc1 - ly);
        float r = s[i]->reach;
        if (dmin < r) { iv[k][0] = 0; iv[k][1] = sqrtf(r * r - dmin * dmin) + 1; k++; }
        for (int j = 0; j < s[i]->nrip; j++) {
            float lo = s[i]->rlo[j], hi = s[i]->rhi[j];
            if (dmin >= hi) continue;
            float a = dmax < lo ? sqrtf(lo * lo - dmax * dmax) - 1 : 0;
            iv[k][0] = a < 0 ? 0 : a;
            iv[k][1] = sqrtf(hi * hi - dmin * dmin) + 1;
            k++;
        }
    }
    if (k == 0) return 0;
    for (int i = 1; i < k; i++)   // sort by start
        for (int j = i; j > 0 && iv[j][0] < iv[j - 1][0]; j--) {
            float t0 = iv[j][0], t1 = iv[j][1];
            iv[j][0] = iv[j - 1][0]; iv[j][1] = iv[j - 1][1];
            iv[j - 1][0] = t0; iv[j - 1][1] = t1;
        }
    int m = 0;
    for (int i = 0; i < k; i++) {
        if (m > 0 && iv[i][0] <= iv[m - 1][1] + 4) { if (iv[i][1] > iv[m - 1][1]) iv[m - 1][1] = iv[i][1]; }
        else { iv[m][0] = iv[i][0]; iv[m][1] = iv[i][1]; m++; }
    }
    while (m > 2) {   // at most 2 per side: close the smallest gap
        int g = 0;
        for (int i = 1; i < m - 1; i++) if (iv[i + 1][0] - iv[i][1] < iv[g + 1][0] - iv[g][1]) g = i;
        iv[g][1] = iv[g + 1][1];
        for (int i = g + 1; i < m - 1; i++) { iv[i][0] = iv[i + 1][0]; iv[i][1] = iv[i + 1][1]; }
        m--;
    }
    // To pixel columns: x covers |x + 0.5 - lx| in [a, b] for x in [lx - b - 0.5, lx - a - 0.5] (left)
    // and [lx + a - 0.5, lx + b - 0.5] (right). Round outwards, even-align, clip, then merge.
    int out[4][2], o = 0;
    const float lx = c->lx;
    for (int side = 0; side < 2; side++)
        for (int i = 0; i < m; i++) {
            int idx = side ? i : m - 1 - i;   // left side outermost first, so spans come sorted
            float a = iv[idx][0], b = iv[idx][1];
            int x0, x1;
            if (side == 0) { x0 = (int)floorf(lx - b - 0.5f); x1 = (int)ceilf(lx - a - 0.5f) + 1; }
            else { x0 = (int)floorf(lx + a - 0.5f); x1 = (int)ceilf(lx + b - 0.5f) + 1; }
            x0 &= ~1;
            x1 = (x1 + 1) & ~1;
            if (x0 < 0) x0 = 0;
            if (x1 > n) x1 = n;
            if (x1 <= x0) continue;
            if (o > 0 && x0 <= out[o - 1][1]) { if (x1 > out[o - 1][1]) out[o - 1][1] = x1; }
            else { out[o][0] = x0; out[o][1] = x1; o++; }
        }
    for (int i = 0; i < o; i++) { spans[i][0] = out[i][0]; spans[i][1] = out[i][1]; }
    return o;
}
