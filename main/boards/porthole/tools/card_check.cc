/* Host check for the Porthole's glance cards under the device's partial redraws.
 * Runs AvatarDisplay::RenderLoop's logic (active rows with column spans and overlays drawn inside
 * every span, the card text box redrawn when the card layer changes, the rim rows between the old
 * and new card progress) through a scripted session: cards surfacing and sinking, a message with a
 * label, a timer counting down and finishing, a call, swipes with a drag, taps, holds, expiry,
 * ripples, and a timer card draining the tide. Every frame the partial panel must equal a full
 * render plus overlays. It also times the card frames and writes a few PPM snapshots.
 *
 *   c++ -std=c++17 -O2 -I<avatar-render>/include -I<avatar-render>/src -I.. -I<cJSON dir> \
 *       card_check.cc <avatar-render>/src/avatar.c <cJSON dir>/cJSON.c -o card_check
 *   ./card_check [snapshot dir]
 */
#include "avatar.h"
#include "avatar_dirty.h"
#include "avatar_overlay.h"
#include "avatar_cards.h"
#include "avatar_card_draw.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#define W 466
#define BAND 16

static uint16_t panel[W * W], ref[W * W], bandbuf[W * BAND];
static avatar_t av;
static avatar_frame_t fr;
static avatar_dirty_t shape, shape_prev;
static CardSet cards;
static CardLayer layer;

static void push(const char* json, int64_t now_us) {
  cJSON* r = cJSON_Parse(json);
  if (!r) { fprintf(stderr, "bad json %s\n", json); exit(2); }
  cards.ApplyMessage(r, now_us);
  cJSON_Delete(r);
}
static void ppm(const char* dir, const char* name, const uint16_t* fb) {
  if (!dir) return;
  char p[512];
  snprintf(p, sizeof p, "%s/%s.ppm", dir, name);
  FILE* f = fopen(p, "wb");
  if (!f) return;
  fprintf(f, "P6\n%d %d\n255\n", W, W);
  for (int i = 0; i < W * W; i++) { float c[3]; ph_unpack(fb[i], c); unsigned char b[3] = {(unsigned char)c[0], (unsigned char)c[1], (unsigned char)c[2]}; fwrite(b, 1, 3, f); }
  fclose(f);
}
static double now_s() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static void render_span(int y0, int y1, const int sp[][2], int ns) {
  for (int by = y0; by < y1; by += BAND) {
    int by1 = by + BAND < y1 ? by + BAND : y1;
    for (int k = 0; k < ns; k++) {
      int cx0 = sp[k][0], cx1 = sp[k][1], w = cx1 - cx0;
      if (w <= 0) continue;
      avatar_render_cols(&av, &fr, bandbuf, w, by, by1, 1, cx0, cx1, AVATAR_RGB565_SWAP);
      porthole_draw_cards(layer, &av, 1, W, bandbuf, w, cx0, cx1, by, by1);
      for (int r = by; r < by1; r++) memcpy(panel + r * W + cx0, bandbuf + (r - by) * w, sizeof(uint16_t) * w);
    }
  }
}

