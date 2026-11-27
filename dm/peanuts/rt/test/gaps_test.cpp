// PNRT: host test of what the host side owes the lifted runtime (run on a PC, no game):
//   1. writes "with the guest stopped": the function runs only between the host's stop and its resume, a stop that cannot be had is
//      a failure and nothing runs, a given-up stop is not tried again at once, and Core::System's entry reaches the host's stop;
//   2. the controller: the state the host hands in is what the runtime's emulated controllers read, and the pad gate filters what
//      the game would read (neutral while on, the latch after, nothing while off);
//   3. a load plan on a synthetic main: what the plan enters of main is checked against the rewriter's lists (a rewrite site's own
//      word and a moved sequence's first word pass, the middle of a moved sequence is refused, with no list a changed word is
//      refused), the patches are written with the threads stopped and not while one stands inside a patch of several
//      instructions, and the mailbox epoch moves on at pause and at resume;
//   4. the page hidden: no publish while hidden, one again when shown, and a finger down when the page went is released.
// Usage: gaps_test <scratch dir>. Built by hand against libpeanuts_rt.a (the include paths of the library's own files).
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <sys/mman.h>

#include "core/core.h"
#include "core/mods/mod_load_plan.h"
#include "hid_core/hid_core.h"
#include "hid_core/resources/npad/dsmod_pad_gate.h"
#include "peanuts_rt.h"

namespace PeanutsHelper {
bool ReadMovedSequences(const char* sites_tsv, std::vector<std::pair<u64, u64>>& out);
std::string CheckMainTargets(const PnRtConfig& config, const std::vector<Core::Mods::ModLoadPlan::Relocation>& relocs,
                             const std::vector<std::pair<u64, u64>>& moved, bool have_moved, std::vector<u64>& seen, u32& checked);
} // namespace PeanutsHelper

namespace {

int g_failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    g_failures += !ok;
}

std::string g_log;
void Log(int, const char*, const char* message) {
    g_log += message ? message : "";
    g_log += '\n';
}

// ---- 1: the stop ------------------------------------------------------------------------------
struct FakeGuest {
    int stops{}, resumes{}, stop_answer{1};
    bool stopped{};
    int inside_answers{}; // stopped_in says 1 this many times, then 0
    int inside_calls{};
    u64 inside_lo{}, inside_hi{};
    static int Stop(void* u, uint32_t) {
        auto& g = *static_cast<FakeGuest*>(u);
        ++g.stops;
        if (g.stop_answer) g.stopped = true;
        return g.stop_answer;
    }
    static void Resume(void* u) {
        auto& g = *static_cast<FakeGuest*>(u);
        ++g.resumes;
        g.stopped = false;
    }
    static int Inside(void* u, uint64_t lo, uint64_t hi) {
        auto& g = *static_cast<FakeGuest*>(u);
        ++g.inside_calls;
        g.inside_lo = lo;
        g.inside_hi = hi;
        return g.inside_answers-- > 0 ? 1 : 0;
    }
    PnRtGuest Callbacks() {
        return {this, &Stop, &Resume, &Inside};
    }
};

