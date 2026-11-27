package dev.peanuts.pack;

import java.io.BufferedInputStream;
import java.io.EOFException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.Locale;

/**
 * &lt;module&gt;.orig.bin for a pack made before the rewriter wrote it (dm/tools/patch_nso.py since 2026-11-14), without a rebuild: the bytes the game's own
 * module has at the runs of &lt;module&gt;.changed.tsv, one run after the other in the table's order. The companion runtime reads them in place of the rewritten
 * words (dm/peanuts/peanuts.c rt_orig_load), and without the file it refuses every patch of a package's load plan (rt_code_rewritten), so a companion that
 * calls the game's own functions falls back to pressing buttons. The file made here is the one a rebuild of the same pack would write.
 * <p>
 * Nothing is written unless the pack's code is made from this very module: the table's build ID is the module's, the module's code matches its SHA-256 where
 * the header asks for the check, and outside the table's runs every byte of &lt;module&gt;.patched.bin's code is the module's own. The file is written as
 * &lt;module&gt;.orig.bin.part and renamed when it is complete; one that is there is never written again, and no other file of the pack is touched.
 * <p>
 * The module is read as a stream and its code decompressed through a 64 KiB window (an LZ4 match reaches no farther back), so a main of 100 MiB of code costs
 * the heap a few MiB. The same checks as peanuts/scripts/pack_origfill.py, which peanuts/tests/test_origfill.py compares this with. No Android classes.
 */
public final class OrigFill {
    private OrigFill() {}

    /** The unfinished file's suffix; the runtime waits for a fresh one before it starts (peanuts.c rt_orig_pending) */
    public static final String PART = ".orig.bin.part";

    /** Why a pack does not get the file; nothing was written */
    public static final class Refused extends IOException {
        public Refused(String why) { super(why); }
    }

    /** A &lt;module&gt;.changed.tsv: its build ID (lower case) and its runs */
    static final class Table {
        String buildId;
        long[] off = new long[1024];
        int[] len = new int[1024];
        int n;
        long bytes;
    }

