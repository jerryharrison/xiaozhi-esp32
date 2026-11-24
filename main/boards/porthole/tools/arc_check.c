/* Host check for the Porthole's volume arc (avatar_overlay.h) under the device's partial redraws.
 * Runs AvatarDisplay::RenderLoop's row/column logic (active rows, column clipping, overlays on
 * full-width bands only, the rim rows between the old and new level) over:
 *   - a drag: the level 0 -> 100 -> 0 in steps of 5, one step every 2-3 frames, with a moving swell
 *   - every level 0..100 held still (the arc's end at every angle, incl. 12 and 6 o'clock)
 *   - taps and listening rings while the arc shows
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
    int act0 = 0, act1 = W, s0 = 0, s1 = 0;
    if (!full) {
      act0 = (a0 < prev0 ? a0 : prev0); if (act0 < 0) act0 = 0; act0 = act0 / BAND * BAND;
      act1 = (a1 > prev1 ? a1 : prev1); act1 = (act1 + BAND - 1) / BAND * BAND; if (act1 > W) act1 = W;
      if (vsig_prev >= 0 && vsig != vsig_prev) {
        porthole_rim_arc_rows(W, W, 2 * PORTHOLE_PI * vsig_prev / 100.0f, 2 * PORTHOLE_PI * vsig / 100.0f, &s0, &s1);
        s0 = s0 / BAND * BAND; s1 = (s1 + BAND - 1) / BAND * BAND; if (s1 > W) s1 = W;
      }
    }
    vsig_prev = vsig;
    /* RenderRows(act, clip=!full): the arc is on, so OverlayInRows is true: full-width bands + overlays */
    for (int pass = 0; pass < 2; pass++) {
      int y0 = pass ? s0 : act0, y1 = pass ? s1 : act1;
      for (int by = y0; by < y1; by += BAND) {
        int by1 = by + BAND < y1 ? by + BAND : y1;
        avatar_render_cols(&av, &fr, bandbuf, W, by, by1, 1, 0, W, AVATAR_RGB565_SWAP);
        porthole_volume_arc(bandbuf, W, by, by1, level);
        memcpy(panel + by * W, bandbuf, sizeof(uint16_t) * W * (by1 - by));
      }
    }
    shape_prev = shape;
    last_sig = sig; prev0 = a0; prev1 = a1; first = 0;

    avatar_render_frame(&av, &fr, ref, W, 0, W, 1, AVATAR_RGB565_SWAP);
    porthole_volume_arc(ref, W, 0, W, level);
    long bad = 0;
    for (int i = 0; i < W * W; i++) bad += panel[i] != ref[i];
    if (bad) { bad_frames++; bad_px += bad; if (bad_frames <= 5) printf("frame %d level %d: %ld px differ from a full render\n", f, level, bad); }
    int bx = 0, by = 0;
    long bone = bone_on_rim(panel, &bx, &by);
    if (bone) {
      bone_frames++;
      if (bone_frames <= 8) printf("frame %d level %d (%s): %ld bone px on the rim, first at x %d y %d\n", f, level, st, bone, bx, by);
    }
  }
  printf("%d frames: %d differ from full render (%ld px), %d show bone rim through the arc\n", frames, bad_frames, bad_px, bone_frames);
  return bad_frames || bone_frames ? 1 : 0;
}
