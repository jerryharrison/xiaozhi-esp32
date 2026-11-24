// Skipper Porthole: at-a-glance cards (packages/avatar-spec/PROTOCOL.md "Cards"), the C++ port of
// packages/avatar-web/src/cards.ts. Plain C++ + cJSON, no ESP-IDF calls: the render task owns a
// CardSet, feeds it card JSON and touch, and asks it each frame for the text to draw and the mood.
// Times are esp_timer microseconds. Wall-clock fields (ends_at, expires_at; unix ms) are turned into
// local deadlines with the message's `now` (the sender's clock), because the device clock is set
// to local time by the OTA check and may not be set at all.
#pragma once
#include <cJSON.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "porthole_jost.h"

enum class CardKind { Glance, Message, Call, Timer };

struct CardParam { char key[12]; float value; };

struct Card {
    std::string id;
    CardKind kind = CardKind::Glance;
    std::string lines[3];   // font encoding (CardUtf8ToFont)
    int nlines = 0;
    std::string label, hint, done, mood = "steady";
    CardParam set[4] = {};
    int nset = 0;
    float dwell = -1;        // seconds, 0 = until handled, <0 = default for the kind
    int priority = -1;       // <0 = default for the kind
    bool has_value = false, has_ends = false;
    float value = 0, total = 0, drain = 0.1f;
    int64_t ends_us = 0, expires_us = 0;
    // runtime
    int64_t answered_us = 0, done_us = 0, play_us = 0, last_ring_us = 0;
};

// One line of card text to draw this frame.
struct CardText {
    const pf_font_t* font;
    std::string text;   // font encoding
    float cx;           // centre x (px)
    int y;              // top of the line box (px)
    int width;          // px, including tracking
    float alpha;        // 0..1
    float track;        // extra px between characters
};

struct CardLayer {
    std::vector<CardText> texts;
    float arc = -1;          // rim progress, fraction clockwise from 12 o'clock (-1: none)
    int dots = 0, dot_on = -1;
    bool dot_pinned = false;
    float dot_alpha = 0;
};

// Character set of porthole_jost.h: ASCII plus these, mapped to control bytes (tools/gen_jost.py).
inline std::string CardUtf8ToFont(const char* s) {
    static const uint32_t kExtra[] = {0, 0x00B7, 0x00B0, 0x2605, 0x2026, 0x2014, 0x2019, 0x00E9};
    std::string out;
    if (s == nullptr) return out;
    const unsigned char* p = (const unsigned char*)s;
    while (*p && out.size() < 120) {
        uint32_t cp;
        int n;
        if (*p < 0x80) { cp = *p; n = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; n = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; n = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; n = 4; }
        else { p++; continue; }
        int k = 1;
        for (; k < n && (p[k] & 0xC0) == 0x80; k++) cp = (cp << 6) | (p[k] & 0x3F);
        p += k;
        if (k < n) continue;
        if (cp >= 32 && cp < 127) { out += (char)cp; continue; }
        if (cp == 0x2018) cp = 0x2019;
        if (cp == 0x2013) cp = 0x2014;
        bool hit = false;
        for (int i = 1; i < 8; i++) if (kExtra[i] == cp) { out += (char)i; hit = true; break; }
        if (!hit && cp == '\n') out += ' ';
    }
    return out;
}

inline int CardTextWidth(const pf_font_t& f, const std::string& s, float track = 0) {
    int w = 0;
    for (char ch : s) {
        const char* p = (ch && ch != '\0') ? strchr(f.chars, ch) : nullptr;
        w += p ? f.glyphs[p - f.chars].adv : f.glyphs[0].adv;
    }
    if (!s.empty()) w += (int)(track * (s.size() - 1));
    return w;
}

class CardSet {
public:
    static constexpr int kMaxCards = 8;
    static constexpr float kRise = 0.95f, kSink = 0.6f, kSwipeSink = 0.42f, kSwipeRise = 0.8f, kSwipeGap = 0.3f;

