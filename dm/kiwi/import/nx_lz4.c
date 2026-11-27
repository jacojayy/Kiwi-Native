// LZ4 block decoder for libnximport (the compressed NCA sections' entries, nx_nca.c). The block format only, no frame:
// sequences of a token (literal length in the high nibble, match length - 4 in the low one; 15 = more bytes follow, each added, until one is not 255),
// the literals, a 2-byte little-endian match offset (1..65535, into what was decoded) and the match length's extra bytes; the last sequence is literals only.
//
// Most runs are under 32 bytes, where a memcpy call with a variable length costs more than the copy. So a run is copied in fixed 16-byte pieces that may
// go past its end (the next sequence overwrites the excess), but only when the piece stays inside both buffers: every such copy is guarded by the room
// left to the end of src and of dst, and the runs near the ends take the exact copies. Nothing is read or written outside [src, src + slen) and
// [dst, dst + dlen), and dst needs no spare room: the caller decodes straight into buffers of the exact size.
// Accepted and rejected inputs, the returned length and the first `returned` bytes of dst are those of the plain decoder this replaces
// (tests/nx_selftest.h keeps it as the reference); bytes of dst after the returned length may have been written.
#include "nx_internal.h"
#include <string.h>

#define COPY16(d, s) memcpy((d), (s), 16)   // a fixed size: the compiler makes it one unaligned load and store

// A multiple of the offset that is at least 16, for offsets 1..15: data that repeats every `off` bytes also repeats every STEP[off] bytes,
// so an overlapping match can be continued in 16-byte pieces from that far back.
static const uint8_t STEP[16] = { 0, 16, 16, 18, 16, 20, 18, 21, 16, 18, 20, 22, 24, 26, 28, 30 };

long nx_lz4_decompress(const uint8_t *src, size_t slen, uint8_t *dst, size_t dlen) {
    const uint8_t *ip = src, *const iend = src + slen;
    uint8_t *op = dst, *const oend = dst + dlen;
    while (ip < iend) {
        unsigned tok = *ip++; size_t lit = tok >> 4, ml = tok & 15;
        if (lit == 15) { uint8_t b; do { if (ip >= iend) return -1; b = *ip++; lit += b; } while (b == 255); }
        size_t in_left = (size_t)(iend - ip), out_left = (size_t)(oend - op);
        if (lit > in_left || lit > out_left) return -1;
        if (lit <= 16 && in_left >= 16 && out_left >= 16) COPY16(op, ip);
        else if (lit <= 32 && in_left >= 32 && out_left >= 32) { COPY16(op, ip); COPY16(op + 16, ip + 16); }
        else memcpy(op, ip, lit);
        ip += lit; op += lit;
        if (ip == iend) break;   // the block ends with literals
        if (iend - ip < 2) return -1;
        size_t off = (size_t)ip[0] | (size_t)ip[1] << 8; ip += 2;
        if (!off || off > (size_t)(op - dst)) return -1;
        if (ml == 15) { uint8_t b; do { if (ip >= iend) return -1; b = *ip++; ml += b; } while (b == 255); }
        ml += 4;
        out_left = (size_t)(oend - op);
        if (ml > out_left) return -1;
        const uint8_t *m = op - off;
        if (out_left - ml >= 15) {   // room for whole 16-byte pieces: the last one ends at most 15 bytes after the match
            if (off >= 16) { for (size_t i = 0; i < ml; i += 16) COPY16(op + i, m + i); }   // a piece never overlaps its source
            else {   // overlapping: a repeated pattern of off bytes. The first 16 bytes one by one, the rest from STEP[off] bytes back (written by then)
                for (size_t i = 0; i < 16; i++) op[i] = m[i];
                const uint8_t *m2 = op - STEP[off];
                for (size_t i = 16; i < ml; i += 16) COPY16(op + i, m2 + i);
            }
        } else if (off >= ml) memcpy(op, m, ml);
        else for (size_t i = 0; i < ml; i++) op[i] = m[i];
        op += ml;
    }
    return (long)(op - dst);
}