void TestStop() {
    std::printf("-- the game's threads stopped\n");
    int ran = 0;
    PnShim::SetGuest({});
    Check(!PnShim::RunWithGuestStopped([&] { ++ran; }) && ran == 0, "a host that cannot stop the threads: refused, the function does not run");

    FakeGuest guest;
    PnShim::SetGuest(guest.Callbacks());
    bool inside_stop = false, nested_ok = false;
    const bool ok = PnShim::RunWithGuestStopped([&] {
        inside_stop = guest.stopped;
        // the runtime reuses a stop it already holds on this thread (a module's restores during a reload)
        nested_ok = PnShim::RunWithGuestStopped([&] { ++ran; }) && guest.stops == 1;
    });
    Check(ok && inside_stop && nested_ok && ran == 1 && guest.stops == 1 && guest.resumes == 1 && !guest.stopped,
          "stopped: the function runs between stop and resume, a nested call does not stop again");

    guest.stop_answer = 0;
    ran = 0;
    Check(!PnShim::RunWithGuestStopped([&] { ++ran; }) && ran == 0 && guest.stops == 2 && guest.resumes == 1,
          "the threads did not stop in time: failure, nothing runs, nothing to resume");
    guest.stop_answer = 1;
    Check(!PnShim::RunWithGuestStopped([&] { ++ran; }) && ran == 0 && guest.stops == 2,
          "right after a stop was given up: refused without holding the game up again");
    std::this_thread::sleep_for(std::chrono::milliseconds(230));
    Check(PnShim::RunWithGuestStopped([&] { ++ran; }) && ran == 1 && guest.stops == 3 && guest.resumes == 2, "later: stops again");
    // Busy (another stop held the game for the whole bound) is not "hard to stop": refused, but the very next call tries again
    guest.stop_answer = -1;
    ran = 0;
    const bool busy_refused = !PnShim::RunWithGuestStopped([&] { ++ran; }) && ran == 0 && guest.stops == 4 && guest.resumes == 2;
    guest.stop_answer = 1;
    Check(busy_refused && PnShim::RunWithGuestStopped([&] { ++ran; }) && ran == 1 && guest.stops == 5,
          "busy: refused, nothing runs, and no back-off (the next call stops)");
    int inside_seen = -2, outside_seen = -2;
    guest.inside_answers = 1;
    PnShim::RunWithGuestStopped([&] { inside_seen = PnShim::StoppedIn(0x1000, 0x1008); });
    outside_seen = PnShim::StoppedIn(0x1000, 0x1008);
    Check(inside_seen == 1 && outside_seen == -1 && guest.inside_lo == 0x1000 && guest.inside_hi == 0x1008,
          "where the threads stand can be asked inside a stop (a writer of several instructions), and only there");

    bool threw = false;
    const int resumes_before = guest.resumes;
    try {
        PnShim::RunWithGuestStopped([] { throw 1; });
    } catch (int) {
        threw = true;
    }
    Check(threw && guest.resumes == resumes_before + 1 && !guest.stopped, "a function that throws: the threads are resumed");

    uint64_t stops{}, failed{}, longest{};
    pn_rt_stop_stats(&stops, &failed, &longest);
    Check(stops == 4 && failed == 4, "the counters: 4 stops run to their end, 4 failures (no host, timed out, backed off, busy)");

    // Core::System's entry, as the lifted runtime calls it (shim/core/core.h)
    Core::System system;
    guest.stop_answer = 0;
    ran = 0;
    const bool through = system.RunWithGuestThreadsSuspended([&] { ++ran; });
    Check(!through && ran == 0, "Core::System::RunWithGuestThreadsSuspended goes through the host's stop (FAIL = the shim header still runs the function beside the game)");
    std::this_thread::sleep_for(std::chrono::milliseconds(230));
    PnShim::SetGuest({});
}

