// PNRT: a package's guest helper on this host.
//
// A companion package may carry a load plan (dualscreen/guest/<build>/load-plan.json, mods/mod_load_plan.h): a mailbox the package's
// native module and its guest helper share, a few instruction patches of main, and (format 2) the helper's own code, which a patched
// call of the game then runs on the game's thread. On Eden the loader applies the plan while it maps main. Here main already runs,
// rewritten for this host, when the package is found, so the plan is applied to the running process:
//
//   1. the plan is read and checked by the runtime's own reader (ReadModLoadPlan: title, build, layout, sizes);
//   2. the host gives one block within a branch's reach of main (the unused end of main's patch area, or free pages beside it):
//      the helper's code, then stubs (below), then the mailbox;
//   3. the code and the patches are linked against that block as ModLoadPlan::Apply links them (the same five relocation kinds and
//      checks; Apply itself wants the code above main in one image, as Eden's loader lays it out), and each patch is checked
//      against the bytes main's file has there (the host reads main as its file has it);
//   4. the helper's uses of x18 are taken out (below);
//   5. the code is made executable and each patch is written into main, one aligned word at a time, while the game runs.
//
// A patch of an instruction this host's rewriter changed or moved is refused, as a cheat's is: the bytes there are not the ones that
// run. Without the rewriter's list of what it changed (a pack made before main.orig.bin) nothing is installed.
//
// What the plan enters of main is checked too (CheckMainTargets): every address in main's code that the helper or a patch branches
// to, or takes (a function it may call through a pointer). The rewriter leaves most of them as the file has them. One it changed in
// place still does its job there: a rewrite site's own word is a branch to a stub that runs the instruction and comes back, and the
// first word of a sequence it moved is the branch to the copy. The middle of a moved sequence is different: those words no longer
// run, the copy does, and code entered there runs instructions the rewriter meant to replace (an exclusive pair without its
// stand-in for x18). The rewriter's own check of branches between modules draws the same line (dm/tools/patch_nso.py, "the middle
// of a window or retry block"). The moved sequences are the "W" lines of the pack's main.sites.tsv (PnRtCode::sites_tsv); a retry
// block (an out-of-line loop back onto a window's store) is in no table of the pack and is not known here.
//
// The patches are written only with the game's threads stopped (PnRtGuest), and not while a thread may be executing inside a patch
// of more than one instruction or return into it: such a thread would run the old start of a sequence and the new end of it. The
// host looks at each stopped thread's pc, registers and stack for that (PnRtGuest::stopped_in). Eden has no such case, its loader
// patches main before the game runs. When the threads cannot be stopped so, after a number of tries over about two seconds, the
// plan is refused: nothing is written to main, the package runs without its helper and mailbox, and the log says so. It is never
// written beside the running game.
//
// x18. Horizon leaves x18 to the program, and a helper built for it uses the register as a scratch between calls. On Android x18 is
// the platform's shadow call stack pointer: Bionic pushes return addresses through it, also in a signal handler that interrupts the
// helper. The rewritten game keeps its x18 in the thread slot K18 and never touches the register; the helper gets the same treatment.
// Each instruction that names x18 becomes a branch to a stub that saves two free registers below sp, loads K18 into one of them,
// runs the instruction with that register in x18's place, stores it back and returns. Only instructions whose register fields are
// known and that can run from another address are taken (no branch, no pc-relative form, no write of sp); a helper with any other
// use of x18 is refused whole, with the offset in the log. K18 serves because x18 is caller-saved on Horizon: the helper expects
// nothing of it across a call, and neither does the game across the call the plan patched.
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <span>
#include <cstdlib>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <sys/mman.h>
#include <unistd.h>

#include "common/hex_util.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/mods/mod_load_plan.h"
#include "core/mods/mod_package_io.h"
#include "peanuts_helper.h"

