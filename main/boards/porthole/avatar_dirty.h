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

// Where this frame can differ from plain sky/sea/rim, as shapes: the horizon rows (all columns) and
// the light's reach (a circle). Rows outside the horizon band only change inside the circle.
typedef struct {
    float hz0, hz1;        // horizon rows (avatar pixels)
    float lx, ly, reach;   // the light's reach circle
} avatar_dirty_t;

static inline void avatar_dirty_shape(const avatar_t* av, avatar_dirty_t* d) {
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
    d->reach = av->fx_reach + 3;
}

// Columns [*x0, *x1) of rows [y0, y1) that can differ from the last frame, given this frame's and
// the last frame's shapes: all of them on horizon rows, else the light circles' extent.
static inline void avatar_band_cols(const avatar_dirty_t* c, const avatar_dirty_t* p, int y0, int y1, int n,
                                    int* x0, int* x1) {
    const avatar_dirty_t* s[2] = {c, p};
    float a = 1e9f, b = -1e9f;
    for (int i = 0; i < 2; i++) {
        if (y1 > s[i]->hz0 && y0 < s[i]->hz1) { *x0 = 0; *x1 = n; return; }
        float yc0 = y0 + 0.5f, yc1 = y1 - 0.5f, ly = s[i]->ly;
        float dy = ly < yc0 ? yc0 - ly : ly > yc1 ? ly - yc1 : 0;   // nearest row centre to the light
        float r = s[i]->reach;
        if (dy >= r) continue;
        float h = sqrtf(r * r - dy * dy) + 1;
        if (s[i]->lx - h < a) a = s[i]->lx - h;
        if (s[i]->lx + h > b) b = s[i]->lx + h;
    }
    int i0 = (int)floorf(a), i1 = (int)ceilf(b) + 1;
    *x0 = i0 < 0 ? 0 : i0 > n ? n : i0;
    *x1 = i1 < 0 ? 0 : i1 > n ? n : i1;
    if (*x1 < *x0) *x1 = *x0;
}
