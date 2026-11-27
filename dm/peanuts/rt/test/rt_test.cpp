// rt_test: loads a real companion package through peanuts_rt with a fake guest memory and writes
// the rendered pages to PNG files.
//
//   rt_test <package dir> <out dir> [romfs dir [--provider]]
//   rt_test --module <package dir> <out dir> <title id hex> <build id hex> [--romfs <dir>] [page id...]
//
// `--provider` (R6) hands the romfs dir to the runtime through PnRtRomfs callbacks (a fake provider
// over POSIX open/pread/readdir, below) instead of PnRtConfig::romfs_dir: the PNGs must come out
// the same, which proves the provider-backed VFS path the .nup host glue (peanuts_nup.c) uses.
//
// The second form (R5) drives a package that ships a native module (dualscreen/modules/
// <platform>/<TITLEID>.so, e.g. Fire Emblem: Three Houses): the runtime must accept the package
// (no "update required" page), load the host platform's module through dlopen, and render the
// default page plus every page named, with fake (all-zero) guest memory and no romfs. Writes
// <out>/default.png and <out>/<page>.png; fails when the status line reports no module.
//
// Writes <out>/standby.png and <out>/gear.png (the Link's Awakening pages; "romfs:" art is blank
// without a romfs dir), then <out>/probe.png from a tiny generated package whose page shows the
// fake memory's rupees (main+0x1CC0760 u32 = 123) and health (main+0x1CC0764 s16 = 48) with the
// runtime's built-in font, which proves the sample -> bind -> text path end to end.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stb_image_write.h>

#include "peanuts_rt.h"

namespace {

/// R6: a fake RomFS provider over a directory on disk, through the PnRtRomfs callbacks only
/// (open/pread/readdir): what peanuts_nup.c does over a .nup, here over the extracted folder.
struct DirProvider {
    std::string root;
    std::atomic<int> opens{0}, reads{0}, lists{0}; // the asset workers call concurrently