// ---- 2: the controller ------------------------------------------------------------------------
void TestPad() {
    std::printf("-- the controller\n");
    using Core::HID::NpadIdType;
    namespace Gate = Core::HID::DSModPadGate;
    Core::HID::HIDCore hid;
    const auto* const p1 = hid.GetEmulatedController(NpadIdType::Player1);
    const auto* const hh = hid.GetEmulatedController(NpadIdType::Handheld);
    Check(p1 != nullptr && hh != nullptr, "the runtime finds controllers for Player1 and Handheld (FAIL = the shim header still has none)");
    if (p1 == nullptr || hh == nullptr) {
        return;
    }
    const int32_t axes[4] = {20000, -3, 0, 7};
    pn_rt_pad_state(nullptr, 0x20, 0x1001, axes); // handheld: A and Left
    Check(hh->IsConnected() && !p1->IsConnected() && hh->GetNpadButtons().raw == 0x1001 && p1->GetSticks().left.x == 20000 &&
              hh->GetSticks().right.y == 7,
          "handheld mode: Handheld connected with the buttons, Player1 not connected, the sticks readable on both");
    pn_rt_pad_state(nullptr, 0, 0x2, axes);
    Check(p1->IsConnected() && !hh->IsConnected() && p1->GetNpadButtons().raw == 0x2, "docked: Player1 connected");

    // The host's live reader: asked at each read, so a button the companion itself released a moment ago is not still held
    struct LivePad {
        uint32_t npad{0x20};
        uint64_t buttons{};
        int reads{};
        static void Read(void* u, uint32_t* npad_id, uint64_t* out, int32_t a[4]) {
            auto& self = *static_cast<LivePad*>(u);
            ++self.reads;
            *npad_id = self.npad;
            *out = self.buttons;
            a[0] = 11;
            a[1] = a[2] = a[3] = 0;
        }
    } live;
    PnRtPad reader{};
    reader.user = &live;
    reader.read = &LivePad::Read;
    PnShim::SetPadReader(reader);
    live.buttons = 0x1; // the companion presses A for the game
    const bool held = hh->GetNpadButtons().raw == 0x1 && hh->IsConnected() && !p1->IsConnected() && p1->GetSticks().left.x == 11;
    live.buttons = 0;   // ... and releases it, later in the same tick: no pn_rt_pad_state in between
    Check(held && hh->GetNpadButtons().raw == 0 && live.reads >= 4, "with the host's reader the controller is read live: a release is seen at once, not at the next sample");
    PnShim::SetPadReader({});
    Check(p1->GetNpadButtons().raw == 0x2, "without one, the last state handed in");

    Gate::Reset();
    PnShim::NoteTick(); // the runtime ticks: the gate is in force
    uint64_t b = 0x1003;
    int32_t a[4] = {100, 200, 300, 400};
    Check(pn_rt_pad_gate(0, &b, a) == 0 && b == 0x1003 && a[0] == 100 && a[3] == 400, "gate off: the game reads the pad as it is");
    Gate::Set(true, /*pass=*/0x2);
    b = 0x1003;
    Check(pn_rt_pad_gate(0, &b, a) == 1 && b == 0x2 && a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0,
          "gate on: only what the companion passes reaches the game, the sticks are centred");
    Gate::Latch(8, 0x1); // focus ended with A held on the handheld
    Gate::Set(false);
    b = 0x1001;
    int32_t c[4] = {5, 6, 7, 8};
    pn_rt_pad_gate(8, &b, c);
    Check(b == 0x1000 && c[0] == 5, "after focus: the button that was held stays hidden, the rest is back");
    b = 0x1000;
    pn_rt_pad_gate(8, &b, c);
    b = 0x1001;
    pn_rt_pad_gate(8, &b, c);
    Check(b == 0x1001, "released once, the button reaches the game again");
    // Liveness: a runtime that stopped ticking does not keep the game's controller
    Gate::Set(true, 0);
    PnShim::NoteTick();
    b = 0x3;
    const bool gated = pn_rt_pad_gate(0, &b, c) == 1 && b == 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(560));
    b = 0x3;
    int32_t d[4] = {9, 9, 9, 9};
    const bool passed = pn_rt_pad_gate(0, &b, d) == 0 && b == 0x3 && d[0] == 9;
    PnShim::NoteTick();
    b = 0x3;
    Check(gated && passed && pn_rt_pad_gate(0, &b, d) == 1 && b == 0,
          "the gate holds while the runtime ticks, passes everything when it has not for half a second, and holds again at its next tick");
    Gate::Reset();
}

// ---- 3: a load plan on a synthetic main ---------------------------------------------------------
constexpr u64 TitleId = 0x01006BB00C6F0000ULL;
const std::string Build = std::string{"909E904AF78AC1B8"} + std::string(48, '0');
constexpr u64 RegionSize = 0x100000, MainSize = 0x40000, TextEnd = 0x10000, BlockAt = 0x80000;
constexpr u32 Nop = 0xD503201F;
u8* g_region{};