namespace PeanutsHelper {
namespace {

constexpr u64 Page = 4096;
constexpr u32 FD = 1, FN = 2, FM = 4, FA = 8; // register fields at bits 4:0, 9:5, 20:16, 14:10
constexpr u32 StubWords = 8;

constexpr u32 Field(u32 w, u32 f) {
    return f == FD ? (w & 31) : f == FN ? ((w >> 5) & 31) : f == FM ? ((w >> 16) & 31) : ((w >> 10) & 31);
}
constexpr u32 WithField(u32 w, u32 f, u32 r) {
    const u32 lo = f == FD ? 0 : f == FN ? 5 : f == FM ? 16 : 10;
    return (w & ~(31u << lo)) | (r << lo);
}

/// The general-purpose register fields of an A64 instruction (`mask`), and whether it can run from another address with sp as it
/// is (`plain`). False for an encoding group this table does not know.
bool Fields(u32 w, u32& mask, bool& plain) {
    mask = 0;
    plain = true;
    const u32 op0 = (w >> 25) & 15;
    const u32 rd = w & 31, rn = (w >> 5) & 31;
    if ((op0 & 0b1110) == 0b1000) { // data processing, immediate
        switch ((w >> 23) & 7) {
        case 0:
        case 1: // adr, adrp
            mask = FD;
            plain = false;
            return true;
        case 2: // add / sub
            mask = FD | FN;
            plain = !(rd == 31 && !((w >> 29) & 1));
            return true;
        case 4: // logical
            mask = FD | FN;
            plain = !(rd == 31 && ((w >> 29) & 3) != 3);
            return true;
        case 5: // move wide
            mask = FD;
            return true;
        case 6: // bitfield
            mask = FD | FN;
            return true;
        case 7: // extract
            mask = FD | FN | FM;
            return true;
        default:
            return false;
        }
    }
    if ((op0 & 0b1110) == 0b1010) { // branches, exceptions, system
        if ((w & 0x7C000000) == 0x14000000 || (w & 0xFF000010) == 0x54000000 || (w & 0xFF000000) == 0xD4000000) {
            return true; // b, bl, b.cond, exception generation: no register
        }
        if ((w & 0x7E000000) == 0x34000000 || (w & 0x7E000000) == 0x36000000 || (w & 0xFFC00000) == 0xD5000000) {
            mask = FD; // cbz / cbnz, tbz / tbnz, system
            plain = false;
            return true;
        }
        if ((w & 0xFE000000) == 0xD6000000) { // br, blr, ret
            mask = FN;
            plain = false;
            return true;
        }
        return false;
    }
    if ((op0 & 0b0101) == 0b0100) { // loads and stores
        const bool v = (w >> 26) & 1;
        if ((w & 0x3F000000) == 0x08000000) { // exclusive
            mask = FD | FN | FM | FA;
            plain = false;
            return true;
        }
        if ((w & 0x3B000000) == 0x18000000) { // literal
            mask = v ? 0 : FD;
            plain = false;
            return true;
        }
        if ((w & 0x3A000000) == 0x28000000) { // pair
            mask = FN | (v ? 0 : FD | FA);
            const u32 idx = (w >> 23) & 3;
            plain = !((idx == 1 || idx == 3) && rn == 31);
            return true;
        }
        if ((w & 0x3A000000) == 0x38000000) { // one register
            mask = FN | (v ? 0 : FD);
            if (!v && (w >> 30) == 3 && ((w >> 22) & 3) == 2) {
                mask &= ~FD; // prfm: bits 4:0 name the prefetch, not a register
            }
            if ((w >> 24) & 1) {
                return true; // unsigned offset
            }
            if (!((w >> 21) & 1)) { // 9-bit offset: unscaled, post-index, unprivileged, pre-index
                const u32 mode = (w >> 10) & 3;
                plain = !((mode == 1 || mode == 3) && rn == 31);
                return true;
            }
            if (((w >> 10) & 3) == 2) { // register offset
                mask |= FM;
                return true;
            }
            return false; // atomic memory operations, pointer authentication
        }
        if ((w & 0x3E000000) == 0x0C000000) { // SIMD structures
            mask = FN | FM;
            plain = false;
            return true;
        }
        return false;
    }
    if ((op0 & 0b0111) == 0b0101) { // data processing, register
        if (!((w >> 28) & 1)) { // logical, add / sub (shifted or extended register)
            mask = FD | FN | FM;
            if (((w >> 24) & 1) && ((w >> 21) & 1)) {
                plain = !(rd == 31 && !((w >> 29) & 1));
            }
            return true;
        }
        const u32 op2 = (w >> 21) & 15;
        if (op2 >= 8) { // three sources
            mask = FD | FN | FM | FA;
            return true;
        }
        switch (op2) {
        case 0: // adc / sbc
        case 4: // conditional select
            mask = FD | FN | FM;
            return true;
        case 2: // conditional compare
            mask = FN | (((w >> 11) & 1) ? 0 : FM);
            return true;
        case 6: // one or two sources
            mask = ((w >> 30) & 1) ? FD | FN : FD | FN | FM;
            return true;
        default:
            return false;
        }
    }
    if ((op0 & 0b0111) == 0b0111) { // floating point and SIMD
        const bool to_int = (w & 0x5F200000) == 0x1E200000 && ((w >> 10) & 0x3F) == 0;
        const bool fixed = (w & 0x5F200000) == 0x1E000000;
        const bool copy = (w & 0x9FE08400) == 0x0E000400;
        if (to_int || fixed || copy) { // one side may be a general-purpose register
            mask = FD | FN;
            plain = false;
        }
        return true;
    }
    return false;
}

/// Takes the uses of x18 out of `code` (at `code_address`), with stubs at `stub_address`. The count of instructions moved, or -1
/// with `error` when one cannot be.
int RewriteX18(std::span<u32> code, const std::vector<u8>& is_code, u64 code_address, std::span<u32> stubs, u64 stub_address,
               u32 slot18, std::string& error) {
    int moved = 0;
    for (size_t i = 0; i < code.size(); ++i) {
        if (!is_code[i]) {
            continue; // the helper's own data (strings, tables): left as it is
        }
        const u32 w = code[i];
        u32 mask{};
        bool plain{};
        const bool raw18 = Field(w, FD) == 18 || Field(w, FN) == 18 || Field(w, FM) == 18 || Field(w, FA) == 18;
        if (!Fields(w, mask, plain)) {
            if (raw18) {
                error = fmt::format("the helper's word {:08x} at +{:#x} is of an instruction group this host does not know and may name x18",
                                    w, i * 4);
                return -1;
            }
            continue;
        }
        u32 uses = 0;
        for (const u32 f : {FD, FN, FM, FA}) {
            if ((mask & f) && Field(w, f) == 18) {
                uses |= f;
            }
        }
        if (!uses) {
            continue;
        }
        if (!plain) {
            error = fmt::format("the helper uses x18 at +{:#x} ({:08x}) in a branch, a pc-relative form or a write of sp, which this host cannot move",
                                i * 4, w);
            return -1;
        }
        if (static_cast<size_t>(moved + 1) * StubWords > stubs.size() || slot18 % 8 != 0 || slot18 / 8 > 0xFFF) {
            error = slot18 % 8 != 0 || slot18 / 8 > 0xFFF ? "the host's x18 slot is not one a load reaches"
                                                          : "the helper names x18 in more instructions than the stub page holds";
            return -1;
        }
        u32 t = 0, a = 0; // two registers the instruction does not name
        for (u32 r = 9; r <= 15 && !a; ++r) {
            if (Field(w, FD) == r || Field(w, FN) == r || Field(w, FM) == r || Field(w, FA) == r) {
                continue;
            }
            (t ? a : t) = r;
        }
        if (!t || !a) {
            error = fmt::format("no free register for the helper's x18 instruction at +{:#x}", i * 4);
            return -1;
        }
        u32 moved_w = w;
        for (const u32 f : {FD, FN, FM, FA}) {
            if (uses & f) {
                moved_w = WithField(moved_w, f, t);
            }
        }
        u32* const s = stubs.data() + static_cast<size_t>(moved) * StubWords;
        const u64 stub = stub_address + static_cast<u64>(moved) * StubWords * 4;
        const u64 site = code_address + i * 4;
        s[0] = 0xA93F0000u | (a << 10) | (31u << 5) | t;              // stp t, a, [sp, #-16]
        s[1] = 0xD53BD040u | a;                                       // mrs a, tpidr_el0
        s[2] = 0xF9400000u | (2u << 10) | (a << 5) | a;               // ldr a, [a, #0x10]   (this thread's slots)
        s[3] = 0xF9400000u | ((slot18 / 8) << 10) | (a << 5) | t;     // ldr t, [a, #K18]
        s[4] = moved_w;
        s[5] = 0xF9000000u | ((slot18 / 8) << 10) | (a << 5) | t;     // str t, [a, #K18]
        s[6] = 0xA97F0000u | (a << 10) | (31u << 5) | t;              // ldp t, a, [sp, #-16]
        s[7] = 0x14000000u | (static_cast<u32>((static_cast<s64>(site + 4) - static_cast<s64>(stub + 28)) >> 2) & 0x03FFFFFFu);
        code[i] = 0x14000000u | (static_cast<u32>((static_cast<s64>(stub) - static_cast<s64>(site)) >> 2) & 0x03FFFFFFu);
        ++moved;
    }
    return moved;
}

/// One relocation of the instruction at `address`, as ModLoadPlan::Apply does it. False when it does not fit.
bool Relocate(u32& instruction, u64 address, const Core::Mods::ModLoadPlan::Relocation& r, u64 mailbox, u64 mailbox_size,
              u64 main, u64 main_size, u64 code, u64 code_len) {
    using Kind = Core::Mods::ModLoadPlan::RelocationKind;
    using Target = Core::Mods::ModLoadPlan::RelocationTarget;
    if (r.addend < 0) {
        return false;
    }
    const u64 addend = static_cast<u64>(r.addend);
    const u64 base = r.target == Target::Mailbox ? mailbox : r.target == Target::Main ? main : code;
    const u64 limit = r.target == Target::Mailbox ? mailbox_size : r.target == Target::Main ? main_size : code_len;
    if ((r.target != Target::Mailbox && !code_len) || addend >= limit) {
        return false;
    }
    const u64 target = base + addend;
    switch (r.kind) {
    case Kind::AArch64Adrp: {
        if ((instruction & 0x9F000000) != 0x90000000) {
            return false;
        }
        const s64 pages = static_cast<s64>(target >> 12) - static_cast<s64>(address >> 12);
        if (pages < -(1LL << 20) || pages >= (1LL << 20)) {
            return false;
        }
        const u64 imm = static_cast<u64>(pages) & 0x1FFFFF;
        instruction = (instruction & 0x9F00001F) | static_cast<u32>((imm & 3) << 29) | static_cast<u32>((imm >> 2) << 5);
        return true;
    }
    case Kind::AArch64AddLo12:
        if ((instruction & 0xFFC00000) != 0x91000000) {
            return false;
        }
        instruction = (instruction & ~(0xFFFu << 10)) | (static_cast<u32>(target & 0xFFF) << 10);
        return true;
    case Kind::AArch64Ldr32Lo12:
    case Kind::AArch64Ldr64Lo12: {
        const bool wide = r.kind == Kind::AArch64Ldr64Lo12;
        const u32 scale = wide ? 3 : 2;
        if ((instruction & 0xFFC00000) != (wide ? 0xF9400000 : 0xB9400000) || (target & ((1u << scale) - 1)) != 0) {
            return false;
        }
        instruction = (instruction & ~(0xFFFu << 10)) | (static_cast<u32>((target & 0xFFF) >> scale) << 10);
        return true;
    }
    case Kind::AArch64Branch26: {
        if ((instruction & 0x7C000000) != 0x14000000 || target % 4 != 0 || address % 4 != 0) {
            return false;
        }
        const s64 delta = static_cast<s64>(target) - static_cast<s64>(address);
        if (delta < -(1LL << 27) || delta >= (1LL << 27)) {
            return false;
        }
        instruction = (instruction & 0xFC000000) | (static_cast<u32>(delta / 4) & 0x03FFFFFF);
        return true;
    }
    }
    return false;
}

/// `bytes` with `relocs` applied, as if at `address`. False when one does not fit.
bool Link(std::vector<u8>& bytes, u64 address, const std::vector<Core::Mods::ModLoadPlan::Relocation>& relocs, u64 mailbox,
          u64 mailbox_size, u64 main, u64 main_size, u64 code, u64 code_len) {
    for (const auto& r : relocs) {
        if (r.offset % 4 != 0 || r.offset > bytes.size() || bytes.size() - r.offset < 4) {
            return false;
        }
        u32 w{};
        std::memcpy(&w, bytes.data() + r.offset, 4);
        if (!Relocate(w, address + r.offset, r, mailbox, mailbox_size, main, main_size, code, code_len)) {
            return false;
        }
        std::memcpy(bytes.data() + r.offset, &w, 4);
    }
    return true;
}

constexpr s64 SignExtend(u64 v, int bits) {
    return static_cast<s64>(v << (64 - bits)) >> (64 - bits);
}

/// Marks the instructions reached from word `entry` by following fall-through and direct branches. `strict`: for an address that
/// only may be code (an adr target, a relocated address): false, with nothing marked, unless every word on the way is a known
/// instruction, stays inside the helper and each path ends in ret, br or b.
bool Walk(std::span<const u32> w, size_t entry, std::vector<u8>& mark, bool strict) {
    std::vector<u8> seen = mark;
    std::vector<size_t> todo{entry};
    size_t fresh = 0;
    while (!todo.empty()) {
        size_t i = todo.back();
        todo.pop_back();
        for (;;) {
            if (i >= w.size()) {
                if (strict) {
                    return false;
                }
                break;
            }
            if (seen[i]) {
                break;
            }
            const u32 x = w[i];
            u32 mask{};
            bool plain{};
            if (x == 0 || !Fields(x, mask, plain)) {
                if (strict) {
                    return false;
                }
                break;
            }
            seen[i] = 1;
            ++fresh;
            s64 target = 0;
            bool branches = true, falls = true;
            if ((x & 0x7C000000) == 0x14000000) { // b, bl
                target = static_cast<s64>(i) + SignExtend(x & 0x03FFFFFF, 26);
                falls = (x >> 31) != 0;
            } else if ((x & 0xFF000010) == 0x54000000 || (x & 0x7E000000) == 0x34000000) { // b.cond, cbz / cbnz
                target = static_cast<s64>(i) + SignExtend((x >> 5) & 0x7FFFF, 19);
            } else if ((x & 0x7E000000) == 0x36000000) { // tbz / tbnz
                target = static_cast<s64>(i) + SignExtend((x >> 5) & 0x3FFF, 14);
            } else {
                branches = false;
                if ((x & 0xFFFFFC1F) == 0xD65F0000 || (x & 0xFFFFFC1F) == 0xD61F0000) { // ret, br
                    falls = false;
                }
            }
            if (branches && target >= 0 && static_cast<size_t>(target) < w.size()) {
                todo.push_back(static_cast<size_t>(target));
            } else if (branches && strict) {
                return false; // a branch out of the helper: not code of ours
            }
            if (!falls) {
                break;
            }
            ++i;
        }
    }
    if (strict && fresh < 2) {
        return false;
    }
    mark = std::move(seen);
    return true;
}

/// Which words of the helper are instructions. A helper is one flat block, code first and its strings and tables after, and a
/// word of data may look like an instruction that names x18 (the characters "r1", "R…", "2…" have 18 in bits 4:0): rewritten, it
/// would be a branch into a stub in the middle of a string, and refused, the whole helper would be for a table entry. The code is
/// what is reached from the helper's start, from every branch the plan links into it and from the addresses its own code takes
/// (adr, relocated addresses) when what is there reads as code to the end, and from the start of each run between those when it
/// reads as code to the end too (a function only called through a pointer); the rest, and the literals its code loads, is data.
std::vector<u8> CodeMap(std::span<const u32> w, const std::vector<size_t>& sure, const std::vector<size_t>& maybe) {
    std::vector<u8> mark(w.size(), 0);
    for (const size_t e : sure) {
        if (e < w.size()) {
            Walk(w, e, mark, false);
        }
    }
    for (const size_t e : maybe) {
        if (e < w.size() && !mark[e]) {
            Walk(w, e, mark, true);
        }
    }
    for (bool more = true; more;) { // addresses the code takes with adr
        more = false;
        for (size_t i = 0; i < w.size(); ++i) {
            if (!mark[i] || (w[i] & 0x9F000000) != 0x10000000) {
                continue;
            }
            const s64 off = SignExtend((((w[i] >> 5) & 0x7FFFF) << 2) | ((w[i] >> 29) & 3), 21);
            const s64 t = static_cast<s64>(i) * 4 + off;
            if (t >= 0 && t % 4 == 0 && static_cast<size_t>(t / 4) < w.size() && !mark[static_cast<size_t>(t / 4)] &&
                Walk(w, static_cast<size_t>(t / 4), mark, true)) {
                more = true;
            }
        }
    }
    size_t end = 0;
    for (size_t i = 0; i < w.size(); ++i) {
        if (mark[i]) {
            end = i + 1;
        }
    }
    // Between the instructions reached: a function only called through a pointer, or strings and tables that a compiler puts between functions
    // (Pokemon Sword's helper has "L_near1", "L_far1"... after its first functions: "r1\0\0" read as an instruction names x18 and is of no known
    // group, and the whole helper was refused for it). Where such a run starts, what reads as code to its end is code; the rest is data
    for (size_t i = 0; i < end; ++i) {
        if (!mark[i] && (i == 0 || mark[i - 1])) {
            size_t k = i;
            while (k < end && !mark[k] && (w[k] == 0 || w[k] == 0xD503201F)) { // padding before a function
                ++k;
            }
            if (k < end && !mark[k]) {
                Walk(w, k, mark, true);
            }
        }
    }
    std::vector<u8> code(mark.begin(), mark.end());
    for (size_t i = 0; i < end; ++i) { // literals: data inside the code
        if (!mark[i] || (w[i] & 0x3B000000) != 0x18000000) {
            continue;
        }
        const s64 t = static_cast<s64>(i) + SignExtend((w[i] >> 5) & 0x7FFFF, 19);
        // ldr of a SIMD register: 4, 8 or 16 bytes; else ldr w and ldrsw 4 bytes, ldr x 8, prfm none
        const u32 opc = w[i] >> 30;
        const size_t words = ((w[i] >> 26) & 1) ? (opc == 3 ? 0 : (1u << opc)) : (opc == 1 ? 2 : opc == 3 ? 0 : 1);
        for (size_t k = 0; k < words && t >= 0 && static_cast<size_t>(t) + k < w.size(); ++k) {
            code[static_cast<size_t>(t) + k] = 0;
        }
    }
    return code;
}

/// Bytes the plan writes into main run as they are (only the helper's own code goes through RewriteX18): a word that names x18,
/// calls the supervisor or touches a system register is refused. Barriers and hints are let through.
bool MainWordAllowed(u32 w) {
    if ((w & 0xFF000000) == 0xD4000000) {
        return false;
    }
    if ((w & 0xFF000000) == 0xD5000000) {
        return (w & 0xFFFFF000) == 0xD5033000 || (w & 0xFFFFF01F) == 0xD503201F; // dsb / dmb / isb, hints (nop)
    }
    u32 mask{};
    bool plain{};
    const bool raw18 = Field(w, FD) == 18 || Field(w, FN) == 18 || Field(w, FM) == 18 || Field(w, FA) == 18;
    if (!Fields(w, mask, plain)) {
        return !raw18;
    }
    for (const u32 f : {FD, FN, FM, FA}) {
        if ((mask & f) && Field(w, f) == 18) {
            return false;
        }
    }
    return true;
}

/// "min_runtime" of a manifest or package.json: a number, or digits in a string (the runtime's own reader takes both). 0 when absent.
u32 MinRuntime(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("min_runtime")) {
        return 0;
    }
    const auto& v = j.at("min_runtime");
    if (v.is_number_unsigned()) {
        return v.get<u32>();
    }
    if (v.is_string()) {
        const auto t = v.get<std::string>();
        if (!t.empty() && t.size() < 6 && t.find_first_not_of("0123456789") == std::string::npos) {
            return static_cast<u32>(std::stoul(t));
        }
    }
    return 0;
}

Result InstallChecked(Core::System& system, const PnRtConfig& config, const std::array<u8, 0x20>& build_id);

} // namespace

