// lz4_test — nx_lz4.c's decoder against the plain decoder it replaced (tests/nx_selftest.h keeps that one and has the list of checks): malformed blocks
// are rejected, valid ones give the same bytes, nothing is read or written outside the buffers. No keys needed.
//   lz4_test [rounds = 2000]                           generated blocks only
//   lz4_test <rounds> <entries.bin> <entries.tsv>      then also a game's own blocks: entries.bin holds the compressed blocks one after another, entries.tsv
//                                                      one line per block after a header line: index, compressed size, decompressed size (tab separated)
// Exit status 0 when every check passed. Worth running once under -fsanitize=address,undefined: every buffer is allocated at its exact size.
#include "nx_selftest.h"

static int real_entries(const char *bin_path, const char *tsv_path) {
    int bad = st_bad; unsigned long cases = 0, blocks = 0; unsigned long long in_bytes = 0, out_bytes = 0;
    FILE *b = fopen(bin_path, "rb"), *t = fopen(tsv_path, "r"); char line[256];
    if (!b || !t) { perror(b ? tsv_path : bin_path); return 1; }
    if (!fgets(line, sizeof line, t)) { fprintf(stderr, "%s: empty\n", tsv_path); return 1; }
    while (fgets(line, sizeof line, t)) {
        unsigned long idx, psize; unsigned long long vsize;
        if (sscanf(line, "%lu %lu %llu", &idx, &psize, &vsize) != 3) { fprintf(stderr, "%s: bad line: %s", tsv_path, line); return 1; }
        uint8_t *src = st_alloc(psize);
        if (fread(src, 1, psize, b) != psize) { fprintf(stderr, "%s: shorter than its table\n", bin_path); return 1; }
        if (st_lz4_case(src, psize, (size_t)vsize) != (long)vsize) st_fail("a game's block does not decode to its size (index, compressed, decompressed)", idx, psize, (size_t)vsize);
        cases++; blocks++; in_bytes += psize; out_bytes += vsize;
        if (blocks % 8 == 0) st_lz4_abuse(src, psize, (size_t)vsize, 6, 12, &cases);   // every 8th also cut short, with bytes changed, with too little and spare output
        free(src);
    }
    fclose(b); fclose(t);
    printf("     %lu blocks, %.1f MiB compressed, %.1f MiB decompressed\n", blocks, (double)in_bytes / 1048576.0, (double)out_bytes / 1048576.0);
    return st_group("LZ4, a game's own blocks: exact, and every 8th cut short, changed, with too little and spare output", cases, bad);
}

int main(int argc, char **argv) {
    unsigned rounds = argc > 1 ? (unsigned)strtoul(argv[1], NULL, 10) : 2000;
    int bad = nx_selftest_lz4(rounds);
    if (argc > 3) bad += real_entries(argv[2], argv[3]);
    printf("%s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}