    std::string Full(const char* path) const {
        return path != nullptr && path[0] != '\0' ? root + "/" + path : root;
    }
    static int Open(void* user, const char* path, uint64_t* size) {
        auto* self = static_cast<DirProvider*>(user);
        const std::string full = self->Full(path);
        struct stat st {};
        if (::stat(full.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return -1;
        const int fd = ::open(full.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) return -1;
        if (size) *size = static_cast<uint64_t>(st.st_size);
        ++self->opens;
        return fd;
    }
    static int64_t Read(void* user, int handle, uint64_t off, void* buf, uint64_t n) {
        ++static_cast<DirProvider*>(user)->reads;
        const ssize_t got = ::pread(handle, buf, static_cast<size_t>(n), static_cast<off_t>(off));
        return got < 0 ? -1 : static_cast<int64_t>(got);
    }
    static void Close(void*, int handle) {
        ::close(handle);
    }
    static int List(void* user, const char* path, void (*cb)(void*, const char*, int), void* cb_user) {
        auto* self = static_cast<DirProvider*>(user);
        ++self->lists;
        DIR* d = ::opendir(self->Full(path).c_str());
        if (d == nullptr) return 0;
        if (cb != nullptr) {
            while (const dirent* e = ::readdir(d)) {
                if (e->d_name[0] == '.') continue;
                struct stat st {};
                const std::string child = self->Full(path) + "/" + e->d_name;
                if (::stat(child.c_str(), &st) != 0) continue;
                cb(cb_user, e->d_name, S_ISDIR(st.st_mode) ? 1 : 0);
            }
        }
        ::closedir(d);
        return 1;
    }
    PnRtRomfs Callbacks() {
        return PnRtRomfs{this, &Open, &Read, &Close, &List};
    }
};

DirProvider* g_provider = nullptr; // set by --provider: MakeConfig then hands callbacks, not a dir

constexpr uint64_t MainBase = 0x0000008000000000ULL; // where the fake "main" module sits
constexpr uint64_t MainSize = 0x2000000ULL;          // 32 MiB
constexpr uint64_t TitleId = 0x01006BB00C6F0000ULL;
constexpr const char* BuildId = "909E904AF78AC1B8DEEFE97AB2CCDB51968F0EC7";

/// A fake guest memory: a map of mapped regions; anything else is unmapped (read fails).
struct FakeMemory {
    std::map<uint64_t, std::vector<uint8_t>> regions;

    std::vector<uint8_t>* Find(uint64_t addr, size_t len, size_t& offset) {
        auto it = regions.upper_bound(addr);
        if (it == regions.begin()) return nullptr;
        --it;
        if (addr < it->first || addr + len > it->first + it->second.size()) return nullptr;
        offset = static_cast<size_t>(addr - it->first);
        return &it->second;
    }
    static int Read(void* user, uint64_t addr, void* buf, size_t len) {
        auto* self = static_cast<FakeMemory*>(user);
        size_t off{};
        auto* r = self->Find(addr, len, off);
        if (!r) return 0;
        std::memcpy(buf, r->data() + off, len);
        return 1;
    }
    static int Write(void* user, uint64_t addr, const void* buf, size_t len) {
        auto* self = static_cast<FakeMemory*>(user);
        size_t off{};
        auto* r = self->Find(addr, len, off);
        if (!r) return 0;
        std::memcpy(r->data() + off, buf, len);
        return 1;
    }
    static uint8_t* PointerAt(void* user, uint64_t addr, size_t len) {
        auto* self = static_cast<FakeMemory*>(user);
        size_t off{};
        auto* r = self->Find(addr, len, off);
        return r ? r->data() + off : nullptr;
    }
    template <typename T>
    void Poke(uint64_t addr, T value) {
        Write(this, addr, &value, sizeof(T));
    }
};

bool WritePng(const std::filesystem::path& path, uint32_t w, uint32_t h,
              const std::vector<uint32_t>& argb) {
    std::vector<uint8_t> rgba(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
        const uint32_t p = argb[i];
        rgba[i * 4 + 0] = static_cast<uint8_t>((p >> 16) & 0xFF);
        rgba[i * 4 + 1] = static_cast<uint8_t>((p >> 8) & 0xFF);
        rgba[i * 4 + 2] = static_cast<uint8_t>(p & 0xFF);
        rgba[i * 4 + 3] = 0xFF; // opaque: the panel composites nothing under the page
    }
    return stbi_write_png(path.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4,
                          rgba.data(), static_cast<int>(w) * 4) != 0;
}

/// Ticks until a new frame lands (or `max_ticks`), then writes it. Returns true when written.
bool RenderPage(PnRt* rt, const char* page, const std::filesystem::path& out) {
    if (page != nullptr && !pn_rt_set_page(rt, page)) {
        std::fprintf(stderr, "rt_test: page '%s' not found\n", page);
        return false;
    }
    std::vector<uint32_t> pixels;
    uint32_t w{}, h{};
    bool got = false;
    // The redraw worker publishes asynchronously; settle a few more frames after the first so
    // assets requested by the first render (none here, but the path is real) could land.
    // Stop once nothing new has been published for ~1.5 s after the first frame (romfs art and
    // the font decode asynchronously on the asset worker and republish when they land).
    int quiet = 0;
    for (int tick = 0; tick < 900 && quiet < 90; ++tick) {
        pn_rt_tick(rt);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        uint32_t fw{}, fh{};
        bool fresh = false;
        if (pn_rt_frame(rt, &fw, &fh, nullptr, 0)) {
            pixels.resize(static_cast<size_t>(fw) * fh);
            if (pn_rt_frame(rt, &w, &h, pixels.data(), pixels.size())) {
                got = fresh = true;
            }
        }
        quiet = got && !fresh ? quiet + 1 : 0;
    }
    if (!got) {
        std::fprintf(stderr, "rt_test: no frame published for page %s\n", page ? page : "(first)");
        return false;
    }
    if (!WritePng(out, w, h, pixels)) {
        std::fprintf(stderr, "rt_test: could not write %s\n", out.string().c_str());
        return false;
    }
    char status[256];
    pn_rt_status(rt, status, sizeof(status));
    std::printf("rt_test: wrote %s (%ux%u) [%s]\n", out.string().c_str(), w, h, status);
    return true;
}

PnRtConfig MakeConfig(FakeMemory& mem, const std::string& package_dir, const char* romfs_dir) {
    PnRtConfig cfg{};
    cfg.package_dir = package_dir.c_str();
    if (g_provider != nullptr && romfs_dir != nullptr) {
        cfg.romfs = g_provider->Callbacks(); // R6: the same folder, through the callbacks only
    } else {
        cfg.romfs_dir = romfs_dir;
    }
    cfg.title_id = TitleId;
    cfg.build_id_hex = BuildId;
    cfg.main_base = MainBase;
    cfg.main_size = MainSize;
    cfg.panel_w = 1280;
    cfg.panel_h = 720;
    cfg.memory = {&mem, &FakeMemory::Read, &FakeMemory::Write, &FakeMemory::PointerAt};
    cfg.log_level = 2;
    return cfg;
}

/// Records every haptic pulse the runtime asks for (PnRtHaptics -> pn_rt_touch tests).
struct HapticLog {
    struct Pulse {
        float amplitude;
        uint32_t ms;
        int respect_system;
        int kind;
    };
    std::vector<Pulse> pulses;
    static void Fn(void* user, float amplitude, uint32_t ms, int respect_system, int kind) {
        static_cast<HapticLog*>(user)->pulses.push_back({amplitude, ms, respect_system, kind});
    }
};

/// Settles a few ticks (16ms apart) without touching the page; used after a touch so the redraw
/// worker has a chance to republish.
void Settle(PnRt* rt, int ticks = 20) {
    for (int i = 0; i < ticks; ++i) {
        pn_rt_tick(rt);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

/// One tap at canvas-space (x,y): finger down for one tick, then released for one tick -- the
/// minimum UpdateGestures needs to see a press-and-release with no travel as a tap (mod_input.cpp
/// DrainTaps runs synchronously inside the same pn_rt_tick that sees the release).
void Tap(PnRt* rt, uint32_t finger, uint32_t x, uint32_t y) {
    pn_rt_touch(rt, finger, x, y, 1);
    pn_rt_tick(rt);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    pn_rt_touch(rt, finger, x, y, 0);
    pn_rt_tick(rt);
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
}

/// Grabs whatever frame is current (ticking a little first if none has been published yet) and
/// writes it to `out`.
bool CapturePng(PnRt* rt, const std::filesystem::path& out) {
    uint32_t w{}, h{};
    std::vector<uint32_t> pixels;
    for (int tick = 0; tick < 120; ++tick) {
        uint32_t fw{}, fh{};
        if (pn_rt_frame(rt, &fw, &fh, nullptr, 0)) {
            pixels.resize(static_cast<size_t>(fw) * fh);
            if (pn_rt_frame(rt, &w, &h, pixels.data(), pixels.size())) {
                return WritePng(out, w, h, pixels);
            }
        }
        pn_rt_tick(rt);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    std::fprintf(stderr, "rt_test: CapturePng: no frame for %s\n", out.string().c_str());
    return false;
}

/// The runtime number this test expects of the lifted runtime (DualScreenRuntimeVersion in
/// mods/mod_runtime.h). Raised by hand with every lift, see PORT.md "How to take the next runtime".
constexpr int ExpectedRuntime = 25;

/// A minimal package whose one page prints the two fake values with the built-in font.
/// `min_runtime` > 0 is written into its manifest (the version gate test), into folder `name`.
std::string WriteProbePackage(const std::filesystem::path& out_dir, int min_runtime = 0,
                              const char* name = "probe_package") {
    const auto pkg = out_dir / name;
    std::filesystem::create_directories(pkg / "dualscreen");
    std::ofstream(pkg / "package.json") << R"({"format":1,"type":"dual-screen-mod",
        "title_id":"01006BB00C6F0000","name":"rt_test probe","version":"0.0.1"})";
    std::ofstream(pkg / "dualscreen" / "manifest.json") << R"({
      "min_runtime": )" << min_runtime << R"(,
      "format": 1, "title_id": "01006BB00C6F0000", "name": "rt_test probe",
      "canvas_w": 640, "canvas_h": 360, "background": "#FF102030",
      "pages": [{"id": "main", "title": "probe", "widgets": [
        {"type": "rect", "rect": [20, 20, 600, 320], "color": "#FF203040"},
        {"type": "label", "rect": [40, 40, 560, 40], "text": "PEANUTS RT PROBE", "text_scale": 6, "color": "#FFFFFFFF"},
        {"type": "value", "rect": [40, 120, 560, 50], "text": "RUPEES ", "bind": "rupees", "text_scale": 8, "color": "#FF60E060"},
        {"type": "value", "rect": [40, 200, 560, 50], "text": "HEALTH ", "bind": "health", "text_scale": 8, "color": "#FFE06060"},
        {"type": "bar", "rect": [40, 280, 560, 30], "bind": "health", "max": 80, "color": "#FFE06060", "bg": "#FF402020"}
      ]}]})";
    std::ofstream(pkg / "dualscreen" / "909E904AF78AC1B8.json") << R"({
      "points": {
        "rupees": {"type": "u32", "ptr": ["main+0x1CC0760"], "offset": "0x0"},
        "health": {"type": "s16", "ptr": ["main+0x1CC0764"], "offset": "0x0"}
      }})";
    return pkg.string();
}

