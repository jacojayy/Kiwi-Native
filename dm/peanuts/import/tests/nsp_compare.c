// nsp_compare: a real game read both ways on this PC, from its dumps where they are (dm/peanuts/peanuts_nsp.c) and from the .nup the importer made of them
// (dm/host/nup_image.h), and every byte compared. Needs the player's own files, named by environment variables; with any of the first three unset it
// prints "skipped" and exits 0.
//   NSP_CMP_KEYS     the keys file (prod.keys); only its path is used here, and it is never printed
//   NSP_CMP_BASE     the base dump (NSP or XCI)
//   NSP_CMP_NUP      the .nup of the same game ("<file>.nup", or with a selector: "<file>.nup:base")
//   NSP_CMP_UPDATE   the update dump (optional; without it the .nup's base package is the one to name)
//   NSP_CMP_AOC      an add-on's index: NSP_CMP_BASE is then the add-on's dump and NSP_CMP_NUP names its package ("<file>.nup:aoc/<index>")
//   NSP_CMP_RANGES   random ranges per thread (default 4000);  NSP_CMP_THREADS  threads for them (default 4, at most 16)
// It compares the two indexes (the same paths and sizes, entry by entry), reads every file in full from both in 1 MiB pieces, then the random ranges from
// several threads at once, and last reads everything once more from each side alone for the two throughputs (the page cache is warm by then: CPU cost, not
// storage). Exit status 0 when nothing differs.
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define LOG(...) do { fprintf(stderr, "[nsp_compare] " __VA_ARGS__); fputc('\n', stderr); } while (0)
#include "nup_image.h"
#include "../../peanuts_nsp.h"
#include "nx_internal.h"

void plat_sha256(const void *p, size_t n, uint8_t out[32]) { nx_sha256(p, n, out); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec + (double)t.tv_nsec / 1e9; }
static _Thread_local uint64_t t_rng;
static uint64_t rnd(void) { t_rng ^= t_rng << 13; t_rng ^= t_rng >> 7; t_rng ^= t_rng << 17; return t_rng; }

static romfs_img *g_d, *g_n;     // the dumps, the .nup
static uint32_t *g_files; static uint64_t g_nfiles;   // the file entries (the same index in both images)

// Bytes of file entry k of the .nup image: a raw extent, or a compressed file's decompressed bytes
static int nup_read(uint64_t k, uint64_t off, uint8_t *buf, size_t len) {
    const img_ent *e = &g_n->e[k];
    if (e->flags & IMG_ENT_ZSTD) {
        char why[200] = ""; const uint8_t *z = nup_z_get(g_n, e->a, why, sizeof why); if (!z) { LOG("%s", why); return 0; }
        memcpy(buf, z + off, len); nup_z_put(g_n, e->a); return 1;
    }
    return romfs_image_read(g_n->fd, buf, len, (off_t)(e->a + off));
}
static void path_of(const romfs_img *m, uint64_t k, char *out, size_t n) {   // for messages: the entry's name (its directory is not kept in the index)
    snprintf(out, n, "%.*s", (int)m->e[k].nlen, m->names + m->e[k].name);
}

typedef struct { int reads; uint64_t seed, bytes, bad; } Job;
static void *ranges(void *arg) {
    Job *j = arg; uint8_t *a = malloc(1 << 18), *b = malloc(1 << 18); t_rng = j->seed;
    for (int r = 0; r < j->reads; r++) {
        uint64_t k = g_files[rnd() % g_nfiles], size = g_d->e[k].b; if (!size) continue;
        uint64_t off = rnd() % size; size_t len = 1 + (size_t)(rnd() % (rnd() % 4 ? 8192 : 1 << 18)); if (len > size - off) len = (size_t)(size - off);
        if (!pn_nsp_read(g_d, &g_d->e[k], off, a, len) || !nup_read(k, off, b, len) || memcmp(a, b, len)) j->bad++;
        j->bytes += len;
    }
    free(a); free(b); return NULL;
}