    static Table table(File f, String name) throws IOException {
        Table t = new Table();
        long end = 0;
        int no = 0;
        for (String line : new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8).split("\n")) {
            no++;
            if (line.endsWith("\r")) line = line.substring(0, line.length() - 1);
            if (line.startsWith("# BUILD_ID")) {
                String[] c = line.split("\t");
                t.buildId = c.length > 1 ? c[1].trim().toLowerCase(Locale.ROOT) : "";
                continue;
            }
            if (line.trim().isEmpty() || line.startsWith("#")) continue;
            String[] c = line.split("\t");
            long off, len;
            try {
                if (c.length < 2) throw new NumberFormatException();
                off = hex(c[0]);
                len = hex(c[1]);
            } catch (NumberFormatException e) {
                throw new Refused(name + ".changed.tsv line " + no + " is not an offset and a length");
            }
            if (len <= 0 || len > Integer.MAX_VALUE || off % 4 != 0 || len % 4 != 0) throw new Refused(name + ".changed.tsv line " + no + " is not whole instructions (" + x(off) + ", " + x(len) + ")");
            if (off < end) throw new Refused(name + ".changed.tsv line " + no + " starts at " + x(off) + ", before the end of the run above it (" + x(end) + ")");
            if (t.n == t.off.length) { t.off = Arrays.copyOf(t.off, t.n * 2); t.len = Arrays.copyOf(t.len, t.n * 2); }
            t.off[t.n] = off; t.len[t.n++] = (int) len;
            t.bytes += len;
            end = off + len;
        }
        if (t.buildId == null || t.buildId.isEmpty()) throw new Refused(name + ".changed.tsv has no BUILD_ID line: the build its runs belong to is unknown");
        if (t.bytes > Integer.MAX_VALUE - 16) throw new Refused(name + ".changed.tsv lists " + t.bytes + " bytes: not a table of the rewriter's");
        return t;
    }

    private static long hex(String s) {
        s = s.trim();
        if (s.startsWith("0x") || s.startsWith("0X")) s = s.substring(2);
        return Long.parseLong(s, 16);
    }

    private static String x(long v) { return "0x" + Long.toHexString(v); }

    /** Opens the game's module file from its first byte */
    public interface Module {
        InputStream open() throws IOException;
    }

    /**
     * Writes &lt;pack&gt;/&lt;name&gt;.orig.bin from the game's module and returns its length; -1 when the file is there already (nothing is written). Refused,
     * with nothing written, when the pack has no &lt;name&gt;.changed.tsv or &lt;name&gt;.patched.bin, the module is not an NSO this reads, or the pack's code is
     * not made from it. label names the module's file in the messages
     */
    public static long fill(File pack, String name, Module module, String label) throws IOException {
        File dst = new File(pack, name + ".orig.bin"), tsv = new File(pack, name + ".changed.tsv"), patched = new File(pack, name + ".patched.bin");
        if (dst.isFile()) return -1;
        if (!tsv.isFile()) throw new Refused("the pack has no " + tsv.getName() + " (made before the rewriter listed what it changed): only a rebuild makes " + dst.getName());
        if (!patched.isFile()) throw new Refused("the pack has no " + patched.getName());
        Table t = table(tsv, name);
        byte[] out;
        try (InputStream in = module.open(); InputStream pin = new BufferedInputStream(new FileInputStream(patched), 1 << 16)) {
            out = make(in, pin, patched.length(), t, name, label);
        }
        File tmp = new File(pack, name + PART);
        try {
            try (FileOutputStream o = new FileOutputStream(tmp)) {
                o.write(out);
                o.getFD().sync();
            }
            if (dst.isFile() || !tmp.renameTo(dst)) throw new IOException(dst.isFile() ? "it appeared meanwhile" : "the rename failed");
        } catch (IOException e) {
            tmp.delete();
            if (dst.isFile()) return -1;   // another start wrote it meanwhile: left as it is
            throw new Refused(dst.getName() + " cannot be written (" + e.getMessage() + ")");
        }
        return out.length;
    }

    /** The file's content: the module's code (in, an NSO from its first byte) at the table's runs, checked against the pack's code (patched, plen bytes) */
    static byte[] make(InputStream in, InputStream patched, long plen, Table t, String name, String label) throws IOException {
        byte[] h = new byte[0x100];
        if (readFully(in, h, 0, h.length) < h.length || h[0] != 'N' || h[1] != 'S' || h[2] != 'O' || h[3] != '0') throw new Refused(label + " is not an NSO");
        long flags = u32(h, 0xc), fo = u32(h, 0x10), mo = u32(h, 0x14), size = u32(h, 0x18), stored = (flags & 1) != 0 ? u32(h, 0x60) : size;
        StringBuilder id = new StringBuilder();
        for (int i = 0x40; i < 0x50; i++) id.append(String.format(Locale.ROOT, "%02x", h[i] & 0xff));
        if (!id.toString().equals(t.buildId))
            throw new Refused("the pack's " + name + " is of build " + t.buildId + ", the game's file is " + id + ": another version of the game than the pack was built from");
        if (mo != 0 || size > Integer.MAX_VALUE || fo < 0x100 || stored > size) throw new Refused(label + ": the NSO's code segment is not one this reads");
        if ((flags & 1) != 0 && (flags & 0x80) != 0)
            throw new Refused(label + " stores its code in the newer compression (ZBIC), which the app reads only while it builds: only a rebuild makes " + name + ".orig.bin");
        if (t.n > 0 && t.off[t.n - 1] + t.len[t.n - 1] > size)
            throw new Refused(name + ".changed.tsv names " + x(t.off[t.n - 1]) + "+" + x(t.len[t.n - 1]) + ", past the end of the code (" + x(size) + ")");
        if (plen < size) throw new Refused(name + ".patched.bin has " + x(plen) + " bytes, less than the code (" + x(size) + "): the pack is damaged");
        skipFully(in, fo - 0x100, label);
        Check c = new Check(patched, t, name);
        if ((flags & 1) != 0) lz4(in, stored, size, c, label);
        else {
            byte[] b = new byte[1 << 16];
            for (long at = 0; at < size; ) {
                int k = (int) Math.min(b.length, size - at);
                if (readFully(in, b, 0, k) < k) throw new Refused(label + " ends inside its code: the file is cut short or damaged");
                c.take(b, 0, k);
                at += k;
            }
        }
        if (c.at != size) throw new Refused(label + ": the code decompresses to " + x(c.at) + " bytes, not the " + x(size) + " of the header: the file is damaged");
        if ((flags & 8) != 0 && !Arrays.equals(c.sha.digest(), Arrays.copyOfRange(h, 0xa0, 0xc0)))
            throw new Refused(label + ": the code does not match its SHA-256 in the header: the file is damaged");
        return c.out;
    }

    /** Takes the module's code in order: hashes it, compares it with the pack's code outside the runs, and keeps the runs' bytes */
    private static final class Check {
        final InputStream patched;
        final Table t;
        final String name;
        final MessageDigest sha;
        final byte[] out;
        byte[] p = new byte[1 << 16];
        long at;      // code bytes taken
        int run, pos; // the first run that ends after at; bytes kept

        Check(InputStream patched, Table t, String name) {
            this.patched = patched; this.t = t; this.name = name;
            out = new byte[(int) t.bytes];
            try { sha = MessageDigest.getInstance("SHA-256"); } catch (NoSuchAlgorithmException e) { throw new IllegalStateException(e); }
        }

        void take(byte[] b, int off, int len) throws IOException {
            sha.update(b, off, len);
            if (p.length < len) p = new byte[len];
            if (readFully(patched, p, 0, len) < len) throw new Refused(name + ".patched.bin ends inside the code: the pack is damaged");
            long end = at + len;
            for (long a = at; a < end; ) {
                while (run < t.n && t.off[run] + t.len[run] <= a) run++;
                long rs = run < t.n ? t.off[run] : Long.MAX_VALUE, re = run < t.n ? rs + t.len[run] : Long.MAX_VALUE;
                if (a < rs) {   // outside the runs: the pack's code must be the file's
                    int n = (int) (Math.min(rs, end) - a), bo = off + (int) (a - at), po = (int) (a - at);
                    if (!ByteBuffer.wrap(b, bo, n).equals(ByteBuffer.wrap(p, po, n))) {
                        int k = 0;
                        while (b[bo + k] == p[po + k]) k++;
                        throw new Refused(name + ".patched.bin differs from the game's file at " + x((a + k) & ~3L) + ", outside the runs of " + name
                                + ".changed.tsv: the pack's code is not made from this file (a mod's module, or a damaged pack)");
                    }
                    a += n;
                } else {   // inside a run: the file's bytes are kept
                    int n = (int) (Math.min(re, end) - a);
                    System.arraycopy(b, off + (int) (a - at), out, pos, n);
                    pos += n;
                    a += n;
                }
            }
            at = end;
        }
    }

    private static final int WINDOW = 1 << 16, CAP = 1 << 21;

    /**
     * An LZ4 block of stored bytes from in, decompressed into c piece by piece: at most size bytes (more is a damaged file). The last 64 KiB stay in the buffer,
     * which is as far back as a match reaches
     */
    private static void lz4(InputStream raw, long stored, long size, Check c, String label) throws IOException {
        In in = new In(raw, stored, label);
        byte[] w = new byte[CAP];
        int pos = 0, sent = 0;
        long total = 0;
        while (in.left > 0) {
            int token = in.read();
            long lit = token >>> 4;
            if (lit == 15) { int b; do { b = in.read(); lit += b; } while (b == 255); }
            total += lit;
            if (total > size) throw new Refused(label + ": the code decompresses to more than the " + x(size) + " bytes of the header: the file is damaged");
            while (lit > 0) {
                if (pos == CAP) { c.take(w, sent, pos - sent); System.arraycopy(w, CAP - WINDOW, w, 0, WINDOW); pos = sent = WINDOW; }
                int k = (int) Math.min(lit, CAP - pos);
                in.read(w, pos, k);
                pos += k;
                lit -= k;
            }
            if (in.left == 0) break;   // the last sequence has only literals
            int d = in.read() | in.read() << 8;
            long m = (token & 15) + 4;
            if ((token & 15) == 15) { int b; do { b = in.read(); m += b; } while (b == 255); }
            if (d == 0 || d > Math.min(total, WINDOW - 1)) throw new Refused(label + ": the code does not decompress (a match before its start): the file is damaged");
            total += m;
            if (total > size) throw new Refused(label + ": the code decompresses to more than the " + x(size) + " bytes of the header: the file is damaged");
            while (m > 0) {
                if (pos == CAP) { c.take(w, sent, pos - sent); System.arraycopy(w, CAP - WINDOW, w, 0, WINDOW); pos = sent = WINDOW; }
                int k = (int) Math.min(m, CAP - pos);
                if (d >= k) System.arraycopy(w, pos - d, w, pos, k);   // no overlap within this piece
                else for (int i = 0; i < k; i++) w[pos + i] = w[pos - d + i];
                pos += k;
                m -= k;
            }
        }
        c.take(w, sent, pos - sent);
    }

    /** The stored bytes of a segment: a buffered reader that ends where the segment ends */
    private static final class In {
        final InputStream in;
        final String label;
        final byte[] buf = new byte[1 << 16];
        int at, end;
        long left;   // bytes of the segment not handed out yet

        In(InputStream in, long stored, String label) { this.in = in; this.left = stored; this.label = label; }

        private void more() throws IOException {
            if (left <= 0) throw new Refused(label + ": the code does not decompress (its stored bytes end inside a sequence): the file is damaged");
            at = 0;
            end = in.read(buf, 0, (int) Math.min(buf.length, left));
            if (end <= 0) throw new Refused(label + " ends inside its code: the file is cut short or damaged");
        }

        int read() throws IOException {
            if (at == end) more();
            left--;
            return buf[at++] & 0xff;
        }

        void read(byte[] b, int off, int n) throws IOException {
            while (n > 0) {
                if (at == end) more();
                int k = Math.min(n, end - at);
                System.arraycopy(buf, at, b, off, k);
                at += k; off += k; n -= k; left -= k;
            }
        }
    }

    private static int readFully(InputStream in, byte[] b, int off, int n) throws IOException {
        int got = 0;
        while (got < n) {
            int k = in.read(b, off + got, n - got);
            if (k < 0) break;
            got += k;
        }
        return got;
    }

    private static void skipFully(InputStream in, long n, String label) throws IOException {
        byte[] b = null;
        while (n > 0) {
            long k = in.skip(n);
            if (k <= 0) {   // a stream that does not skip: read on
                if (b == null) b = new byte[1 << 14];
                k = in.read(b, 0, (int) Math.min(b.length, n));
                if (k < 0) throw new EOFException(label + " ends before its code: the file is cut short or damaged");
            }
            n -= k;
        }
    }

    private static long u32(byte[] b, int o) { return (b[o] & 0xffL) | (b[o + 1] & 0xffL) << 8 | (b[o + 2] & 0xffL) << 16 | (b[o + 3] & 0xffL) << 24; }
}