// --- The map probe: a package with one geometry map area and a marker bound to fake memory. Moving
// the marker reveals map cells (the runtime's own fog of war), each reveal fades in over
// map.style.fade_ticks, and the runtime's snapshot log line carries the counters of the map work
// this port adds (PORT.md: the incremental composite, the pulse memo, the publishes a raster
// refused). The probe reads them from the log.

std::mutex g_log_mutex;
std::string g_last_snapshot; ///< the newest "DSMod snapshot:" line

void CaptureLog(int level, const char* where, const char* message) {
    if (std::strstr(message, "DSMod snapshot:") != nullptr) {
        std::scoped_lock lk{g_log_mutex};
        g_last_snapshot = message;
        return; // every 3 seconds: kept out of the test's output
    }
    std::fprintf(stderr, "[%d] %s: %s\n", level, where, message);
}

/// The number after "<key>=" in the newest snapshot line, -1 when absent.
long SnapshotCounter(const char* key) {
    std::scoped_lock lk{g_log_mutex};
    const std::string needle = std::string{" "} + key + "=";
    const auto at = g_last_snapshot.find(needle);
    return at == std::string::npos ? -1 : std::strtol(g_last_snapshot.c_str() + at + needle.size(), nullptr, 10);
}

std::string WriteMapProbePackage(const std::filesystem::path& out_dir) {
    // The map image's longest side. RT_TEST_RASTER_PX makes the rasters longer (8192 is the
    // runtime's limit) to see the page publish beside slow ones, as on a handheld.
    const char* const px_env = std::getenv("RT_TEST_RASTER_PX");
    const int raster_px = px_env != nullptr ? std::atoi(px_env) : 3072;
    const auto pkg = out_dir / "probe_map";
    std::filesystem::create_directories(pkg / "dualscreen");
    std::ofstream(pkg / "package.json") << R"({"format":1,"type":"dual-screen-mod",
        "title_id":"01006BB00C6F0000","name":"rt_test map probe","version":"0.0.1"})";
    std::ofstream(pkg / "dualscreen" / "manifest.json") << R"({
      "format": 1, "title_id": "01006BB00C6F0000", "name": "rt_test map probe",
      "canvas_w": 640, "canvas_h": 360, "background": "#FF000000",
      "map": {"cell": 16, "icons": {}, "style": {"fade_ticks": 60, "raster_px": )" << raster_px << R"(},
              "areas": {"field": {"geo": "file:field.geo", "min": [0, 0], "max": [1000, 500]}}},
      "pages": [{"id": "main", "title": "map", "widgets": [
        {"type": "map", "rect": [0, 0, 640, 360], "area": "field", "id": "m", "color": "#FF7FD4E8",
         "bg": "#FF0B0F14", "marker_x_bind": "px", "marker_y_bind": "py"}
      ]}]})";
    std::ofstream(pkg / "dualscreen" / "909E904AF78AC1B8.json") << R"({
      "points": {
        "px": {"type": "f32", "ptr": ["main+0x1CC0800"], "offset": "0x0"},
        "py": {"type": "f32", "ptr": ["main+0x1CC0804"], "offset": "0x0"}
      }})";
    // The area's geometry: one rectangle over the whole area (4 vertices normalised to 0..65535,
    // 2 triangles), in the runtime's .geo layout: u32 vertex count, u32 index count, u16 x/y
    // pairs, u32 indices.
    const uint32_t counts[2] = {4, 6};
    const uint16_t verts[8] = {0, 0, 65535, 0, 65535, 65535, 0, 65535};
    const uint32_t indices[6] = {0, 1, 2, 0, 2, 3};
    std::ofstream geo(pkg / "dualscreen" / "field.geo", std::ios::binary);
    geo.write(reinterpret_cast<const char*>(counts), sizeof(counts));
    geo.write(reinterpret_cast<const char*>(verts), sizeof(verts));
    geo.write(reinterpret_cast<const char*>(indices), sizeof(indices));
    return pkg.string();
}

