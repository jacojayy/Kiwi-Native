// nx_selftest.h — the self-tests of the importer's AES (nx_crypto.c) and LZ4 (nx_lz4.c) routines, in a header so the same checks run from
// tests/crypto_test.c and tests/lz4_test.c on a PC and from any tool built for a device, where the ARMv8 path can only be run on the hardware.
//   nx_selftest_crypto(rounds): known answers (FIPS-197 C.1, SP 800-38A F.1.1/F.1.2 ECB and F.5.1/F.5.2 CTR, IEEE 1619 XTS vector 1) under the hardware
//     cipher and under the portable one; then the hardware cipher against the portable one, byte for byte and counter for counter: CTR over every length
//     0..300 with counters at and around a wrap of the low 64 bits and of all 128, in place and not, at unaligned addresses; `rounds` random cases up to
//     4 MB; a stream decrypted in pieces against one call; single blocks, ECB and XTS.
//   nx_selftest_lz4(rounds): nx_lz4_decompress against the plain decoder it replaced (kept below as the reference): hand-made malformed blocks that must
//     be rejected, `rounds` generated blocks with every kind of run, each also cut short, with single bytes changed and with too little or spare output.
// Both return the number of failed checks and print one line per group. Every buffer is allocated at its exact size, so a build with an address sanitizer
// sees any access past an end; without one, guard bytes after the output catch a write past it.
#ifndef NX_SELFTEST_H
#define NX_SELFTEST_H
#include "nx_crypto.h"
#include "nx_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t st_seed = 0x243F6A8885A308D3ull;
static uint64_t st_rnd(void) { uint64_t z = (st_seed += 0x9E3779B97F4A7C15ull); z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull; z = (z ^ (z >> 27)) * 0x94D049BB133111EBull; return z ^ (z >> 31); }
static void st_fill(uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) { if (!(i & 7)) { uint64_t v = st_rnd(); size_t k = n - i < 8 ? n - i : 8; memcpy(p + i, &v, k); } } }
static void *st_alloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { fprintf(stderr, "selftest: out of memory (%zu bytes)\n", n); exit(2); } return p; }
static int st_bad, st_shown;
static void st_fail(const char *what, size_t a, size_t b, size_t c) { st_bad++; if (st_shown++ < 12) printf("  FAIL %s (%zu, %zu, %zu)\n", what, a, b, c); }
static int st_group(const char *name, unsigned long cases, int bad_before) { printf("%s %s: %lu cases\n", st_bad == bad_before ? "ok  " : "FAIL", name, cases); return st_bad - bad_before; }
static void st_unhex(uint8_t *out, const char *hex) { for (size_t i = 0; hex[2 * i]; i++) { unsigned v; sscanf(hex + 2 * i, "%2x", &v); out[i] = (uint8_t)v; } }

// ---------------------------------------------------------------- AES
#define ST_GUARD 32
static void st_ctr_inc(uint8_t c[16]) { for (int i = 15; i >= 0; i--) if (++c[i]) break; }
// CTR from its definition: one block encryption per counter value, the counter stepped bytewise
static void st_ctr_plain(const NxAes *a, uint8_t ctr[16], const uint8_t *in, uint8_t *out, size_t n) {
    for (size_t i = 0; i < n; i += 16) { uint8_t ks[16]; nx_aes_encrypt_block(a, ctr, ks); for (size_t j = 0; j < 16 && i + j < n; j++) out[i + j] = (uint8_t)(in[i + j] ^ ks[j]); st_ctr_inc(ctr); }
}