    // {"type":"card","card":{…},"now":…} or a bare card object. Returns true if the set changed.
    bool ApplyMessage(const cJSON* root, int64_t now_us) {
        const cJSON* card = cJSON_GetObjectItem(root, "card");
        double sender_ms = Num(root, "now", 0);
        return Apply(cJSON_IsObject(card) ? card : root, now_us, sender_ms);
    }
    // The poll reply {"cards":[…],"now":…,"poll":N}: the whole live set.
    bool Reconcile(const cJSON* root, int64_t now_us) {
        const cJSON* arr = cJSON_GetObjectItem(root, "cards");
        if (!cJSON_IsArray(arr)) return false;
        double sender_ms = Num(root, "now", 0);
        bool changed = false;
        std::vector<std::string> keep;
        const cJSON* it;
        cJSON_ArrayForEach(it, arr) {
            const cJSON* id = cJSON_GetObjectItem(it, "id");
            if (cJSON_IsString(id)) keep.push_back(id->valuestring);
            changed |= Apply(it, now_us, sender_ms);
        }
        for (size_t i = live_.size(); i-- > 0;)
            if (std::find(keep.begin(), keep.end(), live_[i].id) == keep.end()) { Remove(live_[i].id, now_us); changed = true; }
        return changed;
    }
    void Remove(const std::string& id, int64_t now_us) {
        for (size_t i = 0; i < live_.size(); i++)
            if (live_[i].id == id) { live_.erase(live_.begin() + i); break; }
        for (auto& in : insts_) if (in.card.id == id) Sink(in, now_us, 0, kSink);
    }

    // Touch on the showing card. Each returns the event to report ("" = none).
    std::string Tap(int64_t now_us) {
        Inst* a = Active();
        if (a == nullptr) return "";
        Card* c = Find(a->card.id);
        if (c == nullptr) return "";
        std::string id = c->id;
        if (c->kind == CardKind::Call && c->answered_us == 0) { c->answered_us = now_us; a->pinned = true; Sync(); return Event(id, "answer"); }
        if (c->kind == CardKind::Timer && c->done_us) { Remove(id, now_us); return Event(id, "done"); }
        if (c->kind == CardKind::Message) { c->play_us = now_us; a->hold_at = now_us; Sync(); return Event(id, "play"); }
        a->pinned = !a->pinned;
        a->hold_at = now_us;
        return Event(id, "tap");
    }
    std::string Hold(int64_t now_us) {
        Inst* a = Active();
        if (a == nullptr) return "";
        Card* c = Find(a->card.id);
        if (c == nullptr) return "";
        std::string id = c->id;
        const char* ev = c->kind == CardKind::Call && c->answered_us == 0 ? "decline" : "dismiss";
        if (c->kind == CardKind::Glance) Sink(*a, now_us, 0, kSink);   // stays live for swiping
        else Remove(id, now_us);
        return Event(id, ev);
    }
    void Swipe(int dir, int64_t now_us) {
        if (live_.empty()) return;
        Inst* a = Active();
        int n = (int)live_.size(), k = -1;
        if (a) for (int i = 0; i < n; i++) if (live_[i].id == a->card.id) k = i;
        int next = ((k < 0 ? (dir > 0 ? -1 : 0) : k) + dir + n) % n;
        Show(live_[next].id, now_us, dir);
    }
    void Drag(float dx) { drag_ = dx; }
    bool Showing() { return Active() != nullptr; }
    size_t LiveCount() const { return live_.size(); }

    // Per frame, before Layout. Returns the card whose mood applies (rising past its start or held),
    // or null. *ring is set when a finished timer wants a ripple.
    const Card* Tick(int64_t now_us, bool* ring) {
        *ring = false;
        for (size_t i = live_.size(); i-- > 0;)
            if (live_[i].expires_us && now_us >= live_[i].expires_us) Remove(live_[i].id, now_us);
        for (auto& c : live_)
            if (c.kind == CardKind::Timer && c.has_ends && c.done_us == 0 && now_us >= c.ends_us) {
                c.done_us = c.last_ring_us = now_us;
                c.priority = std::max(c.priority, 2);
                *ring = true;
                Show(c.id, now_us, 0);
            }
        Sync();
        for (auto& in : insts_) {
            if (in.phase == Phase::Rise && now_us >= in.t0 + Us(in.dur)) { in.phase = Phase::Hold; in.hold_at = now_us; }
            float dw = DwellOf(in.card);
            if (in.phase == Phase::Hold && !in.pinned && dw > 0 && in.card.done_us == 0 && now_us - in.hold_at > Us(dw))
                Sink(in, now_us, 0, kSink);
        }
        insts_.erase(std::remove_if(insts_.begin(), insts_.end(), [&](const Inst& in) {
            return in.phase == Phase::Sink && now_us >= in.t0 + Us(in.dur);
        }), insts_.end());
        Inst* a = Active();
        if (a == nullptr || now_us < a->t0) return nullptr;
        Card* c = Find(a->card.id);
        if (c && c->done_us && now_us - c->last_ring_us > 3200000) { c->last_ring_us = now_us; *ring = true; }
        return c;
    }

