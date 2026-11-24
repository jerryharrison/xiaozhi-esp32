/* Host check for the Porthole's volume arc (avatar_overlay.h) under the device's partial redraws.
 * Runs AvatarDisplay::RenderLoop's row/column logic (active rows with column clipping, the arc drawn
 * inside every span, the rim rows between the old and new level) over:
 *   - a drag: the level 0 -> 100 -> 0 in steps of 5, one step every 2-3 frames, with a moving swell
 *   - every level 0..100 held still (the arc's end at every angle, incl. 12 and 6 o'clock)
 *   - taps and listening rings while the arc shows
 * The rim between the old and new level is redrawn as its box only (porthole_rim_arc_box), like the device.
 * Every frame it checks (1) the partial panel equals a full render plus overlays, pixel for pixel,
 * and (2) no fully covered rim pixel is bone (the bug Jerry saw: white rim through the amber).
 *
 *   cc -O2 -I<avatar-render>/include -I<avatar-render>/src -I.. arc_check.c <avatar-render>/src/avatar.c -lm
 * Add -DPORTHOLE_ARC_LEGACY=1 to build the pre-fix arc: the check must fail.
 */
#include "avatar.h"
#include "avatar_dirty.h"
#include "avatar_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 466
#define BAND 16

static uint16_t panel[W * W], ref[W * W], bandbuf[W * BAND];
static avatar_t av;
static avatar_frame_t fr;
static avatar_dirty_t shape, shape_prev;
static uint16_t ref2[W * W], panel_fast[W * W];
static int fast_bad;

/* The arc as it was computed before the fast path (every pixel through sqrt + blend): the fast path
 * must give the same pixels. */
static inline void ref_rim_arc(uint16_t* band, int stride, int x0, int x1, int width, int y0, int y1, float frac,
                                    int slate_rest) {
    const float R = width / 2.0f, rim = width * PORTHOLE_RIM_FRAC, rc = R - rim / 2;
    const float ro = R + 0.5f, ri = R - rim - 1;
    frac = ph_clampf(frac, 0, 1);
    const float sweep = 2 * PORTHOLE_PI * frac;
    // Clockwise from 12 o'clock up to `sweep`, by cross products with the sweep's direction.
    const float wx = sinf(sweep), wy = -cosf(sweep);
    const int big = sweep > PORTHOLE_PI;
    for (int y = y0; y < y1; y++) {
        float dy = y + 0.5f - R;
        if (fabsf(dy) >= ro) continue;
        float xo = ph_sqrt(ro * ro - dy * dy), xi = fabsf(dy) < ri ? ph_sqrt(ri * ri - dy * dy) : 0;
        uint16_t* row = band + (y - y0) * stride - x0;
        // The ring's columns in this row: two spans, or one across the centre where the row passes
        // above/below the inner edge.
        int spans[2][2] = {{(int)(R - xo), (int)(R - xi) + 1}, {(int)(R + xi), (int)(R + xo) + 1}};
        int n = 2;
        if (xi == 0) { if (!PORTHOLE_ARC_LEGACY) spans[0][1] = spans[1][1]; n = 1; }
        for (int s = 0; s < n; s++) {
            int xa = spans[s][0] < x0 ? x0 : spans[s][0], xb = spans[s][1] > x1 ? x1 : spans[s][1];
            for (int x = xa; x < xb; x++) {
                float dx = x + 0.5f - R, d = ph_sqrt(dx * dx + dy * dy);
                float cov = ph_clampf(rim / 2 - fabsf(d - rc) + 0.5f, 0.0f, 1.0f);
                if (cov <= 0) continue;
                float cr = dx * wy - dy * wx;   // > 0: the sweep direction is clockwise of (dx, dy)
                int lit = big ? !(dx < 0 && cr < 0) : (dx >= 0 && cr >= 0);
                if (!PORTHOLE_ARC_LEGACY) lit = frac >= 1 ? 1 : frac <= 0 ? 0 : lit;
                if (lit) ph_blend(&row[x], kPhAmber, cov);
                else if (slate_rest) ph_blend(&row[x], kPhSlate, cov * 0.8f);
            }
        }
    }
}