static void st_kats(const char *which) {
    uint8_t key[16], a[64], b[64], c[64], ctr[16]; NxAes k, k0;
    st_unhex(key, "000102030405060708090a0b0c0d0e0f"); nx_aes_init(&k, key);   // FIPS-197 appendix C.1
    st_unhex(a, "00112233445566778899aabbccddeeff"); st_unhex(b, "69c4e0d86a7b0430d8cdb78070b4c55a");
    nx_aes_encrypt_block(&k, a, c); if (memcmp(c, b, 16)) st_fail(which, 197, 0, 0);
    nx_aes_decrypt_block(&k, b, c); if (memcmp(c, a, 16)) st_fail(which, 197, 1, 0);
    st_unhex(key, "2b7e151628aed2a6abf7158809cf4f3c"); nx_aes_init(&k, key);   // SP 800-38A: the key and plaintext of every AES-128 example
    st_unhex(a, "6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e5130c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    st_unhex(b, "3ad77bb40d7a3660a89ecaf32466ef97f5d3d58503b9699de785895a96fdbaaf43b1cd7f598ece23881b00e3ed0306887b0c785e27e8ad3f8223207104725dd4");   // F.1.1 / F.1.2 ECB
    for (int i = 0; i < 4; i++) { nx_aes_encrypt_block(&k, a + 16 * i, c + 16 * i); } if (memcmp(c, b, 64)) st_fail(which, 381, 0, 0);
    nx_aes_ecb_decrypt(&k, b, c, 64); if (memcmp(c, a, 64)) st_fail(which, 381, 1, 0);
    st_unhex(b, "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee");   // F.5.1 / F.5.2 CTR
    st_unhex(ctr, "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"); nx_aes_ctr(&k, ctr, a, c, 64); if (memcmp(c, b, 64)) st_fail(which, 385, 1, 0);
    { uint8_t want[16]; st_unhex(want, "f0f1f2f3f4f5f6f7f8f9fafbfcfdff03"); if (memcmp(ctr, want, 16)) st_fail(which, 385, 3, 0); }
    st_unhex(ctr, "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"); nx_aes_ctr(&k, ctr, b, c, 64); if (memcmp(c, a, 64)) st_fail(which, 385, 2, 0);
    st_unhex(ctr, "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff"); memcpy(c, a, 64); nx_aes_ctr(&k, ctr, c, c, 55); if (memcmp(c, b, 55) || memcmp(c + 55, a + 55, 9)) st_fail(which, 385, 4, 0);   // a partial last block, in place
    memset(key, 0, 16); nx_aes_init(&k0, key); memset(a, 0, 32);   // IEEE 1619 XTS-AES-128 vector 1 (sector 0: the same tweak in either byte order)
    st_unhex(b, "917cf69ebd68b2ec9b9fe9a3eadda692cd43d2f59598ed858c02c2652fbf922e");
    nx_aes_xts_decrypt(&k0, &k0, 0, 32, b, c, 32); if (memcmp(c, a, 32)) st_fail(which, 1619, 0, 0);
}

typedef struct StKeys { NxAes hw, pt; } StKeys;   // one key set up for the hardware cipher and for the portable one
static void st_keys(StKeys *k) { uint8_t key[16]; st_fill(key, 16); nx_aes_test_portable(0); nx_aes_init(&k->hw, key); nx_aes_test_portable(1); nx_aes_init(&k->pt, key); nx_aes_test_portable(0); }

// One CTR call of n bytes under the hardware cipher, input at an address ia bytes and output oa bytes off the allocator's alignment (in place: both at oa),
// against the portable cipher on the same bytes.
static void st_ctr_case(const StKeys *k, const uint8_t ctr0[16], size_t n, unsigned ia, unsigned oa, int inplace) {
    uint8_t *data = st_alloc(n), *want = st_alloc(n), *ibuf = st_alloc(ia + n), *obuf = st_alloc(oa + n + ST_GUARD), c_hw[16], c_pt[16];
    st_fill(data, n); memcpy(c_hw, ctr0, 16); memcpy(c_pt, ctr0, 16);
    uint8_t *out = obuf + oa; const uint8_t *in = ibuf + ia;
    memset(out + n, 0xA5, ST_GUARD);
    if (inplace) { memcpy(out, data, n); in = out; } else { memcpy(ibuf + ia, data, n); memset(out, 0, n); }
    nx_aes_test_portable(0); nx_aes_ctr(&k->hw, c_hw, in, out, n);
    nx_aes_test_portable(1); nx_aes_ctr(&k->pt, c_pt, data, want, n);
    nx_aes_test_portable(0);
    if (memcmp(out, want, n)) st_fail("ctr bytes differ (length, input offset, output offset)", n, ia, oa);
    if (memcmp(c_hw, c_pt, 16)) st_fail("ctr counter differs after the call (length, in place, -)", n, (size_t)inplace, 0);
    for (int i = 0; i < ST_GUARD; i++) if (out[n + i] != 0xA5) { st_fail("ctr wrote past the output (length, byte, -)", n, (size_t)i, 0); break; }
    if (!inplace && memcmp(ibuf + ia, data, n)) st_fail("ctr changed its input (length, -, -)", n, 0, 0);
    free(data); free(want); free(ibuf); free(obuf);
}
// A counter d blocks before the low 64 bits wrap; the high half random, or all ones (then the whole 128 bits wrap)
static void st_ctr_near_wrap(uint8_t c[16], unsigned d, int high_ones) { st_fill(c, 8); if (high_ones) memset(c, 0xFF, 8); uint64_t lo = UINT64_MAX - d; for (int i = 0; i < 8; i++) c[15 - i] = (uint8_t)(lo >> (8 * i)); }

static int nx_selftest_crypto(unsigned rounds) {
    int bad0 = st_bad, bad; unsigned long cases;
    nx_aes_test_portable(0); const char *hw = nx_aes_backend(); int have_hw = strcmp(hw, "portable") != 0;
    printf("AES backend: %s%s\n", hw, have_hw ? "" : " (no hardware cipher here: the comparisons below are the portable cipher against itself)");
    bad = st_bad; st_kats("known answer, hardware cipher (standard, check, -)"); st_group("known answers, hardware cipher", 9, bad);
    bad = st_bad; nx_aes_test_portable(1); st_kats("known answer, portable cipher (standard, check, -)"); nx_aes_test_portable(0); st_group("known answers, portable cipher", 9, bad);

    StKeys k; st_keys(&k);
    // the portable CTR itself against the definition (single block encryptions), so the reference of everything below is checked too
    bad = st_bad; cases = 0;
    for (size_t n = 0; n <= 300; n++) for (unsigned w = 0; w < 4; w++) {
        uint8_t c1[16], c2[16], *d = st_alloc(n), *o1 = st_alloc(n), *o2 = st_alloc(n); st_fill(d, n);
        if (w < 2) st_ctr_near_wrap(c1, (unsigned)(st_rnd() % 18), (int)w); else st_fill(c1, 16);
        memcpy(c2, c1, 16);
        nx_aes_test_portable(1); nx_aes_ctr(&k.pt, c1, d, o1, n); st_ctr_plain(&k.pt, c2, d, o2, n); nx_aes_test_portable(0);
        if (memcmp(o1, o2, n) || memcmp(c1, c2, 16)) st_fail("portable ctr against the definition (length, -, -)", n, 0, 0);
        free(d); free(o1); free(o2); cases++;
    }
    st_group("CTR, portable cipher against the definition, lengths 0..300", cases, bad);

    bad = st_bad; cases = 0;
    for (unsigned ci = 0; ci < 28; ci++) {
        uint8_t c[16];
        if (ci < 18) st_ctr_near_wrap(c, ci, 0);   // low half FF..FF, FF..FE, ... FF..EE: the wrap falls on every position of an 8-block step
        else if (ci < 25) { static const unsigned d[7] = { 0, 1, 7, 8, 9, 15, 16 }; st_ctr_near_wrap(c, d[ci - 18], 1); }
        else st_fill(c, 16);
        for (size_t n = 0; n <= 300; n++) for (int inplace = 0; inplace < 2; inplace++) {
            unsigned ia = (unsigned)(st_rnd() % 16), oa = (unsigned)(st_rnd() % 16); if ((n + ci) % 5 == 0) ia = oa = 0;
            st_ctr_case(&k, c, n, ia, oa, inplace); cases++;
        }
    }
    st_group("CTR, hardware against portable, lengths 0..300, 28 counters (25 at a wrap), in place and not, unaligned", cases, bad);

    bad = st_bad; cases = 0;
    for (unsigned r = 0; r < rounds; r++) {
        uint8_t c[16]; size_t n; unsigned kind = (unsigned)(st_rnd() % 8);
        if (r < 4) n = (4u << 20) - (r == 1 ? 1 : r == 2 ? 16 : r == 3 ? 129 : 0);
        else if (kind < 2) n = (size_t)(st_rnd() % ((4u << 20) + 1));                 // anywhere up to 4 MB
        else n = (size_t)(st_rnd() % ((size_t)1 << (9 + st_rnd() % 12)));             // spread over the sizes: under 512 bytes to under 1 MB
        if (st_rnd() % 3 == 0) st_ctr_near_wrap(c, (unsigned)(st_rnd() % (n / 16 + 20)), (int)(st_rnd() & 1)); else st_fill(c, 16);   // a wrap somewhere inside the buffer
        st_ctr_case(&k, c, n, (unsigned)(st_rnd() % 16), (unsigned)(st_rnd() % 16), (int)(st_rnd() & 1)); cases++;
    }
    st_group("CTR, hardware against portable, random lengths up to 4 MB", cases, bad);

    // a stream decrypted in pieces (whole blocks each, the counter carried over; the last piece any length) against one portable call
    bad = st_bad; cases = 0;
    for (unsigned r = 0; r < 200; r++) {
        size_t n = (size_t)(st_rnd() % 70000), done = 0; uint8_t c0[16], c1[16], c2[16], *d = st_alloc(n), *o = st_alloc(n), *want = st_alloc(n);
        st_fill(d, n); if (r & 1) st_ctr_near_wrap(c0, (unsigned)(st_rnd() % 4000), (int)(r & 2)); else st_fill(c0, 16);
        memcpy(c1, c0, 16); memcpy(c2, c0, 16);
        nx_aes_test_portable(1); nx_aes_ctr(&k.pt, c2, d, want, n); nx_aes_test_portable(0);
        while (done < n) { size_t p = 16 * (size_t)(1 + st_rnd() % (st_rnd() % 4 ? 12 : 600)); if (p > n - done) p = n - done; nx_aes_ctr(&k.hw, c1, d + done, o + done, p); done += p; }
        if (memcmp(o, want, n) || memcmp(c1, c2, 16)) st_fail("ctr in pieces (length, -, -)", n, 0, 0);
        free(d); free(o); free(want); cases++;
    }
    st_group("CTR, hardware in pieces against one portable call", cases, bad);

    bad = st_bad; cases = 0;
    for (unsigned r = 0; r < 400; r++) {
        StKeys k1, k2; st_keys(&k1); st_keys(&k2);
        uint8_t x[16], e1[16], e2[16], d1[16], d2[16]; st_fill(x, 16);
        nx_aes_encrypt_block(&k1.hw, x, e1); nx_aes_decrypt_block(&k1.hw, x, d1);
        nx_aes_test_portable(1); nx_aes_encrypt_block(&k1.pt, x, e2); nx_aes_decrypt_block(&k1.pt, x, d2); nx_aes_test_portable(0);
        if (memcmp(e1, e2, 16) || memcmp(d1, d2, 16)) st_fail("single block (round, -, -)", r, 0, 0);
        nx_aes_decrypt_block(&k1.hw, e1, d1); if (memcmp(d1, x, 16)) st_fail("single block round trip (round, -, -)", r, 0, 0);
        size_t n = 16 * (size_t)(st_rnd() % 65); uint8_t *d = st_alloc(n), *o = st_alloc(n), *want = st_alloc(n); st_fill(d, n);   // ECB, in place every other round
        nx_aes_test_portable(1); nx_aes_ecb_decrypt(&k1.pt, d, want, n); nx_aes_test_portable(0);
        if (r & 1) { memcpy(o, d, n); nx_aes_ecb_decrypt(&k1.hw, o, o, n); } else nx_aes_ecb_decrypt(&k1.hw, d, o, n);
        if (memcmp(o, want, n)) st_fail("ecb (bytes, -, -)", n, 0, 0);
        free(d); free(o); free(want);
        size_t ss = r % 3 == 0 ? 0x20 : 0x200, sn = ss * (size_t)(1 + st_rnd() % 6); uint64_t sector = r % 4 == 0 ? st_rnd() : st_rnd() % 5;   // XTS, as the NCA headers use it and wider
        d = st_alloc(sn); o = st_alloc(sn); want = st_alloc(sn); st_fill(d, sn);
        nx_aes_test_portable(1); nx_aes_xts_decrypt(&k1.pt, &k2.pt, sector, ss, d, want, sn); nx_aes_test_portable(0);
        if (r & 1) { memcpy(o, d, sn); nx_aes_xts_decrypt(&k1.hw, &k2.hw, sector, ss, o, o, sn); } else nx_aes_xts_decrypt(&k1.hw, &k2.hw, sector, ss, d, o, sn);
        if (memcmp(o, want, sn)) st_fail("xts (bytes, sector size, -)", sn, ss, 0);
        free(d); free(o); free(want); cases++;
    }
    st_group("single blocks, ECB and XTS, hardware against portable", cases, bad);
    nx_aes_test_portable(0);
    return st_bad - bad0;
}

// ---------------------------------------------------------------- LZ4
// The decoder nx_lz4.c had before it copied in 16-byte pieces: one memcpy per run, overlapping matches byte by byte. Kept word for word as the reference.
static long st_lz4_ref(const uint8_t *src, size_t slen, uint8_t *dst, size_t dlen) {
    size_t ip = 0, op = 0;
    while (ip < slen) {
        uint8_t tok = src[ip++]; size_t lit = tok >> 4, ml = tok & 15;
        if (lit == 15) { uint8_t b; do { if (ip >= slen) return -1; b = src[ip++]; lit += b; } while (b == 255); }
        if (lit > slen - ip || lit > dlen - op) return -1;
        memcpy(dst + op, src + ip, lit); ip += lit; op += lit;
        if (ip == slen) break;   // the block ends with literals
        if (ip + 2 > slen) return -1;
        size_t off = (size_t)src[ip] | (size_t)src[ip + 1] << 8; ip += 2;
        if (!off || off > op) return -1;
        if (ml == 15) { uint8_t b; do { if (ip >= slen) return -1; b = src[ip++]; ml += b; } while (b == 255); }
        ml += 4;
        if (ml > dlen - op) return -1;
        const uint8_t *m = dst + op - off;
        if (off >= ml) memcpy(dst + op, m, ml);
        else for (size_t i = 0; i < ml; i++) dst[op + i] = m[i];   // overlapping: a repeated pattern of off bytes
        op += ml;
    }
    return (long)op;
}

// One block with an output limit of dlen through both decoders: the same result (a length or -1) and, when a length, the same bytes; nothing written
// past dlen. The input and one of the two outputs are exact-size allocations (see the top of the file). Returns the reference's result.
static long st_lz4_case(const uint8_t *src, size_t slen, size_t dlen) {
    uint8_t *s = st_alloc(slen), *ref = st_alloc(dlen), *exact = st_alloc(dlen), *guarded = st_alloc(dlen + ST_GUARD);
    memcpy(s, src, slen); memset(guarded + dlen, 0x5A, ST_GUARD);
    long want = st_lz4_ref(s, slen, ref, dlen), got = nx_lz4_decompress(s, slen, exact, dlen), got2 = nx_lz4_decompress(s, slen, guarded, dlen);
    if (got != want || got2 != want) st_fail("lz4 result differs from the reference (input length, output limit, reference result + 1)", slen, dlen, (size_t)(want + 1));
    else if (want > 0 && (memcmp(exact, ref, (size_t)want) || memcmp(guarded, ref, (size_t)want))) st_fail("lz4 bytes differ from the reference (input length, output limit, length)", slen, dlen, (size_t)want);
    for (int i = 0; i < ST_GUARD; i++) if (guarded[dlen + i] != 0x5A) { st_fail("lz4 wrote past the output (input length, output limit, byte)", slen, dlen, (size_t)i); break; }
    if (memcmp(s, src, slen)) st_fail("lz4 changed its input (input length, -, -)", slen, 0, 0);
    free(s); free(ref); free(exact); free(guarded);
    return want;
}
// The same block cut short at `cuts` places (every place when cuts is 0), with `flips` single bytes changed, and with too little and spare output
static void st_lz4_abuse(const uint8_t *src, size_t slen, size_t dlen, unsigned cuts, unsigned flips, unsigned long *cases) {
    static const long delta[] = { -1, 1, 15, 16, 17, 100, -15, -16, -33 };
    for (unsigned i = 0; i < sizeof delta / sizeof *delta; i++) if (delta[i] > 0 || dlen >= (size_t)-delta[i]) { st_lz4_case(src, slen, (size_t)((long)dlen + delta[i])); ++*cases; }
    st_lz4_case(src, slen, dlen / 2); st_lz4_case(src, slen, 0); *cases += 2;
    if (!cuts) for (size_t n = 0; n < slen; n++) { st_lz4_case(src, n, dlen); st_lz4_case(src, n, dlen + 40); *cases += 2; }
    else for (unsigned i = 0; i < cuts && slen; i++) { st_lz4_case(src, (size_t)(st_rnd() % slen), dlen + (i & 1 ? 40 : 0)); ++*cases; }
    if (!slen) return;
    uint8_t *m = st_alloc(slen);
    for (unsigned i = 0; i < flips; i++) {
        memcpy(m, src, slen); size_t at = (size_t)(st_rnd() % slen); uint8_t v = (uint8_t)st_rnd();
        m[at] = i % 3 == 0 ? (uint8_t)(m[at] ^ (1u << (v & 7))) : i % 3 == 1 ? v : (uint8_t)(v & 1 ? 0xFF : 0x00);
        st_lz4_case(m, slen, dlen + (i & 1 ? 40 : 0)); ++*cases;
    }
    free(m);
}

static size_t st_lz4_put_len(uint8_t *p, size_t v) { size_t n = 0; while (v >= 255) { p[n++] = 255; v -= 255; } p[n++] = (uint8_t)v; return n; }
static size_t st_lz4_pick_run(void) {   // a run length that lands on every way the format writes one
    switch (st_rnd() % 12) {
    case 0: return 0; case 1: return 15; case 2: return 16; case 3: return 14 + st_rnd() % 4; case 4: return 15 + 255 * (st_rnd() % 3) + st_rnd() % 2;
    case 5: return 17 + st_rnd() % 50; case 6: return 200 + st_rnd() % 2000; case 7: return 31 + st_rnd() % 3;
    default: return st_rnd() % 15;
    }
}
// A valid block made of random sequences; returns its length (cap must be large: 64 sequences of up to a few KB each)
static size_t st_lz4_make(uint8_t *out, size_t *decoded) {
    size_t ip = 0, op = 0; unsigned nseq = 1 + (unsigned)(st_rnd() % 64), dull = (unsigned)(st_rnd() % 3);
    for (unsigned s = 0; s < nseq; s++) {
        int last = s == nseq - 1; size_t lit = st_lz4_pick_run(), ml = 0, off = 0;
        if (!op && !lit && !last) lit = 1;   // a match needs something before it
        if (!last) {
            ml = st_lz4_pick_run();   // on top of the minimum of 4
            size_t have = op + lit, max = have < 65535 ? have : 65535;
            switch (st_rnd() % 8) { case 0: off = 1; break; case 1: off = 1 + st_rnd() % 8; break; case 2: off = 8 + st_rnd() % 12; break; case 3: off = max; break; case 4: off = 16; break; default: off = 1 + st_rnd() % max; }
            if (off > max) off = max;
        }
        out[ip++] = (uint8_t)((lit < 15 ? lit : 15) << 4 | (last ? 0 : ml < 15 ? ml : 15));
        if (lit >= 15) ip += st_lz4_put_len(out + ip, lit - 15);
        for (size_t i = 0; i < lit; i++) out[ip++] = dull ? (uint8_t)('a' + st_rnd() % 3) : (uint8_t)st_rnd();
        op += lit;
        if (last) break;
        out[ip++] = (uint8_t)off; out[ip++] = (uint8_t)(off >> 8);
        if (ml >= 15) ip += st_lz4_put_len(out + ip, ml - 15);
        op += ml + 4;
    }
    *decoded = op; return ip;
}

static int nx_selftest_lz4(unsigned rounds) {
    int bad0 = st_bad, bad = st_bad; unsigned long cases = 0;
    // hand-made blocks: the input bytes, the output limit and the result the decoder must give (-1: rejected)
    static const struct { const char *what; uint8_t in[12]; size_t n, dlen; long want; } hand[] = {
        { "empty input", { 0 }, 0, 8, 0 },
        { "literals only", { 0x30, 'a', 'b', 'c' }, 4, 3, 3 },
        { "a match of 5 at offset 1, then the closing token", { 0x11, 'a', 0x01, 0x00, 0x00 }, 5, 6, 6 },
        { "truncated input: 5 literals announced, 3 present", { 0x50, 'a', 'b', 'c' }, 4, 64, -1 },
        { "truncated input: the literal length's extra byte is missing", { 0xF0 }, 1, 64, -1 },
        { "truncated input: the literal length's extra bytes do not end", { 0xF0, 0xFF, 0xFF }, 3, 2048, -1 },
        { "literal run past the input's end (long form)", { 0xF0, 0x05, 'a', 'b' }, 4, 64, -1 },
        { "literal run past the output's end", { 0x30, 'a', 'b', 'c' }, 4, 2, -1 },
        { "offset 0", { 0x10, 'a', 0x00, 0x00, 0x00 }, 5, 64, -1 },
        { "offset beyond the output's start", { 0x10, 'a', 0x02, 0x00, 0x00 }, 5, 64, -1 },
        { "offset beyond the output's start (far)", { 0x30, 'a', 'b', 'c', 0xFF, 0xFF, 0x00 }, 7, 64, -1 },
        { "truncated input: half an offset", { 0x10, 'a', 0x01 }, 3, 64, -1 },
        { "truncated input: the match length's extra byte is missing", { 0x1F, 'a', 0x01, 0x00 }, 4, 4096, -1 },
        { "truncated input: the match length's extra bytes do not end", { 0x1F, 'a', 0x01, 0x00, 0xFF }, 5, 4096, -1 },
        { "match past the output's end", { 0x14, 'a', 0x01, 0x00, 0x00 }, 5, 8, -1 },
        { "match past the output's end (long form: 274 bytes into 100)", { 0x1F, 'a', 0x01, 0x00, 0xFF, 0x00, 0x00 }, 7, 100, -1 },
        { "match ending exactly at the output's end", { 0x14, 'a', 0x01, 0x00, 0x00 }, 5, 9, 9 },
    };
    for (unsigned i = 0; i < sizeof hand / sizeof *hand; i++) {
        uint8_t *s = st_alloc(hand[i].n), *d = st_alloc(hand[i].dlen); memcpy(s, hand[i].in, hand[i].n);
        long got = nx_lz4_decompress(s, hand[i].n, d, hand[i].dlen);
        if (got != hand[i].want) { st_fail(hand[i].what, i, (size_t)(got + 1), (size_t)(hand[i].want + 1)); }
        free(s); free(d);
        st_lz4_case(hand[i].in, hand[i].n, hand[i].dlen); st_lz4_abuse(hand[i].in, hand[i].n, hand[i].dlen, 0, 64, &cases); cases += 2;
    }
    st_group("LZ4, hand-made blocks (valid, truncated, offset 0, offset before the start, runs past either end)", cases, bad);

    bad = st_bad; cases = 0; uint8_t *blk = st_alloc(64 * 12000 + 64);
    for (unsigned r = 0; r < rounds; r++) {
        size_t dec, n = st_lz4_make(blk, &dec);
        if (st_lz4_case(blk, n, dec) != (long)dec) st_fail("generated block does not decode to its length (input length, length, -)", n, dec, 0);
        cases++; st_lz4_abuse(blk, n, dec, n <= 300 ? 0 : 24, 24, &cases);
    }
    st_group("LZ4, generated blocks: exact, cut short, bytes changed, too little and spare output", cases, bad);

    bad = st_bad; cases = 0;
    for (unsigned r = 0; r < rounds; r++) {   // noise, some of it biased towards small tokens so decoding gets past the first sequence
        size_t n = (size_t)(st_rnd() % 400); st_fill(blk, n);
        if (r & 1) for (size_t i = 0; i < n; i += 1 + (size_t)(st_rnd() % 6)) blk[i] = (uint8_t)(st_rnd() % 3 ? blk[i] & 0x33 : 0);
        st_lz4_case(blk, n, (size_t)(st_rnd() % 4096)); cases++;
    }
    st_group("LZ4, random bytes as input", cases, bad);
    free(blk);
    return st_bad - bad0;
}
#endif