    // Seconds left on a card's countdown (or a large number).
    static float Left(const Card& c, int64_t now_us) { return c.has_ends ? (c.ends_us - now_us) / 1e6f : 1e9f; }

    // The text and marks to draw this frame, for a screen of diameter D whose horizon centre sits at y0.
    void Layout(int64_t now_us, float D, float y0, CardLayer* out) {
        out->texts.clear();
        out->arc = -1;
        out->dots = 0;
        const float R = D / 2;
        float vis = 0;
        Inst* a = Active();
        for (auto& in : insts_) {
            float p = Progress(in, now_us);
            if (p <= 0) continue;
            bool dragging = &in == a && drag_ != 0;
            float dx = dragging ? drag_ * 0.35f : 0, alpha = dragging ? 1 - std::min(1.0f, fabsf(drag_) / 260) * 0.6f : 1;
            vis = std::max(vis, p * alpha);
            const Card& c = in.card;
            auto add = [&](const std::string& s, float dy, const pf_font_t* f, float al, float delay, float track_em = 0) {
                if (s.empty()) return;
                float pp = EaseOut(std::clamp((p - delay) / (1 - delay), 0.0f, 1.0f));
                if (pp <= 0.004f) return;
                float rest = std::min(y0 + dy * D, D * 0.86f), q = 1 - pp;   // (never down into the dots)
                float half = sqrtf(std::max(0.0f, 0.81f * R * R - (rest - R) * (rest - R)));
                float maxw = 2 * half - D * 0.06f;
                std::string t = s;
                float track = track_em * f->line_h * 0.8f;
                int w = CardTextWidth(*f, t, track);
                static const pf_font_t* const kSmaller[] = {&jf_call, &jf_title, &jf_msg, &jf_body, &jf_small};
                for (int i = 0; i < 4 && w > maxw; i++)
                    if (f == kSmaller[i]) { f = kSmaller[i + 1]; track = track_em * f->line_h * 0.8f; w = CardTextWidth(*f, t, track); }
                while (w > maxw && t.size() > 2) {   // still too wide: cut with an ellipsis (\x04)
                    t.pop_back();
                    if (t.back() == '\x04') t.pop_back();
                    t.back() = '\x04';
                    w = CardTextWidth(*f, t, track);
                }
                float mid = rest + q * D * 0.075f;   // Surface: drifts up out of the water as it fades in
                CardText ct = {f, t, R + dx + q * in.dir * D * 0.05f, (int)lroundf(mid - f->line_h * 0.5f), w,
                               al * alpha * powf(pp, 1.3f), track};
                out->texts.push_back(ct);
            };
            std::string title = c.nlines > 0 ? c.lines[0] : "", sub = c.nlines > 1 ? c.lines[1] : "";
            bool done = c.kind == CardKind::Timer && c.done_us;
            if (c.kind == CardKind::Call && c.answered_us) sub = Clock((now_us - c.answered_us) / 1e6f);
            if (c.kind == CardKind::Timer && c.has_ends && !done) title = title.empty() ? Clock(Left(c, now_us)) : title + " \x01 " + Clock(Left(c, now_us));
            if (done) {
                add(c.done.empty() ? "Time's up" : c.done, 0.205f, &jf_title, 1, 0);
                add("tap to clear", 0.315f, &jf_body, 0.7f, 0.12f);
            } else if (!c.label.empty()) {
                std::string L = c.label;
                for (auto& ch : L) if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
                add(L, 0.15f, &jf_label, 0.75f, 0, 0.16f);
                add(title, 0.235f, &jf_msg, 1, 0.08f);
                add(sub, 0.315f, &jf_msg, 0.9f, 0.14f);
            } else if (c.kind == CardKind::Call) {
                add(title, 0.2f, &jf_call, 1, 0);
                add(sub.empty() ? "calling\x04" : sub, 0.315f, &jf_body, 0.82f, 0.12f);
            } else {
                add(title, sub.empty() ? 0.235f : 0.205f, &jf_title, 1, 0);
                add(sub, 0.315f, &jf_body, 0.82f, 0.12f);
            }
            if (!done && c.nlines > 2) add(c.lines[2], 0.395f, &jf_body, 0.6f, 0.18f);
            std::string hint = c.hint;
            if (c.kind == CardKind::Message && c.play_us && now_us - c.play_us < 3400000) hint = "playing\x04";
            else if (c.kind == CardKind::Call && c.answered_us) hint = "hold to hang up";
            if (!hint.empty()) add(hint, 0.395f + (R - y0) / D, &jf_small, 0.6f, 0.35f);   // low on the sea, near the rim
            // the rim arc draws itself on as the card surfaces (and off as it sinks)
            float arc = -1;
            if (done) arc = 1;
            else if (c.has_ends) arc = std::clamp(Left(c, now_us) / (c.total > 0 ? c.total : 3600), 0.0f, 1.0f);
            else if (c.has_value) arc = std::clamp(c.value, 0.0f, 1.0f);
            if (arc >= 0) out->arc = std::max(out->arc, arc * EaseOut(std::clamp((p - 0.25f) / 0.75f, 0.0f, 1.0f)));
        }
        if (vis > 0.01f && live_.size() > 1) {
            out->dots = (int)live_.size();
            Inst* cur = a ? a : (insts_.empty() ? nullptr : &insts_.back());
            for (int k = 0; cur && k < out->dots; k++) if (live_[k].id == cur->card.id) out->dot_on = k;
            out->dot_pinned = cur && cur->pinned;
            out->dot_alpha = vis;
        }
    }