int main(int argc, char** argv) {
  const char* snap = argc > 1 ? argv[1] : nullptr;
  avatar_init(&av, W);
  const float dt = 1 / 30.0f;
  const int frames = 30 * 75;
  int prev0 = 0, prev1 = 0, bad_frames = 0, first = 1, box[4] = {0, 0, 0, 0};
  uint32_t last_sig = 0, card_sig = 0;
  float arc_drawn = -1;
  long bad_px = 0, px_card = 0;
  int card_frames = 0;
  double t_card = 0, t_full = 0;
  std::string applied;
  for (int f = 0; f < frames; f++) {
    float t = f * dt;
    int64_t now = (int64_t)(t * 1e6);
    const double wall = 1795600000000.0 + t * 1000;   // the sender's clock (unix ms)
    char js[600];
    auto at = [&](float s) { return fabsf(t - s) < dt / 2; };
    if (at(1)) { snprintf(js, sizeof js, "{\"type\":\"card\",\"now\":%.0f,\"card\":{\"id\":\"next\",\"label\":\"Next up\",\"lines\":[\"VB practice \xC2\xB7 6:00\",\"Amelia \xC2\xB7 Sports Center\"],\"progress\":{\"ends_at\":%.0f,\"total\":3600},\"expires_at\":%.0f}}", wall, wall + 2400000, wall + 60000); push(js, now); }
    if (at(4)) { snprintf(js, sizeof js, "{\"type\":\"card\",\"now\":%.0f,\"card\":{\"id\":\"w\",\"lines\":[\"59\xC2\xB0 cloudy\",\"hi 60 \xC2\xB7 lo 47\"]}}", wall); push(js, now); }
    if (at(9)) { snprintf(js, sizeof js, "{\"type\":\"card\",\"now\":%.0f,\"card\":{\"id\":\"msg\",\"kind\":\"message\",\"label\":\"Angie\",\"lines\":[\"Running 10 late,\",\"start dinner?\"],\"hint\":\"tap to hear\"}}", wall); push(js, now); }
    if (at(12)) cards.Tap(now);                       /* play the message */
    if (at(16)) cards.Swipe(1, now);
    if (t > 18 && t < 18.6f) cards.Drag(-(t - 18) * 120); else if (at(18.6f)) { cards.Drag(0); cards.Swipe(1, now); }
    if (at(22)) { snprintf(js, sizeof js, "{\"type\":\"card\",\"now\":%.0f,\"card\":{\"id\":\"leave\",\"lines\":[\"Leave by 5:35\",\"18 min drive\"],\"mood\":\"timer\",\"set\":{\"tilt\":0.055},\"progress\":{\"ends_at\":%.0f,\"total\":1800}}}", wall, wall + 900000); push(js, now); }
    if (at(26)) cards.Hold(now);
    if (at(28)) { snprintf(js, sizeof js, "{\"type\":\"card\",\"now\":%.0f,\"card\":{\"id\":\"pasta\",\"kind\":\"timer\",\"lines\":[\"Pasta\"],\"mood\":\"timer\",\"progress\":{\"ends_at\":%.0f,\"total\":12,\"done\":\"Pasta's ready\"}}}", wall, wall + 12000); push(js, now); }
    if (at(44)) cards.Tap(now);                       /* clear the finished timer */
    if (at(46)) { snprintf(js, sizeof js, "{\"type\":\"card\",\"now\":%.0f,\"card\":{\"id\":\"call\",\"kind\":\"call\",\"lines\":[\"Angie\"],\"set\":{\"pulse\":2.2,\"glow\":1.35},\"hint\":\"tap answer \xC2\xB7 hold decline\"}}", wall); push(js, now); }
    if (at(50)) cards.Tap(now);                       /* answer */
    if (at(56)) cards.Hold(now);                      /* hang up */
    if (at(58)) { snprintf(js, sizeof js, "{\"type\":\"card\",\"now\":%.0f,\"card\":{\"id\":\"stars\",\"lines\":[\"Amelia \xE2\x98\x85 159 \xC2\xB7 Ben \xE2\x98\x85 54\",\"chores 12/41 done\"],\"mood\":\"star\"}}", wall); push(js, now); }
    if (at(60) || at(60.5f) || at(61)) avatar_tap(&av);
    if (at(64)) { snprintf(js, sizeof js, "{\"type\":\"card\",\"now\":%.0f,\"card\":{\"id\":\"w\",\"remove\":true}}", wall); push(js, now); }

    /* as AvatarDisplay::ApplyInputs / UpdateCards (idle: cards set the mood) */
    bool ring = false;
    const Card* c = cards.Tick(now, &ring);
    if (ring) avatar_tap(&av);
    cards.TakeEvents();
    std::string want = c ? c->mood : "steady", key = c ? "card:" + c->id + ":" + want : want;
    if (key != applied) {
      avatar_set_state(&av, want.c_str());
      if (c) for (int i = 0; i < c->nset; i++) avatar_set_param(&av, c->set[i].key, c->set[i].value);
      applied = key;
    }
    if (c && c->kind == CardKind::Timer && c->has_ends && !c->done_us)
      avatar_set_param(&av, "level", std::min(0.1f, c->drain) * std::clamp(1 - CardSet::Left(*c, now) / (c->total > 0 ? c->total : 600), 0.0f, 1.0f));
    else if (c && c->kind == CardKind::Timer && c->done_us) avatar_set_param(&av, "level", 0);
    for (int i = 0; i < 3; i++) av.tgt.sky[i] = 0;
    avatar_step(&av, dt);
    for (int i = 0; i < 3; i++) av.cur.sky[i] = av.tgt.sky[i] = 0;
    avatar_settle(&av);
    avatar_prepare(&av, &fr);
    cards.Layout(now, (float)W, W * (0.5f + av.cur.level), &layer);
    uint32_t csig;
    int cbox[4];
    porthole_card_damage(layer, W, W, &csig, cbox);
    float carc = layer.arc > 0 ? roundf(layer.arc * 2000) / 2000 : -1;
    layer.arc = carc;

    uint32_t sig = avatar_static_sig(&av);
    int a0, a1;
    avatar_active_rows(&av, &a0, &a1);
    int full = first || sig != last_sig;
    avatar_dirty_shape(&av, &fr, &shape);
    if (full) shape_prev = shape;
    double t0 = now_s();
    long px0 = 0;
    if (full) {
      int sp[1][2] = {{0, W}};
      render_span(0, W, sp, 1);
    } else {
      int act0 = std::max(0, std::min(a0, prev0)) / BAND * BAND, act1 = std::min(W, (std::max(a1, prev1) + BAND - 1) / BAND * BAND);
      for (int by = act0; by < act1; by += BAND) {
        int by1 = std::min(by + BAND, act1), sp[4][2];
        int ns = avatar_band_spans(&shape, &shape_prev, by, by1, W, sp);
        render_span(by, by1, sp, ns);
        for (int k = 0; k < ns; k++) px0 += (long)(by1 - by) * (sp[k][1] - sp[k][0]);
      }
      if (csig != card_sig) {
        int b[4] = {cbox[0], cbox[1], cbox[2], cbox[3]};
        if (box[2] > box[0]) { b[0] = std::min(b[0], box[0]); b[1] = std::min(b[1], box[1]); b[2] = std::max(b[2], box[2]); b[3] = std::max(b[3], box[3]); }
        int r[4] = {std::max(0, b[0]) & ~1, std::max(0, b[1]) & ~1, std::min(W, (b[2] + 1) & ~1), std::min(W, (b[3] + 1) & ~1)};
        if (r[2] > r[0] && r[3] > r[1]) { int sp[1][2] = {{r[0], r[2]}}; render_span(r[1], r[3], sp, 1); px0 += (long)(r[3] - r[1]) * (r[2] - r[0]); }
      }
      if (carc != arc_drawn) {
        int r[4];   // as the device: only the rim's box between the old and new progress
        porthole_rim_arc_box(W, W, 2 * PORTHOLE_PI * std::max(0.0f, carc), 2 * PORTHOLE_PI * std::max(0.0f, arc_drawn), r);
        int sp[1][2] = {{r[0], r[2]}};
        if (r[2] > r[0] && r[3] > r[1]) { render_span(r[1], r[3], sp, 1); px0 += (long)(r[3] - r[1]) * (r[2] - r[0]); }
      }
    }
    double t1 = now_s();
    if (!layer.texts.empty()) { card_frames++; px_card += px0; t_card += t1 - t0; }
    shape_prev = shape;
    card_sig = csig;
    arc_drawn = carc;
    if (cbox[2] > cbox[0]) memcpy(box, cbox, sizeof box); else box[0] = box[2] = 0;
    last_sig = sig; prev0 = a0; prev1 = a1; first = 0;

    double t2 = now_s();
    avatar_render_frame(&av, &fr, ref, W, 0, W, 1, AVATAR_RGB565_SWAP);
    porthole_draw_cards(layer, &av, 1, W, ref, W, 0, W, 0, W);
    if (!layer.texts.empty()) t_full += now_s() - t2;
    long bad = 0;
    int bx = -1, by = -1;
    for (int i = 0; i < W * W; i++) if (panel[i] != ref[i]) { if (!bad) { bx = i % W; by = i / W; } bad++; }
    if (bad) { bad_frames++; bad_px += bad; if (bad_frames <= 6) printf("frame %d t=%.2f: %ld px differ, first at %d,%d\n", f, t, bad, bx, by); }
    static const float shots[] = {2.2f, 10.5f, 23.5f, 33, 41.5f, 51, 60};
    for (float s : shots) if (at(s)) { char n[32]; snprintf(n, sizeof n, "card-%04.1f", s); ppm(snap, n, panel); }
  }
  printf("%d frames: %d differ from a full render (%ld px)\n", frames, bad_frames, bad_px);
  printf("frames with a card: %d, %ld kpx redrawn per frame (full frame %d kpx), host render %.2f ms vs full %.2f ms\n", card_frames,
         card_frames ? px_card / card_frames / 1000 : 0, W * W / 1000, card_frames ? t_card * 1e3 / card_frames : 0, card_frames ? t_full * 1e3 / card_frames : 0);
  return bad_frames ? 1 : 0;
}