int main(void) {
    const char *keys = getenv("NSP_CMP_KEYS"), *base = getenv("NSP_CMP_BASE"), *upd = getenv("NSP_CMP_UPDATE"), *nup = getenv("NSP_CMP_NUP");
    if (!keys || !*keys || !base || !*base || !nup || !*nup) { printf("skipped: NSP_CMP_KEYS, NSP_CMP_BASE and NSP_CMP_NUP name the keys file, the base dump and its .nup\n"); return 0; }
    int nranges = getenv("NSP_CMP_RANGES") ? atoi(getenv("NSP_CMP_RANGES")) : 4000, threads = getenv("NSP_CMP_THREADS") ? atoi(getenv("NSP_CMP_THREADS")) : 4;
    if (threads < 1) threads = 1; if (threads > 16) threads = 16;
    char spec[4096], why[400] = ""; uint64_t bad = 0;
    setenv("DM_NX_KEYS", keys, 1);
    const char *aoc = getenv("NSP_CMP_AOC");
    snprintf(spec, sizeof spec, "nsp:%s%s%s%s%s", base, upd && *upd ? "|" : "", upd && *upd ? upd : "", aoc && *aoc ? "|aoc/" : "", aoc && *aoc ? aoc : "");
    double t0 = now_s();
    if (!(g_d = pn_nsp_open(spec, 0, NULL, why, sizeof why))) { printf("the dumps do not open: %s\n", why); return 1; }
    double t_open = now_s() - t0; t0 = now_s();
    if (!(g_n = nup_image_open(nup, why, sizeof why))) { printf("the .nup does not open: %s\n", why); return 1; }
    printf("dumps: %s; index of %llu entries built in %.0f ms (the .nup's: %.0f ms)\n", pn_nsp_describe(g_d), (unsigned long long)g_d->n, t_open * 1e3, (now_s() - t0) * 1e3);

    // the indexes
    uint64_t ndiff = 0, total = 0;
    if (g_d->n != g_n->n) { printf("DIFFERENT: the dumps give %llu entries, the .nup %llu\n", (unsigned long long)g_d->n, (unsigned long long)g_n->n); return 1; }
    g_files = malloc(sizeof *g_files * (size_t)g_d->n);
    for (uint64_t k = 0; k < g_d->n; k++) {
        const img_ent *x = &g_d->e[k], *y = &g_n->e[k];
        if (x->type != y->type || x->nlen != y->nlen || memcmp(g_d->names + x->name, g_n->names + y->name, x->nlen) || x->b != y->b || (!x->type && x->a != y->a)) {
            char nm[800]; path_of(g_d, k, nm, sizeof nm); if (ndiff++ < 10) printf("DIFFERENT: entry %llu (%s)\n", (unsigned long long)k, nm);
        } else if (x->type) { g_files[g_nfiles++] = (uint32_t)k; total += x->b; }
    }
    printf("index: %llu entries, %llu files, %.2f GiB; entries that differ: %llu\n", (unsigned long long)g_d->n, (unsigned long long)g_nfiles, (double)total / 1073741824.0, (unsigned long long)ndiff);
    if (ndiff || !g_nfiles) return 1;

    // every file in full
    enum { CH = 1 << 20 }; uint8_t *a = malloc(CH), *b = malloc(CH); uint64_t bytes = 0, fbad = 0;
    t0 = now_s();
    for (uint64_t i = 0; i < g_nfiles; i++) {
        uint64_t k = g_files[i], size = g_d->e[k].b; int ok = 1;
        for (uint64_t off = 0; off < size && ok; off += CH) {
            size_t len = size - off < CH ? (size_t)(size - off) : CH;
            if (!pn_nsp_read(g_d, &g_d->e[k], off, a, len)) { printf("the dumps: %s\n", pn_nsp_last_error()); ok = 0; }
            else if (!nup_read(k, off, b, len) || memcmp(a, b, len)) ok = 0;
            bytes += len;
        }
        if (!ok) { char nm[800]; path_of(g_d, k, nm, sizeof nm); if (fbad++ < 10) printf("DIFFERENT: file %s (entry %llu, %llu bytes)\n", nm, (unsigned long long)k, (unsigned long long)size); }
    }
    printf("full read: %llu files, %llu bytes compared, %llu files differ (%.1f s, both sides and the compare)\n", (unsigned long long)g_nfiles, (unsigned long long)bytes, (unsigned long long)fbad, now_s() - t0);
    bad += fbad;

    // random ranges, several threads at once
    pthread_t th[16]; Job j[16]; uint64_t rbytes = 0, rbad = 0;
    for (int i = 0; i < threads; i++) { j[i] = (Job){ nranges, 0x9E3779B97F4A7C15ull * (uint64_t)(i + 1), 0, 0 }; pthread_create(&th[i], NULL, ranges, &j[i]); }
    for (int i = 0; i < threads; i++) { pthread_join(th[i], NULL); rbytes += j[i].bytes; rbad += j[i].bad; }
    printf("random ranges: %d threads x %d, %llu bytes compared, %llu ranges differ\n", threads, nranges, (unsigned long long)rbytes, (unsigned long long)rbad);
    bad += rbad;

    // each side alone, everything once more: the throughputs
    double td, tn; uint64_t hits = 0, misses = 0, held = 0, entries = 0;
    t0 = now_s();
    for (uint64_t i = 0; i < g_nfiles; i++) { uint64_t k = g_files[i], size = g_d->e[k].b; for (uint64_t off = 0; off < size; off += CH) pn_nsp_read(g_d, &g_d->e[k], off, a, size - off < CH ? (size_t)(size - off) : CH); }
    td = now_s() - t0; t0 = now_s();
    for (uint64_t i = 0; i < g_nfiles; i++) { uint64_t k = g_files[i], size = g_n->e[k].b; for (uint64_t off = 0; off < size; off += CH) nup_read(k, off, b, size - off < CH ? (size_t)(size - off) : CH); }
    tn = now_s() - t0;
    nx_shcache_stats(&hits, &misses, &held, &entries);
    printf("throughput, everything in 1 MiB reads, one thread, page cache warm: dumps %.0f MiB/s (%.2f s), .nup %.0f MiB/s (%.2f s)\n",
           (double)total / 1048576.0 / td, td, (double)total / 1048576.0 / tn, tn);
    printf("shared cache of decoded entries: %llu hits, %llu misses, %llu KiB held in %llu entries\n", (unsigned long long)hits, (unsigned long long)misses, (unsigned long long)(held >> 10), (unsigned long long)entries);
    printf(bad ? "DIFFERENT: %llu mismatches\n" : "EQUAL: 0 mismatches\n", (unsigned long long)bad);
    free(a); free(b); romfs_image_close(g_d); romfs_image_close(g_n);
    return bad != 0;
}