    // Reported touch results, drained by the display (id, event).
    std::vector<std::pair<std::string, std::string>> TakeEvents() { auto e = std::move(events_); events_.clear(); return e; }

private:
    enum class Phase { Rise, Hold, Sink };
    struct Inst {
        Card card;   // a copy, so a removed card can still sink
        Phase phase = Phase::Rise;
        int64_t t0 = 0, hold_at = 0;
        float dur = kRise, from = 0;
        int dir = 0;
        bool pinned = false;
    };
    std::vector<Card> live_;
    std::vector<Inst> insts_;
    std::vector<std::pair<std::string, std::string>> events_;
    float drag_ = 0;

    static int64_t Us(float s) { return (int64_t)(s * 1e6f); }
    static float EaseOut(float x) { return 1 - (1 - x) * (1 - x) * (1 - x); }
    static double Num(const cJSON* o, const char* k, double d) { const cJSON* v = cJSON_GetObjectItem(o, k); return cJSON_IsNumber(v) ? v->valuedouble : d; }
    static std::string Str(const cJSON* o, const char* k) { const cJSON* v = cJSON_GetObjectItem(o, k); return cJSON_IsString(v) ? CardUtf8ToFont(v->valuestring) : ""; }
    static std::string Clock(float s) {
        int t = std::max(0, (int)ceilf(s));
        char b[16];
        if (t >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
        else snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
        return b;
    }
    static int DefaultPriority(CardKind k) { return k == CardKind::Call ? 3 : k == CardKind::Message ? 2 : 1; }
    static float DwellOf(const Card& c) {
        if (c.dwell >= 0) return c.dwell;
        return c.kind == CardKind::Glance ? 6 : c.kind == CardKind::Message ? 10 : 0;
    }
    static int PriorityOf(const Card& c) { return c.priority >= 0 ? c.priority : DefaultPriority(c.kind); }
    std::string Event(const std::string& id, const char* e) { events_.push_back({id, e}); return e; }

    Card* Find(const std::string& id) {
        for (auto& c : live_) if (c.id == id) return &c;
        return nullptr;
    }
    Inst* Active() {
        for (auto& in : insts_) if (in.phase != Phase::Sink) return &in;
        return nullptr;
    }
    // Refresh the instances' copies from the live set (text updates, answer/play state).
    void Sync() {
        for (auto& in : insts_) if (Card* c = Find(in.card.id)) in.card = *c;
    }
    float Progress(const Inst& in, int64_t now_us) const {
        float k = std::clamp((now_us - in.t0) / 1e6f / in.dur, 0.0f, 1.0f);
        if (in.phase == Phase::Rise) return now_us < in.t0 ? 0 : k;
        if (in.phase == Phase::Sink) return in.from * (1 - k * k * k);
        return 1;
    }
    void Sink(Inst& in, int64_t now_us, int dir, float dur) {
        if (in.phase == Phase::Sink) return;
        in.from = Progress(in, now_us);
        in.phase = Phase::Sink;
        in.t0 = now_us;
        in.dur = dur;
        in.dir = dir;
    }
    void Show(const std::string& id, int64_t now_us, int dir) {
        Card* c = Find(id);
        if (c == nullptr) return;
        Inst* cur = Active();
        if (cur && cur->card.id == id) { cur->hold_at = now_us; return; }
        int64_t start = now_us;
        if (cur) {
            Sink(*cur, now_us, dir ? -dir : 0, dir ? kSwipeSink : kSink * 0.8f);
            start = now_us + Us(dir ? kSwipeGap : 0.3f);
        }
        Inst in;
        in.card = *c;
        in.t0 = start;
        in.dur = dir ? kSwipeRise : kRise;
        in.dir = dir;
        insts_.push_back(in);
        if (insts_.size() > 4) insts_.erase(insts_.begin());
    }
    bool Apply(const cJSON* m, int64_t now_us, double sender_ms) {
        const cJSON* jid = cJSON_GetObjectItem(m, "id");
        if (!cJSON_IsString(jid) || jid->valuestring[0] == 0) return false;
        std::string id = jid->valuestring;
        if (cJSON_IsTrue(cJSON_GetObjectItem(m, "remove"))) { bool had = Find(id) != nullptr; Remove(id, now_us); return had; }
        Card n;
        Card* old = Find(id);
        if (old) n = *old;
        n.id = id;
        std::string kind = Str(m, "kind");
        n.kind = kind == "message" ? CardKind::Message : kind == "call" ? CardKind::Call : kind == "timer" ? CardKind::Timer : CardKind::Glance;
        n.nlines = 0;
        const cJSON* lines = cJSON_GetObjectItem(m, "lines");
        const cJSON* it;
        if (cJSON_IsArray(lines)) cJSON_ArrayForEach(it, lines) if (cJSON_IsString(it) && n.nlines < 3) n.lines[n.nlines++] = CardUtf8ToFont(it->valuestring);
        n.label = Str(m, "label");
        n.hint = Str(m, "hint");
        n.mood = cJSON_IsString(cJSON_GetObjectItem(m, "mood")) ? cJSON_GetObjectItem(m, "mood")->valuestring : "steady";
        n.nset = 0;
        const cJSON* set = cJSON_GetObjectItem(m, "set");
        if (cJSON_IsObject(set)) cJSON_ArrayForEach(it, set) if (cJSON_IsNumber(it) && n.nset < 4 && strlen(it->string) < sizeof n.set[0].key) {
            strcpy(n.set[n.nset].key, it->string);
            n.set[n.nset++].value = (float)it->valuedouble;
        }
        n.dwell = (float)Num(m, "dwell", -1);
        n.priority = (int)Num(m, "priority", -1);
        n.drain = std::clamp((float)Num(m, "drain", 0.1), 0.0f, 0.1f);
        auto to_local = [&](double ms) -> int64_t {   // a unix-ms deadline as an esp_timer time
            double ref = sender_ms > 0 ? sender_ms : ms;   // without `now`, treat it as already due
            return now_us + (int64_t)((ms - ref) * 1000.0);
        };
        const cJSON* pr = cJSON_GetObjectItem(m, "progress");
        n.has_value = n.has_ends = false;
        if (cJSON_IsObject(pr)) {
            if (cJSON_IsNumber(cJSON_GetObjectItem(pr, "ends_at"))) {
                n.has_ends = true;
                n.ends_us = to_local(Num(pr, "ends_at", 0));
                n.total = (float)Num(pr, "total", 0);
                if (sender_ms <= 0) n.has_ends = false;   // can't place it in time
            } else if (cJSON_IsNumber(cJSON_GetObjectItem(pr, "value"))) {
                n.has_value = true;
                n.value = (float)Num(pr, "value", 0);
            }
            n.done = Str(pr, "done");
        }
        n.expires_us = cJSON_IsNumber(cJSON_GetObjectItem(m, "expires_at")) && sender_ms > 0 ? to_local(Num(m, "expires_at", 0)) : 0;
        if (n.expires_us && n.expires_us <= now_us) { if (old) Remove(id, now_us); return old != nullptr; }
        if (old) {
            bool same = old->nlines == n.nlines && old->label == n.label && old->hint == n.hint && old->mood == n.mood &&
                        old->has_ends == n.has_ends && old->has_value == n.has_value && fabsf(old->value - n.value) < 1e-4f &&
                        llabs(old->ends_us - n.ends_us) < 1500000;
            for (int i = 0; same && i < n.nlines; i++) same = old->lines[i] == n.lines[i];
            *old = n;
            Sync();
            return !same;
        }
        if ((int)live_.size() >= kMaxCards) live_.erase(live_.begin());
        live_.push_back(n);
        Inst* a = Active();
        if (a == nullptr || PriorityOf(n) >= PriorityOf(a->card)) Show(id, now_us, 0);
        return true;
    }
};