/// Walks the marker across the map for 175 ticks (a new cell every few ticks, so a fade is always
/// in progress), stops, and lets the last fade run out. The snapshot line comes every 180 ticks:
/// the one at 180 closes the walk, the one at 360 holds the still fade that followed.
/// Returns the failures.
int RunMapProbe(FakeMemory& mem, const std::filesystem::path& out_dir) {
    const std::string probe = WriteMapProbePackage(out_dir);
    PnRtConfig cfg = MakeConfig(mem, probe, nullptr);
    cfg.panel_w = 640;
    cfg.panel_h = 360;
    cfg.log = &CaptureLog;
    PnRt* rt = pn_rt_create(&cfg);
    if (rt == nullptr) {
        std::fprintf(stderr, "rt_test: pn_rt_create failed for the map probe\n");
        return 1;
    }
    struct Counters {
        long frames, composites, incremental, fade_steps, pulse_hits, skipped, base_ms, composite_ms;
    };
    const auto read = [] {
        return Counters{SnapshotCounter("ui_frames"),      SnapshotCounter("map_composites"),
                        SnapshotCounter("incremental"),    SnapshotCounter("fade_steps"),
                        SnapshotCounter("pulse_memo_hits"), SnapshotCounter("publish_skipped"),
                        SnapshotCounter("base_ms"),        SnapshotCounter("composite_ms")};
    };
    int ticks = 0, slow_ticks = 0;
    double worst_ms = 0.0;
    const auto run_to = [&](int until, bool moving) {
        for (; ticks < until; ++ticks) {
            if (moving) {
                mem.Poke<float>(MainBase + 0x1CC0800, 60.0f + static_cast<float>(ticks) * 0.5f);
                mem.Poke<float>(MainBase + 0x1CC0804, 250.0f);
            }
            const auto t0 = std::chrono::steady_clock::now();
            pn_rt_tick(rt);
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            worst_ms = std::max(worst_ms, ms);
            slow_ticks += ms > 16.0;
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    };
    run_to(175, true);
    const bool wrote = CapturePng(rt, out_dir / "probe_map_fading.png");
    run_to(184, false);
    const Counters walk = read(); // the snapshot of tick 180
    run_to(364, false);
    const Counters end = read(); // the snapshot of tick 360
    std::printf("rt_test: map probe, the walk (ticks 1-180): ui_frames=%ld map_composites=%ld incremental=%ld "
                "fade_steps=%ld pulse_memo_hits=%ld publish_skipped=%ld base_ms=%ld composite_ms=%ld\n",
                walk.frames, walk.composites, walk.incremental, walk.fade_steps, walk.pulse_hits, walk.skipped,
                walk.base_ms, walk.composite_ms);
    std::printf("rt_test: map probe, the still fade after it (ticks 181-360): ui_frames +%ld map_composites +%ld "
                "incremental +%ld pulse_memo_hits +%ld publish_skipped +%ld; "
                "slowest tick %.1f ms, %d ticks over 16 ms\n",
                end.frames - walk.frames, end.composites - walk.composites, end.incremental - walk.incremental,
                end.pulse_hits - walk.pulse_hits, end.skipped - walk.skipped, worst_ms, slow_ticks);
    int failures = !wrote;
    const auto expect = [&](bool ok, const char* what) {
        if (!ok) {
            std::fprintf(stderr, "rt_test: FAILED: map probe: %s\n", what);
            ++failures;
        }
    };
    expect(walk.composites > 0 && walk.fade_steps > 0, "no map composite was built during a fade");
    expect(walk.incremental > 0, "no composite was incremental");
    expect(walk.pulse_hits > 0, "the pulse lookup never found the area's composite memo");
    // The page publishes on every second tick at most, and a composite belongs to a publish.
    expect(walk.composites <= 180 / 2 + 4, "more map composites than publish ticks during the walk");
    // A raster runs only in a redraw job, and a job is sent only by a publish that got the map
    // state: a publish refused for a raster (publish_skipped) is retried, and every composite is
    // followed by a frame. So the page cannot be starved by rasters, however long they are.
    expect(end.frames >= end.composites, "a map composite was not followed by a published frame");
    const bool slow = std::getenv("RT_TEST_RASTER_PX") != nullptr; // long rasters on purpose: fewer steps fit
    // The page keeps publishing while the map fades: at least a frame every fourth tick.
    expect(slow || walk.frames >= 180 / 4, "the page published too few frames while the map was fading");
    // The fade that is still running at tick 180 has about 55 ticks left (fade_ticks 60, the last
    // reveal near tick 175) and steps on every fourth tick: about 14 composites, all incremental.
    const long still = end.composites - walk.composites;
    expect(still <= 20 && (slow ? still >= 1 : still >= 8),
           "a still fade should step on every fourth tick (8 to 20 composites)");
    expect(end.incremental - walk.incremental == still, "a fade step was not an incremental composite");
    expect(end.pulse_hits - walk.pulse_hits >= still, "a fade step's pulse lookup missed the memo");
    // The tick thread never waits for a raster.
    expect(worst_ms < 50.0, "a tick took 50 ms or more");
    pn_rt_destroy(rt);
    return failures;
}

} // namespace

