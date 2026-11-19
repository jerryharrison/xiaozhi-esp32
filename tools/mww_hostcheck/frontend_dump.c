// Host build of the exact micro-speech frontend + ESPHome settings + int8
// quantization used by main/audio/wake_words/micro_wake_word.cc.
// Reads raw s16le mono 16 kHz PCM on stdin; writes one line per feature:
//   40 uint16 frontend values, then 40 int8 quantized values.
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "frontend.h"
#include "frontend_util.h"
#include "../../main/audio/wake_words/micro_wake_word_settings.h"

int main(void) {
    struct FrontendConfig cfg;
    struct FrontendState st;
    mww_fill_frontend_config(&cfg, 10);
    if (!FrontendPopulateState(&cfg, &st, 16000)) { fprintf(stderr, "populate failed\n"); return 1; }
    size_t cap = 1 << 20, n = 0; int16_t* pcm = malloc(cap * 2);
    size_t r; while ((r = fread(pcm + n, 2, cap - n, stdin)) > 0) { n += r; if (n == cap) { cap *= 2; pcm = realloc(pcm, cap * 2); } }
    // Feed in arbitrary chunk sizes (512, like AFE fetch) to exercise buffering.
    size_t idx = 0;
    while (idx < n) {
        size_t avail = n - idx; if (avail > 512) avail = 512;
        const int16_t* p = pcm + idx; size_t left = avail;
        while (left > 0) {
            size_t used = 0;
            struct FrontendOutput o = FrontendProcessSamples(&st, p, left, &used);
            p += used; left -= used;
            if (o.size) {
                for (size_t i = 0; i < o.size; i++) printf("%u ", o.values[i]);
                for (size_t i = 0; i < o.size; i++) printf("%d ", mww_quantize_feature(o.values[i]));
                printf("\n");
            }
            if (used == 0) break;
        }
        idx += avail;
    }
    FrontendFreeStateContents(&st);
    return 0;
}