int MemRead(void*, uint64_t addr, void* buf, size_t len) {
    const u64 base = reinterpret_cast<u64>(g_region);
    if (addr < base || addr + len > base + RegionSize) return 0;
    std::memcpy(buf, reinterpret_cast<void*>(addr), len);
    return 1;
}
int MemWrite(void*, uint64_t addr, const void* buf, size_t len) {
    const u64 base = reinterpret_cast<u64>(g_region);
    if (addr < base || addr + len > base + RegionSize) return 0;
    std::memcpy(reinterpret_cast<void*>(addr), buf, len);
    return 1;
}
uint8_t* MemPointer(void*, uint64_t addr, size_t len) {
    const u64 base = reinterpret_cast<u64>(g_region);
    return addr < base || addr + len > base + RegionSize ? nullptr : reinterpret_cast<uint8_t*>(addr);
}
// The rewriter's work on this main: a moved sequence at 0x200..0x210, one rewrite site at 0x300
int CodeRewritten(void*, uint64_t addr, size_t len) {
    const u64 off = addr - reinterpret_cast<u64>(g_region);
    return (off < 0x210 && off + len > 0x200) || (off < 0x304 && off + len > 0x300);
}
uint64_t CodeBlock(void*, size_t size) {
    if (size == 0 || (size & 0x3FFF) != 0 || size > RegionSize - BlockAt) return 0;
    mprotect(g_region + BlockAt, size, PROT_READ | PROT_WRITE);
    std::memset(g_region + BlockAt, 0, size);
    return reinterpret_cast<u64>(g_region + BlockAt);
}
void ResetMain() {
    mprotect(g_region, MainSize, PROT_READ | PROT_WRITE);
    for (u64 at = 0; at < TextEnd; at += 4) std::memcpy(g_region + at, &Nop, 4);
}
u32 Word(u64 offset) {
    u32 w{};
    std::memcpy(&w, g_region + offset, 4);
    return w;
}

/// A package whose load plan patches two instructions at main+0x100 with a call of the helper, and whose helper calls main+`calls`.
std::string WritePlanPackage(const std::filesystem::path& dir, u64 calls) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir / "dualscreen" / "guest" / Build);
    std::ofstream(dir / "package.json") << R"({"format":1,"type":"dual-screen-mod","title_id":"01006BB00C6F0000","name":"gaps","version":"0.0.1","min_runtime":19})";
    std::ofstream(dir / "dualscreen" / "manifest.json") << R"({
      "min_runtime": 19, "format": 1, "title_id": "01006BB00C6F0000", "name": "gaps",
      "load_plan": "guest/<build>/load-plan.json",
      "canvas_w": 640, "canvas_h": 360, "background": "#FF102030",
      "actions": {"go_two": {"kind": "page", "page": "two"}},
      "pages": [
        {"id": "one", "title": "one", "widgets": [
          {"type": "rect", "id": "btn", "rect": [20, 20, 600, 200], "color": "#FF203040", "on_tap": "go_two"},
          {"type": "value", "rect": [40, 240, 560, 50], "text": "V ", "bind": "v", "text_scale": 8, "color": "#FF60E060"}]},
        {"id": "two", "title": "two", "widgets": [
          {"type": "label", "rect": [40, 40, 560, 40], "text": "TWO", "text_scale": 6, "color": "#FFFFFFFF"}]}]})";
    std::ofstream(dir / "dualscreen" / "909E904AF78AC1B8.json") << R"({"points": {"v": {"type": "u32", "ptr": ["main+0x20000"], "offset": "0x0"}}})";
    std::ofstream(dir / "dualscreen" / "guest" / Build / "load-plan.json") << R"({
      "format": 2, "module": "main", "title_id": "01006BB00C6F0000", "build_id": ")" << Build << R"(",
      "layout": {"segments": [{"location": 0, "size": 65536}, {"location": 65536, "size": 65536}, {"location": 131072, "size": 65536}], "bss_size": 65536},
      "mailbox_size": 4096, "mailbox_epoch_offset": 8,
      "patches": [{"offset": 256, "expected": "1f2003d51f2003d5", "replacement": "000000941f2003d5",
                   "relocations": [{"offset": 0, "target": "code", "kind": "aarch64_branch26", "addend": 0}]}],
      "guest_code": {"bytes": "00000094c0035fd6",
                     "relocations": [{"offset": 0, "target": "main", "kind": "aarch64_branch26", "addend": )" << calls << R"(}]}})";
    return dir.string();
}

