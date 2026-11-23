/* Host check for the Porthole's partial redraws: runs a scripted session (every mood, taps, a
 * voice swell, star, storm, quiet) and keeps a simulated panel updated only in the rows
 * AvatarDisplay would redraw (avatar_dirty.h + the colour signature), then compares it with a full
 * render every frame. Also reports how many rows per frame get redrawn.
 *
 *   cc -O2 -I<avatar-render>/include -I<avatar-render>/src -I.. dirty_check.c <avatar-render>/src/avatar.c -lm
 */
#include "avatar.h"
#include "avatar_dirty.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define W 466
#define BAND 16

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

int main(int argc, char** argv) {
  float fps = argc > 1 ? (float)atof(argv[1]) : 30;
  static uint16_t panel[W * W], ref[W * W];
  static avatar_t av;
  avatar_init(&av, W);
  for (int i = 0; i < 3; i++) av.cur.sky[i] = av.tgt.sky[i] = 0;
  const float dt = 1 / fps;
  int frames = (int)(44 * fps), prev0 = 0, prev1 = 0, rows = 0, fulls = 0, bad_frames = 0;
  long bad_px = 0;
  int maxdiff = 0;
  uint32_t last_sig = 0;
  double t_partial = 0, t_full = 0;
  const char* last_state = "";
  for (int f = 0; f < frames; f++) {
    float t = f * dt;
    const char* st = t < 4 ? "steady" : t < 9 ? "listening" : t < 14 ? "working" : t < 22 ? "steady" : t < 25 ? "star"
                   : t < 32 ? "storm" : t < 38 ? "quiet" : "steady";
    if (strcmp(st, last_state)) { avatar_set_state(&av, st); last_state = st; }
    if (fabsf(t - 6) < dt / 2 || fabsf(t - 30) < dt / 2 || fabsf(t - 40) < dt / 2) avatar_tap(&av);
    if (t >= 15 && t < 21) {   /* Skipper speaking: syllable-ish swell */
      float lv = 0.5f + 0.45f * sinf(t * 11) * sinf(t * 2.3f);
      avatar_feed(&av, lv < 0 ? 0 : lv, 0.5f);
    } else if (t >= 21 && t < 21 + dt * 1.5f) avatar_feed(&av, 0, 0.5f);
    for (int i = 0; i < 3; i++) av.tgt.sky[i] = 0;
    avatar_step(&av, dt);
    for (int i = 0; i < 3; i++) av.cur.sky[i] = av.tgt.sky[i] = 0;   /* as AvatarDisplay does */
    avatar_settle(&av);

    uint32_t sig = avatar_static_sig(&av);
    int a0, a1;
    avatar_active_rows(&av, &a0, &a1);
    int full = f == 0 || sig != last_sig;
    int y0 = full ? 0 : (a0 < prev0 ? a0 : prev0), y1 = full ? W : (a1 > prev1 ? a1 : prev1);
    y0 = y0 / BAND * BAND;
    y1 = (y1 + BAND - 1) / BAND * BAND;
    if (y1 > W) y1 = W;
    double t0 = now_s();
    if (y1 > y0) avatar_render(&av, panel + y0 * W, W, y0, y1, AVATAR_RGB565_SWAP);
    double t1 = now_s();
    avatar_render(&av, ref, W, 0, W, AVATAR_RGB565_SWAP);
    double t2 = now_s();
    t_partial += t1 - t0;
    t_full += t2 - t1;
    rows += y1 - y0;
    fulls += full;
    last_sig = sig;
    prev0 = a0; prev1 = a1;
    long bad = 0;
    int by0 = W, by1 = -1, bx0 = W, bx1 = -1;
    for (int i = 0; i < W * W; i++)
      if (panel[i] != ref[i]) {
        bad++;
        int yy = i / W, xx = i % W;
        if (yy < by0) by0 = yy; if (yy > by1) by1 = yy; if (xx < bx0) bx0 = xx; if (xx > bx1) bx1 = xx;
        uint16_t p = (uint16_t)((panel[i] >> 8) | (panel[i] << 8)), q = (uint16_t)((ref[i] >> 8) | (ref[i] << 8));
        int d = abs(((p >> 5) & 63) - ((q >> 5) & 63));
        if (d > maxdiff) maxdiff = d;
      }
    if (bad) { bad_frames++; bad_px += bad; if (bad_frames <= 5) printf("frame %d t=%.2f %s: %ld px differ (rows %d..%d, active %d..%d) at y %d..%d x %d..%d\n", f, t, st, bad, y0, y1, a0, a1, by0, by1, bx0, bx1); }
  }
  printf("%d frames at %.0f fps: %d rows/frame avg, %d full redraws, %d frames with differences (%ld px, max green diff %d)\n",
         frames, fps, rows / frames, fulls, bad_frames, bad_px, maxdiff);
  printf("host time: partial %.2f ms/frame vs full %.2f ms/frame (%.0f%%)\n", t_partial * 1e3 / frames, t_full * 1e3 / frames,
         100 * t_partial / t_full);
  return bad_frames ? 1 : 0;
}
