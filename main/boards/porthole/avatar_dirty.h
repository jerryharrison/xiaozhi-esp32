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
// (lineAlpha), plus the timer sweep. Their glides are exponential, so finish them once they are
// within a fraction of a colour step: after that they hold exactly still, and so does the
// signature below (a full redraw happens on any change, so antialiased edges never go stale).
static inline void avatar_settle(avatar_t* av) {
    for (int i = 0; i < 3; i++)
        if (fabsf(av->cur.sea[i] - av->tgt.sea[i]) < 0.4f) av->cur.sea[i] = av->tgt.sea[i];
    if (fabsf(av->cur.lineAlpha - av->tgt.lineAlpha) < 0.003f) av->cur.lineAlpha = av->tgt.lineAlpha;
}

static inline uint32_t avatar_static_sig(const avatar_t* av) {
    float v[5] = {av->cur.sea[0], av->cur.sea[1], av->cur.sea[2], av->cur.lineAlpha,
                  av->timer_on ? 1 + av->timer_p : 0};
    const uint8_t* p = (const uint8_t*)v;
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < sizeof v; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}