/* Rim pixels that must be amber or slate whenever the arc shows: fully inside the ring (1 px margin). */
static long bone_on_rim(const uint16_t* fb, int* bx, int* by) {
  const float R = W / 2.0f, rim = W * PORTHOLE_RIM_FRAC, rc = R - rim / 2;
  long n = 0;
  for (int y = 0; y < W; y++)
    for (int x = 0; x < W; x++) {
      float dx = x + 0.5f - R, dy = y + 0.5f - R, d = sqrtf(dx * dx + dy * dy);
      if (fabsf(d - rc) > rim / 2 - 1.5f) continue;
      float c[3];
      ph_unpack(fb[y * W + x], c);
      if (c[2] > 150 && c[1] > 200) { if (!n) { *bx = x; *by = y; } n++; }   /* bone: amber/slate both have blue < 130 */
    }
  return n;
}

int main(void) {
  avatar_init(&av, W);
  const float dt = 1 / 30.0f;
  int prev0 = 0, prev1 = 0, vsig_prev = -1, bad_frames = 0, bone_frames = 0, first = 1;
  uint32_t last_sig = 0;
  long bad_px = 0;
  int level = 0, dir = 5;
  /* script: drag up and down twice (frames 0..~170), then hold each level (101 x 3 frames), then taps */
  int frames = 2 * 2 * 21 * 2 + 303 + 120;
  for (int f = 0; f < frames; f++) {
    const char* st = f < 168 ? "working" : f < 471 ? "steady" : "listening";
    if (f == 0 || f == 168 || f == 471) avatar_set_state(&av, st);
    if (f < 168) {                       /* drag: a step every 2 frames, sometimes 3 */
      if (f % 2 == 0 && (f % 14) != 0) { level += dir; if (level >= 100) { level = 100; dir = -5; } if (level <= 0) { level = 0; dir = 5; } }
    } else if (f < 471) level = (f - 168) / 3;
    else { level = 35; if (f % 15 == 0) avatar_tap(&av); }
    for (int i = 0; i < 3; i++) av.tgt.sky[i] = 0;
    avatar_step(&av, dt);
    for (int i = 0; i < 3; i++) av.cur.sky[i] = av.tgt.sky[i] = 0;
    avatar_settle(&av);
    avatar_prepare(&av, &fr);

    /* as RenderLoop: the arc is an overlay; its on/off changes the overlay signature (full redraw) */
    uint32_t sig = avatar_static_sig(&av);
    int vsig = level;
    int a0, a1;
    avatar_active_rows(&av, &a0, &a1);
    int full = first || sig != last_sig;
    avatar_dirty_shape(&av, &fr, &shape);
    if (full) shape_prev = shape;
    int act0 = 0, act1 = W, s0 = 0, s1 = 0, bx0 = 0, bx1 = W;
    if (!full) {
      act0 = (a0 < prev0 ? a0 : prev0); if (act0 < 0) act0 = 0; act0 = act0 / BAND * BAND;
      act1 = (a1 > prev1 ? a1 : prev1); act1 = (act1 + BAND - 1) / BAND * BAND; if (act1 > W) act1 = W;
      if (vsig_prev >= 0 && vsig != vsig_prev) {
        int box[4];
        porthole_rim_arc_box(W, W, 2 * PORTHOLE_PI * vsig_prev / 100.0f, 2 * PORTHOLE_PI * vsig / 100.0f, box);
        s0 = box[1]; s1 = box[3]; bx0 = box[0]; bx1 = box[2];
      }
    }
    vsig_prev = vsig;
    /* RenderRows(act, clip=!full): column spans (avatar_band_spans), the arc drawn inside each span
     * (overlays are column-aware); then the rim box between the old and new level. */
    for (int pass = 0; pass < 2; pass++) {
      int y0 = pass ? s0 : act0, y1 = pass ? s1 : act1;
      for (int by = y0; by < y1; by += BAND) {
        int by1 = by + BAND < y1 ? by + BAND : y1;
        int sp[4][2] = {{0, W}}, ns = 1;
        if (!full && pass == 0) ns = avatar_band_spans(&shape, &shape_prev, by, by1, W, sp);
        if (pass == 1) { sp[0][0] = bx0; sp[0][1] = bx1; }
        for (int k = 0; k < ns; k++) {
          int cx0 = sp[k][0], cx1 = sp[k][1], w = cx1 - cx0;
          avatar_render_cols(&av, &fr, bandbuf, w, by, by1, 1, cx0, cx1, AVATAR_RGB565_SWAP);
          porthole_volume_arc(bandbuf, w, cx0, cx1, W, by, by1, level);
          for (int r = by; r < by1; r++) memcpy(panel + r * W + cx0, bandbuf + (r - by) * w, sizeof(uint16_t) * w);
        }
      }
    }
    shape_prev = shape;
    last_sig = sig; prev0 = a0; prev1 = a1; first = 0;

    avatar_render_frame(&av, &fr, ref, W, 0, W, 1, AVATAR_RGB565_SWAP);
    porthole_volume_arc(ref, W, 0, W, W, 0, W, level);
    long bad = 0;
    for (int i = 0; i < W * W; i++) bad += panel[i] != ref[i];
    /* the fast path against the old per-pixel arc, as a volume level and as a card's progress (bone rest) */
    for (int slate = 0; slate < 2; slate++) {
      float fr2 = slate ? level / 100.0f : (f % 101) / 100.0f;
      avatar_render_frame(&av, &fr, ref2, W, 0, W, 1, AVATAR_RGB565_SWAP);
      memcpy(panel_fast, ref2, sizeof ref2);
      ref_rim_arc(ref2, W, 0, W, W, 0, W, fr2, slate);
      porthole_rim_arc(panel_fast, W, 0, W, W, 0, W, fr2, slate);
      long d = 0;
      for (int i = 0; i < W * W; i++) d += panel_fast[i] != ref2[i];
      if (d) { fast_bad++; if (fast_bad <= 5) printf("frame %d frac %.2f slate %d: fast arc differs from the per-pixel arc in %ld px\n", f, fr2, slate, d); }
    }
    if (bad) { bad_frames++; bad_px += bad; if (bad_frames <= 5) printf("frame %d level %d: %ld px differ from a full render\n", f, level, bad); }
    int bx = 0, by = 0;
    long bone = bone_on_rim(panel, &bx, &by);
    if (bone) {
      bone_frames++;
      if (bone_frames <= 8) printf("frame %d level %d (%s): %ld bone px on the rim, first at x %d y %d\n", f, level, st, bone, bx, by);
    }
  }
  printf("%d frames: %d differ from full render (%ld px), %d show bone rim through the arc, %d fast-arc mismatches\n", frames, bad_frames, bad_px, bone_frames, fast_bad);
  /* box sanity: every rim pixel whose lit state flips between two levels lies inside the box */
  long outside = 0;
  for (int la = 0; la <= 100; la++)
    for (int lb = 0; lb <= 100; lb += 1) {
      if (la == lb) continue;
      int box[4];
      porthole_rim_arc_box(W, W, 2 * PORTHOLE_PI * la / 100.0f, 2 * PORTHOLE_PI * lb / 100.0f, box);
      if (lb % 7 && lb != la + 1 && lb != la - 1) continue;   /* every neighbour pair, every 7th level otherwise */
      memset(ref, 0, sizeof ref); memset(ref2, 0, sizeof ref2);
      porthole_volume_arc(ref, W, 0, W, W, 0, W, la);
      porthole_volume_arc(ref2, W, 0, W, W, 0, W, lb);
      for (int y = 0; y < W; y++)
        for (int x = 0; x < W; x++)
          if (ref[y * W + x] != ref2[y * W + x] && (x < box[0] || x >= box[2] || y < box[1] || y >= box[3])) outside++;
    }
  printf("arc box: %ld changed px outside the box\n", outside);
  return bad_frames || bone_frames || fast_bad || outside ? 1 : 0;
}
