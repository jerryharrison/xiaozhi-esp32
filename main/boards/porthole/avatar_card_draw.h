// Skipper Porthole: drawing the glance cards (avatar_cards.h) over rendered avatar pixels, and what
// a frame's card layer covers. Shared by AvatarDisplay and the host check tools/card_check.cc.
#pragma once
#include "avatar.h"
#include "avatar_cards.h"
#include "avatar_overlay.h"

inline const pf_glyph_t* PorthGlyph(const pf_font_t& f, char ch) {
    const char* p = ch ? strchr(f.chars, ch) : nullptr;
    return p ? &f.glyphs[p - f.chars] : nullptr;
}

// Rows [y0, y1), columns [x0, x1) of a band (column x0 at band[0], `stride` px per row) on a screen
// `width` px across, whose avatar renders at 1/scale resolution.
inline void porthole_draw_cards(const CardLayer& L, const avatar_t* av, int scale, int width, uint16_t* band, int stride,
                                int x0, int x1, int y0, int y1) {
    const float lw_half = width * 0.01f + 1.5f;   // half the horizon line, plus antialiasing
    for (const auto& t : L.texts) {
        const pf_font_t& f = *t.font;
        if (t.y >= y1 || t.y + f.line_h <= y0 || t.alpha <= 0.01f) continue;
        float x = t.cx - t.width * 0.5f;
        for (char ch : t.text) {
            auto g = PorthGlyph(f, ch);
            if (g == nullptr) { x += f.glyphs[0].adv + t.track; continue; }
            int gx = (int)lroundf(x) + g->xo, gy = t.y + g->yo;
            if (gx < x1 && gx + g->w > x0)
                for (int r = std::max(gy, y0); r < std::min(gy + g->h, y1); r++) {
                    uint16_t* row = band + (r - y0) * stride - x0;
                    for (int c = std::max(0, x0 - gx); c < g->w && gx + c < x1; c++) {
                        int px = gx + c;
                        if (px < 0 || px >= width) continue;
                        // text lives on the sea: nothing above the horizon (+ half the line)
                        int ax = std::min(av->size, px / scale);
                        if (r < av->ys[ax] * scale + lw_half) continue;
                        int idx = (r - gy) * g->w + c;
                        uint8_t byte = f.bitmap[g->off + idx / 2];
                        int a = (idx & 1) ? (byte & 15) : (byte >> 4);
                        if (a) ph_blend(&row[px], kPhBone, a * (1 / 15.0f) * t.alpha);
                    }
                }
            x += g->adv + t.track;
        }
    }
    if (L.arc > 0) porthole_rim_arc(band, stride, x0, x1, width, y0, y1, L.arc, 0);
    if (L.dots > 0) {
        const float R = width / 2.0f, rr = R - width * PORTHOLE_RIM_FRAC - width * 0.03f, sp = 0.052f;
        for (int k = 0; k < L.dots; k++) {
            float ang = PORTHOLE_PI / 2 + ((L.dots - 1) / 2.0f - k) * sp;
            bool on = k == L.dot_on;
            porthole_dot(band, stride, x0, x1, y0, y1, R + cosf(ang) * rr, R + sinf(ang) * rr, width * (on ? 0.0075f : 0.0055f),
                         on && L.dot_pinned ? kPhAmber : kPhBone, (on ? (L.dot_pinned ? 1.0f : 0.9f) : 0.28f) * L.dot_alpha);
        }
    }
}

// A signature of the card layer's text and dots, and the box they cover (x0, y0, x1, y1; empty when
// x1 <= x0). The rim progress is tracked separately (it is a sector of the rim, not a box).
inline void porthole_card_damage(const CardLayer& L, int width, int height, uint32_t* sig, int box[4]) {
    uint32_t h = 2166136261u;
    auto fnv = [&](const void* d, size_t n) { const uint8_t* p = (const uint8_t*)d; for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; };
    box[0] = width; box[1] = height; box[2] = 0; box[3] = 0;
    auto grow = [&](int bx0, int by0, int bx1, int by1) {
        box[0] = std::min(box[0], bx0); box[1] = std::min(box[1], by0);
        box[2] = std::max(box[2], bx1); box[3] = std::max(box[3], by1);
    };
    for (const auto& t : L.texts) {
        // everything that moves a pixel: text, font, position (the x centre to 1/4 px), opacity
        int q[5] = {(int)lroundf(t.cx * 4), t.y, (int)lroundf(t.alpha * 4096), t.font->line_h, (int)lroundf(t.track * 16)};
        fnv(t.text.data(), t.text.size());
        fnv(q, sizeof q);
        int x = (int)floorf(t.cx - t.width * 0.5f);
        grow(x - 4, t.y - 2, x + t.width + 6, t.y + t.font->line_h + 2);
    }
    if (L.dots > 0) {
        int q[4] = {L.dots, L.dot_on, L.dot_pinned, (int)lroundf(L.dot_alpha * 4096)};
        fnv(q, sizeof q);
        const int yd = (int)(height - width * PORTHOLE_RIM_FRAC - width * 0.03f), hw = L.dots * 6 + 8;
        grow(width / 2 - hw, yd - 8, width / 2 + hw, yd + 8);
    }
    *sig = h;
}