PnRtConfig PlanConfig(const std::string& package, const char* sites, FakeGuest* guest) {
    PnRtConfig cfg{};
    cfg.package_dir = package.c_str();
    cfg.title_id = TitleId;
    cfg.build_id_hex = Build.c_str();
    cfg.main_base = reinterpret_cast<u64>(g_region);
    cfg.main_size = MainSize;
    cfg.panel_w = 640;
    cfg.panel_h = 360;
    cfg.memory = {nullptr, &MemRead, &MemWrite, &MemPointer, nullptr};
    cfg.helper.code_rewritten = &CodeRewritten;
    cfg.helper.slot18_offset = 0x40;
    cfg.helper.code_block = &CodeBlock;
    cfg.helper.text_end = TextEnd;
    cfg.helper.sites_tsv = sites;
    if (guest) cfg.guest = guest->Callbacks();
    cfg.log = &Log;
    cfg.log_level = 2;
    return cfg;
}

u32 g_publishes = 0;
void Publish(void*, uint32_t, uint32_t, const uint32_t*) {
    ++g_publishes;
}
void PublishPartial(void*, uint32_t, uint32_t, const uint32_t*, const int32_t*) {
    ++g_publishes;
}
void Settle(PnRt* rt, int ticks) {
    for (int i = 0; i < ticks; ++i) {
        pn_rt_tick(rt);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}
bool OnPage(PnRt* rt, const char* id) {
    char status[256];
    pn_rt_status(rt, status, sizeof status);
    return std::strstr(status, (std::string{"page="} + id).c_str()) != nullptr;
}

void TestPlan(const std::filesystem::path& scratch) {
    std::printf("-- a load plan's calls into main\n");
    using Reloc = Core::Mods::ModLoadPlan::Relocation;
    using Kind = Core::Mods::ModLoadPlan::RelocationKind;
    using Target = Core::Mods::ModLoadPlan::RelocationTarget;
    const std::string sites = (scratch / "main.sites.tsv").string();
    std::ofstream(sites) << "0x200\tW\t0x210\n0x300\tx18\n0x400\tJ\t0x500\t3\n";
    std::vector<std::pair<u64, u64>> moved;
    Check(PeanutsHelper::ReadMovedSequences(sites.c_str(), moved) && moved.size() == 1 && moved[0].first == 0x200 && moved[0].second == 0x210,
          "the site table: one moved sequence read, the other kinds of line passed over");
    std::vector<std::pair<u64, u64>> none;
    Check(!PeanutsHelper::ReadMovedSequences((scratch / "absent.tsv").string().c_str(), none), "a pack without the table: said so");

    PnRtConfig cfg{};
    cfg.main_base = reinterpret_cast<u64>(g_region);
    cfg.helper.code_rewritten = &CodeRewritten;
    cfg.helper.text_end = TextEnd;
    const auto verdict = [&](s64 addend, Kind kind, bool have, u32* counted = nullptr) {
        std::vector<Reloc> relocs(1);
        relocs[0].target = Target::Main;
        relocs[0].kind = kind;
        relocs[0].addend = addend;
        std::vector<u64> seen;
        u32 checked = 0;
        auto why = PeanutsHelper::CheckMainTargets(cfg, relocs, moved, have, seen, checked);
        if (counted) *counted = checked;
        return why;
    };
    u32 counted = 0;
    Check(verdict(0x1000, Kind::AArch64Branch26, true, &counted).empty() && counted == 1, "a function the rewriter did not touch: callable");
    Check(verdict(0x300, Kind::AArch64Branch26, true).empty(), "a function whose first instruction is a rewrite site: callable (the site does its job in place)");
    Check(verdict(0x200, Kind::AArch64Branch26, true).empty(), "the first word of a moved sequence: callable (the branch to its copy)");
    const std::string mid = verdict(0x208, Kind::AArch64Branch26, true);
    Check(mid.find("calls main+0x208") != std::string::npos && mid.find("changed or moved") != std::string::npos, "the middle of a moved sequence: refused, named");
    Check(verdict(0x20c, Kind::AArch64Adrp, true).find("takes the address of main+0x20c") != std::string::npos, "an address taken in the middle of one: refused too");
    Check(!verdict(0x300, Kind::AArch64Branch26, false).empty() && verdict(0x1000, Kind::AArch64Branch26, false).empty(),
          "no table of moved sequences: a changed word is refused, an unchanged one is not");
    Check(verdict(0x20208, Kind::AArch64Adrp, true, &counted).empty() && counted == 0, "main's data: not an entry, not counted");
    Check(verdict(0x20208, Kind::AArch64Branch26, true).find("past the end of main's code") != std::string::npos, "a branch past main's code is an entry all the same: refused");
    {
        std::vector<Reloc> pair(2);
        pair[0].target = pair[1].target = Target::Main;
        pair[0].kind = Kind::AArch64Adrp;
        pair[1].kind = Kind::AArch64AddLo12;
        pair[0].addend = pair[1].addend = 0x1000;
        std::vector<u64> seen;
        u32 checked = 0;
        Check(PeanutsHelper::CheckMainTargets(cfg, pair, moved, true, seen, checked).empty() && checked == 1, "an adrp and its add: one address");
    }

    std::printf("-- the plan installed on a synthetic main\n");
    {
        ResetMain();
        g_log.clear();
        const std::string package = WritePlanPackage(scratch / "plan_mid", 0x208);
        FakeGuest unused;
        PnRtConfig c = PlanConfig(package, sites.c_str(), &unused);
        PnRt* rt = pn_rt_create(&c);
        Check(rt != nullptr && Word(0x100) == Nop && g_log.find("the load plan calls main+0x208") != std::string::npos,
              "a helper that calls into the middle of a moved sequence: the plan is refused, main is untouched, the package still loads");
        pn_rt_destroy(rt);
    }
    for (const int why : {0, 1, 2}) { // the threads never stop; one always stands inside the patch; a host that cannot stop them
        ResetMain();
        g_log.clear();
        const std::string package = WritePlanPackage(scratch / ("plan_refused_" + std::to_string(why)), 0x300);
        FakeGuest guest;
        guest.stop_answer = why == 0 ? 0 : 1;
        guest.inside_answers = why == 1 ? 1000 : 0;
        PnRtConfig c = PlanConfig(package, sites.c_str(), why == 2 ? nullptr : &guest);
        PnRt* rt = pn_rt_create(&c);
        const bool said = g_log.find(why == 2 ? "this host cannot stop the game's threads" : "main could not be patched with the game's threads stopped") != std::string::npos;
        Check(rt != nullptr && Word(0x100) == Nop && Word(0x104) == Nop && said && g_log.find("load plan installed") == std::string::npos &&
                  guest.stops == (why == 2 ? 0 : 40) && guest.resumes == (why == 1 ? 40 : 0),
              why == 0   ? "the threads never all stop: the plan is refused after its tries, nothing is written to main, the package loads without its helper"
              : why == 1 ? "a thread always stands inside the patch: refused the same way, every stop resumed"
                         : "a host that cannot stop the threads: refused, never written beside the running game");
        pn_rt_destroy(rt);
    }
    {
        ResetMain();
        g_log.clear();
        const std::string package = WritePlanPackage(scratch / "plan_site", 0x300);
        FakeGuest guest;
        guest.inside_answers = 2; // a game thread stands inside the two-instruction patch at the first two looks
        PnRtConfig c = PlanConfig(package, sites.c_str(), &guest);
        c.ui.publish_ui = &Publish;
        c.ui.publish_ui_partial = &PublishPartial;
        PnRt* rt = pn_rt_create(&c);
        const u64 mailbox = reinterpret_cast<u64>(g_region) + BlockAt + 0x4000;
        const auto epoch = [&] {
            u32 e{};
            std::memcpy(&e, reinterpret_cast<void*>(mailbox + 8), 4);
            return e;
        };
        Check(rt != nullptr && (Word(0x100) & 0xFC000000) == 0x94000000 && Word(0x104) == Nop && g_log.find("load plan installed") != std::string::npos,
              "a helper that calls a rewrite site: installed, main+0x100 calls the helper");
        Check(g_log.find("1 address(es) of main's code the plan enters checked") != std::string::npos, "the count of checked addresses is logged");
        Check(guest.stops == 3 && guest.resumes == 3 && guest.inside_calls == 3 && guest.inside_lo == reinterpret_cast<u64>(g_region) + 0x104 &&
                  guest.inside_hi == reinterpret_cast<u64>(g_region) + 0x108 && g_log.find("with the game's threads stopped") != std::string::npos,
              "the patch is written with the threads stopped, at the third look, when no thread stands inside it");
        if (rt != nullptr) {
            Check(epoch() == 1, "the mailbox epoch starts at 1");
            pn_rt_lifecycle(rt, 1);
            const u32 at_pause = epoch();
            pn_rt_lifecycle(rt, 0);
            Check(at_pause == 2 && epoch() == 3, "it moves on at the pause and again at the resume (the guest's word)");

            // ---- 4: the page hidden ----
            std::printf("-- the page hidden\n");
            u32 v = 7;
            std::memcpy(g_region + 0x20000, &v, 4);
            Settle(rt, 30);
            const u32 shown_publishes = g_publishes;
            Check(shown_publishes > 0 && OnPage(rt, "one"), "shown: the page is published");
            pn_rt_touch(rt, 1, 320, 120, 1); // a finger down on the button
            Settle(rt, 3);
            pn_rt_set_present(rt, 0); // the window goes with the finger still down
            v = 8;
            std::memcpy(g_region + 0x20000, &v, 4);
            Settle(rt, 20);
            Check(g_publishes == shown_publishes, "hidden: a changed value publishes nothing");
            pn_rt_set_present(rt, 1);
            Settle(rt, 30);
            Check(g_publishes > shown_publishes, "shown again: the page is published with the new value");
            // The finger was released when the page went, with no call from the window: the runtime gets its release at its
            // first look after the page is back (as on Eden, whose mailbox keeps a lost finger as ended), which ends the press
            // on the button: its action runs. Left held, the press would never end and the page would stay.
            Check(OnPage(rt, "two"), "the finger that was down when the page went is released, not left held");
            pn_rt_set_panel(rt, 1280, 720); // twice the canvas: a touch at the view's (640, 240) is the page's (320, 120)
            pn_rt_set_page(rt, "one");
            Settle(rt, 5);
            pn_rt_touch(rt, 2, 640, 240, 1);
            Settle(rt, 1);
            pn_rt_touch(rt, 2, 640, 240, 0);
            Settle(rt, 10);
            Check(OnPage(rt, "two"), "with the panel's real size set, a touch in panel pixels lands on the page's button");
        }
        pn_rt_destroy(rt);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: gaps_test <scratch dir>\n");
        return 2;
    }
    const std::filesystem::path scratch = argv[1];
    std::filesystem::create_directories(scratch);
    g_region = static_cast<u8*>(mmap(nullptr, RegionSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (g_region == MAP_FAILED) {
        return 2;
    }
    TestStop();
    TestPad();
    TestPlan(scratch);
    {
        std::printf("-- companions off for the launch\n");
        ResetMain();
        g_log.clear();
        const std::string package = WritePlanPackage(scratch / "plan_off", 0x300);
        const std::string sites = (scratch / "main.sites.tsv").string();
        FakeGuest guest;
        PnRtConfig c = PlanConfig(package, sites.c_str(), &guest);
        pn_rt_set_companions_disabled(1);
        PnRt* rt = pn_rt_create(&c);
        Check(rt == nullptr && Word(0x100) == Nop && g_log.find("load plan installed") == std::string::npos,
              "off: no runtime, and the package's load plan does not touch main");
        pn_rt_set_companions_disabled(0);
        rt = pn_rt_create(&c);
        Check(rt != nullptr && (Word(0x100) & 0xFC000000) == 0x94000000, "on again: the same package loads and its plan is applied");
        pn_rt_destroy(rt);
    }
    std::printf("gaps_test: %s (%d failure(s))\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