/// The moved sequences of main from the rewriter's site table: the lines "<lo>\tW\t<hi>" as {lo, hi} offsets. False when the
/// file cannot be read.
bool ReadMovedSequences(const char* sites_tsv, std::vector<std::pair<u64, u64>>& out) {
    out.clear();
    std::ifstream f{sites_tsv ? sites_tsv : ""};
    if (!sites_tsv || !f) {
        return false;
    }
    for (std::string line; std::getline(f, line);) {
        const size_t a = line.find('\t');
        if (a == std::string::npos || line.compare(a, 3, "\tW\t") != 0) {
            continue;
        }
        char* end{};
        const u64 lo = std::strtoull(line.c_str(), &end, 16);
        const u64 hi = std::strtoull(line.c_str() + a + 3, nullptr, 16);
        if (end != line.c_str() && hi > lo) {
            out.emplace_back(lo, hi);
        }
    }
    return true;
}

/// The addresses of main's code that `relocs` branch to or take (see the top of this file). Empty when all may be entered, else
/// the refusal. `moved` and `have_moved` are ReadMovedSequences' result; `seen` keeps what was already looked at (an adrp and its
/// add name one address); `checked` counts the addresses looked at. Not in the anonymous namespace: rt/test/gaps_test.cpp calls
/// the two functions.
std::string CheckMainTargets(const PnRtConfig& config, const std::vector<Core::Mods::ModLoadPlan::Relocation>& relocs,
                             const std::vector<std::pair<u64, u64>>& moved, bool have_moved, std::vector<u64>& seen, u32& checked) {
    using Kind = Core::Mods::ModLoadPlan::RelocationKind;
    using Target = Core::Mods::ModLoadPlan::RelocationTarget;
    for (const auto& r : relocs) {
        if (r.target != Target::Main || r.addend < 0) {
            continue;
        }
        const u64 off = static_cast<u64>(r.addend);
        if (off >= config.helper.text_end) {
            if (r.kind == Kind::AArch64Branch26) { // a branch is an entry wherever it goes
                return fmt::format("the load plan calls main+{:#x}, which is past the end of main's code (+{:#x}): not something to run", off,
                                   config.helper.text_end);
            }
            continue; // main's data: read or written, never entered
        }
        if (std::find(seen.begin(), seen.end(), off) != seen.end()) {
            continue;
        }
        seen.push_back(off);
        ++checked;
        if (config.helper.code_rewritten(config.helper.user, config.main_base + (off & ~u64{3}), 4) == 0) {
            continue; // as the file has it
        }
        const char* const does = r.kind == Kind::AArch64Branch26 ? "calls" : "takes the address of";
        if (!have_moved) {
            return fmt::format("the load plan {} main+{:#x}, which this host's rewriter changed or moved (the pack's list of moved sequences, "
                               "main.sites.tsv, is missing: build the game again)",
                               does, off);
        }
        for (const auto& [lo, hi] : moved) {
            if (off > lo && off < hi) {
                return fmt::format("the load plan {} main+{:#x}, which this host's rewriter changed or moved (inside the sequence "
                                   "main+{:#x}..{:#x}, which runs from a copy elsewhere)",
                                   does, off, lo, hi);
            }
        }
        // changed in place: a rewrite site's own word, or the first word of a moved sequence. What stands there does the job
    }
    return {};
}