int RunModuleMode(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr,
                     "usage: rt_test --module <package dir> <out dir> <title hex> <build hex> [--romfs <dir>] [--panel <w>x<h>] [page...]\n");
        return 2;
    }
    const std::string package_dir = argv[2];
    const std::filesystem::path out_dir = argv[3];
    const uint64_t title = std::strtoull(argv[4], nullptr, 16);
    const char* build = argv[5];
    // R7: `--romfs <dir>` anywhere after the build id hands the module a game RomFS (the Dread
    // module's standby page names romfs:...bctex textures); the remaining arguments are page ids.
    // Runtime 25: `--panel <w>x<h>` sets the second screen's size in pixels (default 1240x1080), so
    // a package with canvas_alt can be drawn on a portrait or a narrow panel and @screen.* read.
    const char* romfs_dir = nullptr;
    unsigned panel_w = 1240, panel_h = 1080;
    std::vector<const char*> pages;
    for (int i = 6; i < argc; ++i) {
        if (std::strcmp(argv[i], "--romfs") == 0 && i + 1 < argc) {
            romfs_dir = argv[++i];
        } else if (std::strcmp(argv[i], "--panel") == 0 && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%ux%u", &panel_w, &panel_h) != 2 || panel_w == 0 || panel_h == 0) {
                std::fprintf(stderr, "rt_test: --panel wants <w>x<h>\n");
                return 2;
            }
        } else {
            pages.push_back(argv[i]);
        }
    }
    std::filesystem::create_directories(out_dir);
    FakeMemory mem;
    mem.regions[MainBase] = std::vector<uint8_t>(MainSize, 0);
    PnRtConfig cfg = MakeConfig(mem, package_dir, romfs_dir);
    cfg.title_id = title;
    cfg.build_id_hex = build;
    cfg.panel_w = panel_w;
    cfg.panel_h = panel_h;
    const std::string data_dir = (out_dir / "data").string();
    cfg.data_dir = data_dir.c_str();
    PnRt* rt = pn_rt_create(&cfg);
    if (rt == nullptr) {
        std::fprintf(stderr, "rt_test: pn_rt_create failed for %s\n", package_dir.c_str());
        return 1;
    }
    int failures = 0;
    failures += !RenderPage(rt, nullptr, out_dir / "default.png");
    char status[512];
    pn_rt_status(rt, status, sizeof(status));
    std::printf("rt_test: module status: %s\n", status);
    if (std::strstr(status, "module=none") != nullptr || std::strstr(status, "module=error") != nullptr) {
        std::fprintf(stderr, "rt_test: FAILED: the native module did not load\n");
        ++failures;
    }
    if (std::strstr(status, "page=update_required") != nullptr ||
        std::strstr(status, "page=idle") != nullptr) {
        std::fprintf(stderr, "rt_test: FAILED: the package was not accepted (%s)\n", status);
        ++failures;
    }
    for (const char* page : pages) {
        failures += !RenderPage(rt, page, out_dir / (std::string{page} + ".png"));
    }
    pn_rt_destroy(rt);
    std::printf("rt_test: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--module") == 0) {
        return RunModuleMode(argc, argv);
    }
    if (argc < 3) {
        std::fprintf(stderr, "usage: rt_test <package dir> <out dir> [romfs dir [--provider]]\n");
        return 2;
    }
    const std::string package_dir = argv[1];
    const std::filesystem::path out_dir = argv[2];
    const char* romfs_dir = argc > 3 ? argv[3] : nullptr;
    DirProvider provider;
    if (romfs_dir != nullptr && argc > 4 && std::strcmp(argv[4], "--provider") == 0) {
        provider.root = romfs_dir;
        g_provider = &provider;
        std::printf("rt_test: romfs through PnRtRomfs callbacks over %s\n", romfs_dir);
    }
    std::filesystem::create_directories(out_dir);

    FakeMemory mem;
    mem.regions[MainBase] = std::vector<uint8_t>(MainSize, 0);
    mem.Poke<uint32_t>(MainBase + 0x1CC0760, 123); // rupees
    mem.Poke<int16_t>(MainBase + 0x1CC0764, 48);   // health (quarter hearts)

    int failures = 0;
    {
        PnRtConfig cfg = MakeConfig(mem, package_dir, romfs_dir);
        PnRt* rt = pn_rt_create(&cfg);
        if (rt == nullptr) {
            std::fprintf(stderr, "rt_test: pn_rt_create failed for %s\n", package_dir.c_str());
            return 1;
        }
        failures += !RenderPage(rt, "standby", out_dir / "standby.png");
        failures += !RenderPage(rt, "gear", out_dir / "gear.png");
        pn_rt_destroy(rt);
    }
    {
        const std::string probe = WriteProbePackage(out_dir);
        PnRtConfig cfg = MakeConfig(mem, probe, nullptr);
        PnRt* rt = pn_rt_create(&cfg);
        if (rt == nullptr) {
            std::fprintf(stderr, "rt_test: pn_rt_create failed for the probe package\n");
            return 1;
        }
        failures += !RenderPage(rt, nullptr, out_dir / "probe.png");
        pn_rt_destroy(rt);
    }
    // --- The version gate: a package that asks for exactly this runtime is accepted, one that
    // asks for the next number gets the runtime's own "update required" page instead of its pages.
    for (const int ask : {ExpectedRuntime, ExpectedRuntime + 1}) {
        const std::string name = "probe_min_runtime_" + std::to_string(ask);
        const std::string probe = WriteProbePackage(out_dir, ask, name.c_str());
        PnRtConfig cfg = MakeConfig(mem, probe, nullptr);
        PnRt* rt = pn_rt_create(&cfg);
        if (rt == nullptr) {
            std::fprintf(stderr, "rt_test: pn_rt_create failed for %s\n", name.c_str());
            return 1;
        }
        failures += !RenderPage(rt, nullptr, out_dir / (name + ".png"));
        char status[256];
        pn_rt_status(rt, status, sizeof(status));
        const bool refused = std::strstr(status, "page=update_required") != nullptr;
        const bool want_refused = ask > ExpectedRuntime;
        std::printf("rt_test: min_runtime %d: %s (%s)\n", ask, refused ? "refused" : "accepted", status);
        if (refused != want_refused) {
            std::fprintf(stderr, "rt_test: FAILED: min_runtime %d should be %s by runtime %d\n", ask,
                         want_refused ? "refused" : "accepted", ExpectedRuntime);
            ++failures;
        }
        pn_rt_destroy(rt);
    }
    failures += RunMapProbe(mem, out_dir);
    // --- R2 interaction: touch -> AuxRouting -> mod_input.cpp/mod_actions.cpp, on the real LA
    // package. The panel is set to the manifest's own canvas size (1240x1080) so the touch
    // coordinates handed to pn_rt_touch equal canvas pixels exactly (to_canvas_x/y is then the
    // identity), which is all that matters for hitting these two widgets.
    const auto interact_dir = out_dir / "out-romfs2";
    std::filesystem::create_directories(interact_dir);
    HapticLog haptics;
    {
        // (a) Tap the gear page's "Close" button (rect [30,962,1180,118], on_tap "page_map_from_gear"):
        // must switch the current page away from "gear".
        PnRtConfig cfg = MakeConfig(mem, package_dir, romfs_dir);
        cfg.panel_w = 1240;
        cfg.panel_h = 1080;
        cfg.haptics = {&haptics, &HapticLog::Fn};
        PnRt* rt = pn_rt_create(&cfg);
        if (rt == nullptr) {
            std::fprintf(stderr, "rt_test: pn_rt_create failed for the touch test\n");
            return 1;
        }
        failures += !RenderPage(rt, "gear", interact_dir / "gear_before_close.png");
        char status[256];
        pn_rt_status(rt, status, sizeof(status));
        std::printf("rt_test: before Close tap: %s\n", status);

        Tap(rt, /*finger=*/1, /*x=*/30 + 1180 / 2, /*y=*/962 + 118 / 2); // (620, 1021)
        Settle(rt, 10);

        pn_rt_status(rt, status, sizeof(status));
        std::printf("rt_test: after Close tap: %s\n", status);
        const bool page_switched = std::strstr(status, "page=map") != nullptr;
        if (!page_switched) {
            std::fprintf(stderr, "rt_test: FAILED: Close tap did not switch off the gear page\n");
        }
        failures += !page_switched;
        failures += !CapturePng(rt, interact_dir / "gear_after_close.png");
        std::printf("rt_test: haptic pulses so far: %zu\n", haptics.pulses.size());
        pn_rt_destroy(rt);
    }
    {
        // (b) On the gear page: tap item disc "cell_8" (ItemMagicPowder, payload 8, select_group
        // "gear"), then tap the X slot ("drop_X", drop_action "equip_x", accept_group "gear").
        // The tap-then-tap path in DrainTaps (mod_input.cpp) carries the selection into RunAction's
        // ActionKind::Write for "equip_x" (point "equip_x" = main+0x1CC0783, u8), which must write
        // the fake memory through PnRtMemory.write.
        mem.Poke<uint8_t>(MainBase + 0x1CC0783, 0xFF);        // X slot: "empty" sentinel
        mem.Poke<uint32_t>(MainBase + 0x1CC0788, 1u << 8);    // own_8 (bit 8): the Magic Powder is owned

        PnRtConfig cfg = MakeConfig(mem, package_dir, romfs_dir);
        cfg.panel_w = 1240;
        cfg.panel_h = 1080;
        PnRt* rt = pn_rt_create(&cfg);
        if (rt == nullptr) {
            std::fprintf(stderr, "rt_test: pn_rt_create failed for the equip test\n");
            return 1;
        }
        failures += !RenderPage(rt, "gear", interact_dir / "gear_before_equip.png");

        uint8_t before{};
        FakeMemory::Read(&mem, MainBase + 0x1CC0783, &before, sizeof(before));
        std::printf("rt_test: main+0x1CC0783 (equip_x) before = 0x%02X\n", before);

        Tap(rt, 1, 85 + 164 / 2, 137 + 164 / 2);     // cell_8 center (167, 219): select payload 8
        Settle(rt, 2);
        Tap(rt, 1, 1040 + 196 / 2, 211 + 204 / 2);   // drop_X center (1138, 313): tap-drop equip_x
        Settle(rt, 10);

        uint8_t after{};
        FakeMemory::Read(&mem, MainBase + 0x1CC0783, &after, sizeof(after));
        std::printf("rt_test: main+0x1CC0783 (equip_x) after  = 0x%02X\n", after);
        const bool equipped = after == 8;
        if (!equipped) {
            std::fprintf(stderr,
                         "rt_test: FAILED: equip_x was not written (expected 0x08, got 0x%02X)\n",
                         after);
        }
        failures += !equipped;
        failures += !CapturePng(rt, interact_dir / "gear_after_equip.png");
        pn_rt_destroy(rt);
    }
    if (g_provider != nullptr) {
        std::printf("rt_test: provider calls: %d opens, %d reads, %d lists\n", provider.opens.load(),
                    provider.reads.load(), provider.lists.load());
        if (provider.opens == 0 || provider.reads == 0) {
            std::fprintf(stderr, "rt_test: FAILED: the provider callbacks were never used\n");
            ++failures;
        }
    }
    std::printf("rt_test: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
