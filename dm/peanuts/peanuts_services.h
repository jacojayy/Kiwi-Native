// Kiwi Native: the parts of peanuts_services.c that only compute, so a PC test can run them (rt/test/svc_test.cpp): the add-on content spec, the frame
// totals' arithmetic over the replay's ring of GPU times, and when a pushed battery reading is still believed.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- add-on content ----
// The spec the host's own mount makes for an index (dm/host/hle_fs.c aoc_dir): "<dir>/<i>.nup:aoc/<i>" or "nsp:<dir>/<i>.nsp|aoc/<i>", and for a link whose target
// is fd:<n>/<name> (the launcher's descriptor of a file used where it is) the target in place of the path. 0: the index is neither (a folder, an image, nothing)
static inline int pn_aoc_spec(char *spec, size_t n, const char *base, int index) {
    char p[1024], t[512]; struct stat st; ssize_t len;
    if (snprintf(p, sizeof p, "%s/%d.nup", base, index) < (int)sizeof p) {
        if (!stat(p, &st) && S_ISREG(st.st_mode)) return snprintf(spec, n, "%s:aoc/%d", p, index) < (int)n;
        if ((len = readlink(p, t, sizeof t - 1)) > 3 && (t[len] = 0, !strncmp(t, "fd:", 3))) return snprintf(spec, n, "%s:aoc/%d", t, index) < (int)n;
    }
    if (snprintf(p, sizeof p, "%s/%d.nsp", base, index) < (int)sizeof p) {
        if (!stat(p, &st) && S_ISREG(st.st_mode)) return snprintf(spec, n, "nsp:%s|aoc/%d", p, index) < (int)n;
        if ((len = readlink(p, t, sizeof t - 1)) > 3 && (t[len] = 0, !strncmp(t, "fd:", 3))) return snprintf(spec, n, "nsp:%s|aoc/%d", t, index) < (int)n;
    }
    return 0;
}

// ---- frame totals ----
// The replay keeps each frame's GPU time in a ring of PN_PERF_RING frames, in about PN_PERF_LAG frames after the frame; gpu_ms(f) is frame f's time in ms, or
// negative while it is not in (or no longer). A step at frame number `frame` adds the frames from the last step's end up to frame - PN_PERF_LAG whose time is in;
// frames the ring has already dropped (a step came late) are passed over, never counted with another frame's time
enum { PN_PERF_RING = 64, PN_PERF_LAG = 9 };
typedef struct { uint64_t next, frames; int64_t sum_ns; } pn_perf_acc;
static inline void pn_perf_step(pn_perf_acc *a, uint64_t frame, double (*gpu_ms)(uint64_t f)) {
    if (frame < a->next) a->next = 0;                                              // (the counter never goes back; a guard, not a case)
    if (frame >= PN_PERF_LAG) { uint64_t end = frame - PN_PERF_LAG + 1;             // frames [next, end) are due
        if (end > PN_PERF_RING - PN_PERF_LAG && a->next < end - (PN_PERF_RING - PN_PERF_LAG)) a->next = end - (PN_PERF_RING - PN_PERF_LAG);   // older ones left the ring
        for (; a->next < end; a->next++) { double g = gpu_ms(a->next); if (g >= 0) { a->sum_ns += (int64_t)(g * 1e6); a->frames++; } } }
}

// ---- battery ----
// A reading the app pushed is believed for PN_BATTERY_FRESH_NS; older (the app's loop stopped), the kernel's files are asked instead. pushed_pct: -2 never
// pushed, -1 the device has no battery, else 0..100
#define PN_BATTERY_FRESH_NS 60000000000ull
static inline int pn_battery_pushed_fresh(int pushed_pct, uint64_t pushed_ns, uint64_t now_ns) { return pushed_pct != -2 && now_ns - pushed_ns < PN_BATTERY_FRESH_NS; }
// The kernel's two lines as an answer: 1 with *percent 0..100 and *charging ("Charging" or "Full", as the emulator's Android side counts it); 0 when the level is no number
static inline int pn_battery_parse(const char *capacity, const char *status, int *percent, int *charging) {
    int v = -1;
    if (!capacity || sscanf(capacity, "%d", &v) != 1 || v < 0 || v > 100) return 0;
    *percent = v; *charging = status && (!strcmp(status, "Charging") || !strcmp(status, "Full"));
    return 1;
}

#ifdef __cplusplus
}
#endif