/// Nothing a package's files hold may end the game: an exception from the readers below is a refused plan.
Result Install(Core::System& system, const PnRtConfig& config, const std::array<u8, 0x20>& build_id) {
    try {
        return InstallChecked(system, config, build_id);
    } catch (const std::exception& e) {
        Result out;
        out.error = std::string{"the load plan could not be read: "} + e.what();
        return out;
    }
}

namespace {

Result InstallChecked(Core::System& system, const PnRtConfig& config, const std::array<u8, 0x20>& build_id) {
    Result out;
    const auto fail = [&](std::string why) {
        out.error = std::move(why);
        return out;
    };
    // The plan's own layout stands for main's here: the build ID, which the reader checks, names the executable
    const std::string build = Common::HexToString(build_id, true);
    const std::string dir = std::string{config.package_dir} + "/dualscreen";
    nlohmann::json manifest, plan_json;
    try {
        std::ifstream m{dir + "/manifest.json"};
        if (!m) {
            return out; // no manifest here: the runtime's own discovery reports it
        }
        manifest = nlohmann::json::parse(m, nullptr, true, true);
        if (!manifest.is_object() || !manifest.contains("load_plan") || !manifest.at("load_plan").is_string()) {
            return out; // a package without a load plan
        }
        std::string path = manifest.at("load_plan").get<std::string>();
        if (const size_t pos = path.find("<build>"); pos != std::string::npos) {
            path.replace(pos, 7, build);
        }
        std::ifstream p{dir + "/" + path};
        if (!p) {
            return fail("the package has no load plan for this build (" + build.substr(0, 16) + ")");
        }
        plan_json = nlohmann::json::parse(p, nullptr, true, true);
    } catch (const nlohmann::json::exception& e) {
        return fail(std::string{"load plan or manifest is not valid JSON: "} + e.what());
    }
    Core::Mods::LoadPlanLayout layout;
    try {
        const auto& l = plan_json.at("layout");
        for (size_t i = 0; i < layout.segments.size(); ++i) {
            layout.segments[i].location = l.at("segments").at(i).at("location").get<u32>();
            layout.segments[i].size = l.at("segments").at(i).at("size").get<u32>();
        }
        layout.bss_size = l.at("bss_size").get<u32>();
    } catch (const nlohmann::json::exception&) {
        return fail("load plan layout missing");
    }
    u32 required = MinRuntime(manifest); // the larger of the manifest's and package.json's, as the runtime takes it
    if (std::ifstream pkg{std::string{config.package_dir} + "/package.json"}; pkg) {
        const auto pj = nlohmann::json::parse(pkg, nullptr, false, true);
        if (!pj.is_discarded()) {
            required = std::max(required, MinRuntime(pj));
        }
    }
    const auto assets = FileSys::OpenHostDirectory(dir);
    if (!assets) {
        return fail("the package's dualscreen folder cannot be opened");
    }
    auto found = Core::Mods::ReadModLoadPlan(assets, manifest, required, config.title_id, build_id, "main", layout);
    if (found.status == Core::Mods::LoadPlanDiscovery::Status::None) {
        return out;
    }
    if (found.status != Core::Mods::LoadPlanDiscovery::Status::Found || !found.plan) {
        return fail(found.error.empty() ? "load plan refused" : found.error);
    }
    const auto& plan = *found.plan;
    if (config.helper.code_rewritten == nullptr || config.helper.code_block == nullptr || config.main_base == 0 || config.main_size == 0 ||
        config.memory.read == nullptr) {
        return fail("this host gave no way to check main's code or to place a helper");
    }
    const u64 base = config.main_base;
    const u64 image_size = u64{layout.segments[2].location} + layout.segments[2].size + layout.bss_size;
    // What the plan writes must be in main's code as this host mapped it (its text to the end of its 16 KiB page), not in what the
    // plan's own layout calls the image: a write past it would leave a page of the game's data read-only and executable
    const u64 text_limit = config.helper.text_end ? (config.helper.text_end + 0x3FFF) & ~u64{0x3FFF} : 0;
    if (!text_limit || text_limit > config.main_size) {
        return fail("this host did not say where main's code ends");
    }
    const u64 code_size = plan.GuestCodeSize(), mailbox_size = (plan.MailboxSize() + Page - 1) & ~(Page - 1);
    const auto* const guest = plan.GuestCodeData();
    const u64 code_len = guest ? guest->bytes.size() : 0;
    if ((code_size != 0) != (code_len != 0)) {
        return fail("unsupported guest code instructions");
    }
    const u64 stub_size = code_size ? Page : 0;
    const u64 exec_size = (code_size + stub_size + 0x3FFF) & ~u64{0x3FFF};   // whole 16 KiB pages for the executable part
    const u64 block_size = exec_size + ((mailbox_size + 0x3FFF) & ~u64{0x3FFF});
    // Writable and zero; the host keeps it for the life of the process
    const u64 block = config.helper.code_block(config.helper.user, block_size);
    if (!block) {
        return fail(fmt::format("the host has no room near main for the helper ({} bytes)", block_size));
    }
    const u64 code = code_size ? block : 0, stubs = block + code_size, mailbox = block + exec_size;

    // Every patch against the file's bytes of main, and linked, before anything is written
    std::vector<std::vector<u8>> staged;
    for (const auto& w : plan.Writes()) {
        const u64 n = w.expected.size();
        if (n == 0 || n != w.replacement.size() || w.offset % 4 != 0 || n % 4 != 0 || w.offset > image_size || n > image_size - w.offset ||
            w.offset > text_limit || n > text_limit - w.offset) {
            return fail("a load plan patch is not whole instructions inside main's code");
        }
        for (const auto& o : plan.Writes()) {
            if (&o != &w && w.offset < o.offset + o.expected.size() && o.offset < w.offset + n) {
                return fail("two load plan patches overlap");
            }
        }
        if (config.helper.code_rewritten(config.helper.user, base + w.offset, n) != 0) {
            return fail(fmt::format("the load plan patches main+{:#x}, which this host's rewriter changed or moved (or its list is missing: build the game again)",
                                    w.offset));
        }
        std::vector<u8> now(n);
        if (!config.memory.read(config.memory.user, base + w.offset, now.data(), n) || now != w.expected) {
            return fail(fmt::format("main+{:#x} does not hold the bytes the load plan expects", w.offset));
        }
        auto bytes = w.replacement;
        if (!Link(bytes, base + w.offset, w.relocations, mailbox, plan.MailboxSize(), base, image_size, code, code_len)) {
            return fail(fmt::format("the patch of main+{:#x} cannot be linked (a branch that does not reach the helper?)", w.offset));
        }
        for (u64 k = 0; k < n; k += 4) {
            u32 word{};
            std::memcpy(&word, bytes.data() + k, 4);
            if (!MainWordAllowed(word)) {
                return fail(fmt::format("the load plan writes {:08x} to main+{:#x}: a word that names x18 or is a supervisor call or system instruction",
                                        word, w.offset + k));
            }
        }
        staged.push_back(std::move(bytes));
    }
    // What the plan enters of main, before anything is written
    u32 targets_checked = 0;
    {
        std::vector<std::pair<u64, u64>> moved_sequences;
        const bool have_moved = ReadMovedSequences(config.helper.sites_tsv, moved_sequences);
        std::vector<u64> seen;
        for (const auto& w : plan.Writes()) {
            if (auto why = CheckMainTargets(config, w.relocations, moved_sequences, have_moved, seen, targets_checked); !why.empty()) {
                return fail(std::move(why));
            }
        }
        if (guest) {
            if (auto why = CheckMainTargets(config, guest->relocations, moved_sequences, have_moved, seen, targets_checked); !why.empty()) {
                return fail(std::move(why));
            }
        }
    }
    int moved = 0;
    if (code_size) {
        auto bytes = guest->bytes;
        if (!Link(bytes, code, guest->relocations, mailbox, plan.MailboxSize(), base, image_size, code, code_len)) {
            return fail("the helper's code cannot be linked here");
        }
        bytes.resize(code_size);
        std::vector<u32> words(code_size / 4), stub_words(stub_size / 4);
        std::memcpy(words.data(), bytes.data(), code_size);
        std::string why;
        using Kind = Core::Mods::ModLoadPlan::RelocationKind;
        using Target = Core::Mods::ModLoadPlan::RelocationTarget;
        std::vector<size_t> sure{0}, maybe;
        const auto entries = [&](const std::vector<Core::Mods::ModLoadPlan::Relocation>& relocs) {
            for (const auto& r : relocs) {
                if (r.target == Target::Code && r.addend >= 0 && r.addend % 4 == 0) {
                    (r.kind == Kind::AArch64Branch26 ? sure : maybe).push_back(static_cast<size_t>(r.addend / 4));
                }
            }
        };
        entries(guest->relocations);
        for (const auto& w : plan.Writes()) {
            entries(w.relocations);
        }
        const auto is_code = CodeMap(words, sure, maybe);
        moved = RewriteX18(words, is_code, code, stub_words, stubs, config.helper.slot18_offset, why);
        if (moved < 0) {
            return fail(why);
        }
        std::memcpy(reinterpret_cast<void*>(code), words.data(), code_size);
        std::memcpy(reinterpret_cast<void*>(stubs), stub_words.data(), stub_size);
        if (mprotect(reinterpret_cast<void*>(block), exec_size, PROT_READ | PROT_EXEC) != 0) {
            return fail(std::string{"the helper's code cannot be made executable: "} + std::strerror(errno));
        }
        __builtin___clear_cache(reinterpret_cast<char*>(block), reinterpret_cast<char*>(block + exec_size));
    }
    const u32 epoch = 1;
    if (const auto at = plan.MailboxEpochOffset()) {
        std::memcpy(reinterpret_cast<void*>(mailbox + *at), &epoch, 4);
    }
    // main's patches last, a word at a time: the game runs, and from the first word on its threads may enter the helper
    const u64 ps = static_cast<u64>(sysconf(_SC_PAGESIZE));
    // The bytes other patches branch into (a format 1 plan's payloads, in main's padding) are written before the branches: a game
    // thread that took a patched call first found the padding still empty
    std::vector<size_t> order, late;
    for (size_t i = 0; i < staged.size(); ++i) {
        const u64 at = base + plan.Writes()[i].offset;
        bool into = false;
        for (u64 k = 0; k < staged[i].size() && !into; k += 4) {
            u32 word{};
            std::memcpy(&word, staged[i].data() + k, 4);
            if ((word & 0x7C000000) != 0x14000000) {
                continue;
            }
            const u64 target = at + k + static_cast<u64>(SignExtend(word & 0x03FFFFFF, 26) * 4);
            for (size_t j = 0; j < staged.size(); ++j) {
                const u64 o = base + plan.Writes()[j].offset;
                into = into || (j != i && target >= o && target < o + staged[j].size());
            }
        }
        (into ? late : order).push_back(i);
    }
    order.insert(order.end(), late.begin(), late.end());
    // The error (errno) of the patch that could not be written, 0 when all are. Called with the game's threads stopped when the
    // host can: system calls and stores only, nothing that allocates or logs
    u64 failed_offset = 0;
    const auto write_patches = [&]() -> int {
        for (const size_t i : order) {
            const auto& w = plan.Writes()[i];
            const u64 at = base + w.offset, n = staged[i].size(), pg = at & ~(ps - 1), len = ((at + n + ps - 1) & ~(ps - 1)) - pg;
            if (mprotect(reinterpret_cast<void*>(pg), len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
                failed_offset = w.offset;
                return errno ? errno : EIO;
            }
            for (u64 k = 0; k < n; k += 4) {
                u32 word{};
                std::memcpy(&word, staged[i].data() + k, 4);
                __atomic_store_n(reinterpret_cast<u32*>(at + k), word, __ATOMIC_RELEASE);
            }
            mprotect(reinterpret_cast<void*>(pg), len, PROT_READ | PROT_EXEC);
            __builtin___clear_cache(reinterpret_cast<char*>(at), reinterpret_cast<char*>(at + n));
        }
        return 0;
    };
    // Stopped, and no thread in or coming back into a patch of several instructions past its first: up to 40 tries, 30 ms apart
    // (a thread found inside moves on once it runs again; the game stands still only during a try, 25 ms at most). Not so within
    // those tries, or a host that cannot stop the threads or cannot look at them: refused, nothing written
    if (config.guest.stop == nullptr || config.guest.resume == nullptr || config.guest.stopped_in == nullptr) {
        out.error = "this host cannot stop the game's threads and look at where they stand, so main cannot be patched safely: the load plan is not "
                    "installed (the package runs without its helper)";
        return out;
    }
    int write_error = 0, timed_out = 0, found_inside = 0;
    bool written = false;
    constexpr int Tries = 40;
    for (int attempt = 0; attempt < Tries && !written; ++attempt) {
        if (attempt != 0) {
            usleep(30000);
        }
        if (config.guest.stop(config.guest.user, 25) <= 0) {
            ++timed_out;
            continue;
        }
        bool inside = false;
        for (size_t i = 0; i < staged.size() && !inside; ++i) {
            const u64 at = base + plan.Writes()[i].offset, n = staged[i].size();
            inside = n > 4 && config.guest.stopped_in(config.guest.user, at + 4, at + n) != 0;
        }
        if (!inside) {
            write_error = write_patches();
            written = true;
        } else {
            ++found_inside;
        }
        config.guest.resume(config.guest.user);
    }
    const char* const how = "with the game's threads stopped";
    if (!written) {
        out.error = fmt::format("main could not be patched with the game's threads stopped ({} tries: {} times they did not all stop, {} times a "
                                "thread stood inside a patched sequence or was to return into it): the load plan is not installed, nothing was "
                                "written to main (the package runs without its helper)",
                                Tries, timed_out, found_inside);
        return out;
    }
    if (write_error != 0) {
        out.error = fmt::format("main+{:#x} cannot be patched ({}): the helper is not installed", failed_offset, std::strerror(write_error));
        return out;
    }
    system.SetDualScreenGuestMailbox(mailbox, plan.MailboxSize(), plan.MailboxEpochOffset() ? epoch : 0);
    out.installed = true;
    if (const auto at = plan.MailboxEpochOffset()) {
        out.epoch_address = mailbox + *at;
        out.epoch = epoch;
    }
    LOG_INFO(Core, "peanuts_rt: load plan installed: mailbox {:#x}+{:#x}, helper code {:#x}+{:#x} ({} x18 instruction(s) moved to stubs), {} patch(es) of main "
                   "written {}; {} address(es) of main's code the plan enters checked against the rewriter's lists",
             mailbox, plan.MailboxSize(), code, code_len, moved, staged.size(), how, targets_checked);
    return out;
}

} // namespace

} // namespace PeanutsHelper
