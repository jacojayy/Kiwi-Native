// svc_test: the companion runtime's host services on a PC, against a fake host (CMake: -DPEANUTS_RT_TEST=ON builds it beside rt_test).
// This program defines the host's C functions itself (the Android host's are weak references in the shims): a fake Vulkan replay with a list of live textures
// (vka_rb_available / vka_rb_submit, answered when the test "presents"), a fake battery, fake frame totals and a fake add-on content opener. Everything else is the
// product's code: the lifted broker, host values and sysfont source, the shims, the patch writer. The last part goes through the entry points the lifted
// code itself calls (Tegra::GPU, Core::PerfStats, Common::GetPowerStatus, OpenAocRomfs, a package whose manifest declares patches, loaded with pn_rt_create):
// it passes only once those shims and stubs call the services (dm/peanuts/rt/PORT.md lists the lines).
//   svc_test [<dir with the firmware's fonts as .ttf: standard.ttf, cjk_kr.ttf, ...>]
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
#include <unistd.h>

#include "../../../gfx/vk_android/vka_readback.h"
#include "common/common_types.h"
#include "core/file_sys/vfs/vfs.h"
#include "core/mods/mod_host_values.h"
#include "core/mods/mod_sysfont_source.h"
#include "core/mods/mod_texture_readback.h"
#include "../../peanuts_services.h"
#include "common/device_power_state.h"
#include "core/core.h"
#include "core/mods/mod_romfs_sources.h"
#include "core/mods/mod_types.h"
#include "core/perf_stats.h"
#include "peanuts_patches.h"
#include "peanuts_rt.h"
#include "pnrt_services.h"
#include "video_core/gpu.h"

namespace {
int g_failed = 0, g_checks = 0;
#define CHECK(cond) \
    do { \
        ++g_checks; \
        if (!(cond)) { \
            ++g_failed; \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        } \
    } while (0)

// ---- the fake replay ------------------------------------------------------------------------------------------------------------------------------------------
struct FakeImage {
    vka_rb_cand cand;
    std::vector<u8> texels; // rows of `stride` bytes
    size_t stride;
};
std::vector<FakeImage> g_images;
std::vector<vka_rb_request> g_queued;
bool g_replay_up = true;
int g_submits = 0;

// One present: every queued request is picked and answered, as the present thread does
void Present() {
    auto queued = std::move(g_queued);
    g_queued.clear();
    for (const auto& rq : queued) {
        std::vector<vka_rb_cand> cands;
        std::vector<size_t> index;
        for (size_t i = 0; i < g_images.size(); ++i) {
            const auto& c = g_images[i].cand;
            if (rq.addr ? c.addr != rq.addr : (c.width != rq.width || c.height != rq.height)) {
                continue;
            }
            cands.push_back(c);
            index.push_back(i);
        }
        const vka_rb_pick pick = rq.pick(rq.user, cands.data(), static_cast<uint32_t>(cands.size()));
        vka_rb_result r{pick.status, 0, rq.width, rq.height, 0, pick.candidates, nullptr, 0, 0};
        if (pick.status == VKA_RB_READY) {
            const auto& image = g_images[index[pick.index]];
            if (!vka_rb_copyable(image.cand.format, rq.format)) {
                r.status = VKA_RB_UNSUPPORTED;
            } else {
                r.addr = image.cand.addr;
                r.generation = image.cand.generation;
                r.rows = image.texels.data();
                r.row_bytes = image.stride;
                r.bgra = image.cand.format >= VKA_RB_F_BGRA8;
            }
        }
        rq.done(rq.user, &r);
    }
}

FakeImage MakeImage(u64 addr, u32 w, u32 h, u8 format, bool rt, bool redrawn, u64 generation, u8 seed) {
    const size_t bpp = format == VKA_RB_F_R8 ? 1 : 4;
    FakeImage image{{addr, w, h, format, 1, 1, static_cast<uint8_t>(rt), static_cast<uint8_t>(redrawn), generation}, {}, size_t{w} * bpp + 12}; // rows wider than the pixels
    image.texels.resize(image.stride * h);
    for (size_t i = 0; i < image.texels.size(); ++i) {
        image.texels[i] = static_cast<u8>(seed + i * 7);
    }
    return image;
}

class ShimBackend final : public Core::Mods::TextureReadbackBackend {
public:
    bool Available() const override {
        return PnShim::ReadbackAvailable();
    }
    bool Submit(const VideoCore::DSMod::ReadbackRequest& request, VideoCore::DSMod::ReadbackCallback callback) override {
        return PnShim::ReadbackSubmit(request, std::move(callback));
    }
};

using Clock = std::chrono::steady_clock;
EdenDsmodTextureRequest Request(u32 format, u64 addr, u32 w, u32 h, u32 flags) {
    return {sizeof(EdenDsmodTextureRequest), format, addr, w, h, flags, 0};
}

void TestReadback() {
    // The pure rules of vka_readback.h
    CHECK(vka_rb_format_class(VKA_RB_VK_R8G8B8A8_UNORM, 0) == VKA_RB_F_RGBA8 && vka_rb_format_class(VKA_RB_VK_R8G8B8A8_UNORM, 1) == VKA_RB_F_RGBA8_SRGB);
    CHECK(vka_rb_format_class(VKA_RB_VK_B8G8R8A8_UNORM, 1) == VKA_RB_F_BGRA8_SRGB && vka_rb_format_class(VKA_RB_VK_R8_UNORM, 0) == VKA_RB_F_R8);
    CHECK(vka_rb_format_class(VKA_RB_VK_A8B8G8R8_SRGB_PACK32, 0) == VKA_RB_F_RGBA8_SRGB);
    CHECK(vka_rb_format_class(131 /* BC1 */, 0) == VKA_RB_F_OTHER && vka_rb_format_class(97 /* RGBA16F */, 0) == VKA_RB_F_OTHER && vka_rb_format_class(122 /* B10G11R11 */, 0) == VKA_RB_F_OTHER);
    CHECK(vka_rb_in_bounds(2048, 2048, VKA_RB_RGBA8) && !vka_rb_in_bounds(2049, 16, VKA_RB_R8) && !vka_rb_in_bounds(0, 16, VKA_RB_R8) && !vka_rb_in_bounds(16, 16, 3));
    CHECK(vka_rb_copyable(VKA_RB_F_BGRA8_SRGB, VKA_RB_RGBA8) && !vka_rb_copyable(VKA_RB_F_R8, VKA_RB_RGBA8) && !vka_rb_copyable(VKA_RB_F_OTHER, VKA_RB_R8));

    // A scene as a game keeps it: the fog target among a dozen one-off 256x256 targets, a BGRA window texture, an R8 mask, a block-compressed texture
    g_images.clear();
    for (int i = 0; i < 11; ++i) {
        g_images.push_back(MakeImage(0x10000 + i * 0x1000, 256, 256, i % 2 ? VKA_RB_F_RGBA8 : VKA_RB_F_RGBA8_SRGB, true, false, 100 + i, static_cast<u8>(i)));
    }
    g_images.push_back(MakeImage(0x80000, 256, 256, VKA_RB_F_RGBA8_SRGB, true, true, 50, 0xA0)); // the one drawn into frame after frame
    g_images.push_back(MakeImage(0x90000, 64, 32, VKA_RB_F_BGRA8, true, true, 7, 0x31));
    g_images.push_back(MakeImage(0xA0000, 32, 32, VKA_RB_F_R8, false, false, 3, 0x55));
    g_images.push_back(MakeImage(0xB0000, 128, 128, VKA_RB_F_OTHER, false, false, 1, 0x77));

    Core::Mods::TextureReadbackBroker broker{std::make_unique<ShimBackend>()};
    auto now = Clock::now();
    s32 status = 99;
    EdenDsmodTextureInfo info{};
    info.struct_size = sizeof info;
    std::vector<u8> out(256 * 256 * 4);

    // By size with the selector a fog readback uses: exactly one candidate, its address reported
    const u32 selector = EDEN_DSMOD_TEXTURE_REQUIRE_RENDER_TARGET | EDEN_DSMOD_TEXTURE_REQUIRE_SRGB | EDEN_DSMOD_TEXTURE_REQUIRE_REDRAWN;
    auto rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0, 256, 256, selector);
    u64 ticket = broker.Request(&rq, &status, now);
    CHECK(ticket != 0 && status == EDEN_DSMOD_TEXTURE_PENDING);
    CHECK(broker.Poll(ticket, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_PENDING); // nothing before a present
    Present();
    CHECK(broker.Poll(ticket, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_READY);
    CHECK(info.candidates == 1 && info.gpu_address == 0x80000 && info.generation == 50 && info.size == out.size() && info.width == 256 && info.height == 256);
    {
        const auto& fog = g_images[11];
        bool same = true;
        for (u32 y = 0; y < 256 && same; ++y) {
            same = std::memcmp(out.data() + y * 256 * 4, fog.texels.data() + y * fog.stride, 256 * 4) == 0; // tight rows, the image's first row first
        }
        CHECK(same);
    }
    // Without the selector the same size is ambiguous: the newest is read and the count says so
    now += std::chrono::milliseconds{600};
    rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0, 256, 256, 0);
    ticket = broker.Request(&rq, &status, now);
    Present();
    CHECK(broker.Poll(ticket, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_READY && info.candidates == 12 && info.gpu_address == 0x10000 + 10 * 0x1000);
    // By address, the selector still applies: the fog target matches, a one-off target is a mismatch
    now += std::chrono::milliseconds{600};
    rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0x80000, 256, 256, selector);
    ticket = broker.Request(&rq, &status, now);
    Present();
    CHECK(broker.Poll(ticket, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_READY && info.candidates == 1 && info.gpu_address == 0x80000);
    now += std::chrono::milliseconds{600};
    rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0x10000, 256, 256, selector);
    ticket = broker.Request(&rq, &status, now);
    Present();
    CHECK(broker.Poll(ticket, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_MISMATCH);
    // B, G, R, A rows come out R, G, B, A
    now += std::chrono::milliseconds{600};
    rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0x90000, 64, 32, 0);
    ticket = broker.Request(&rq, &status, now);
    Present();
    CHECK(broker.Poll(ticket, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_READY && info.size == 64 * 32 * 4);
    {
        const auto& bgra = g_images[12];
        const u8* src = bgra.texels.data() + 5 * bgra.stride + 9 * 4;
        const u8* dst = out.data() + (5 * 64 + 9) * 4;
        CHECK(dst[0] == src[2] && dst[1] == src[1] && dst[2] == src[0] && dst[3] == src[3]);
    }
    // R8; and the wrong layout for it
    now += std::chrono::milliseconds{600};
    rq = Request(EDEN_DSMOD_TEXTURE_R8, 0, 32, 32, 0);
    ticket = broker.Request(&rq, &status, now);
    Present();
    CHECK(broker.Poll(ticket, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_READY && info.size == 32 * 32 && info.gpu_address == 0xA0000 &&
          out[33] == g_images[13].texels[g_images[13].stride + 1]);
    now += std::chrono::milliseconds{600};
    rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0xA0000, 32, 32, 0);
    ticket = broker.Request(&rq, &status, now);
    Present();
    CHECK(broker.Poll(ticket, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_MISMATCH);
    // A format with no conversion: a mismatch by address, not found by size; nothing at an address: not found
    now += std::chrono::milliseconds{600};
    rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0xB0000, 128, 128, 0);
    ticket = broker.Request(&rq, &status, now);
    Present();
    CHECK(broker.Poll(ticket, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_MISMATCH);
    now += std::chrono::milliseconds{600};
    rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0, 128, 128, 0);
    ticket = broker.Request(&rq, &status, now);
    Present();
    CHECK(broker.Poll(ticket, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_NOT_FOUND);
    now += std::chrono::milliseconds{600};
    rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0xDEAD000, 16, 16, 0);
    ticket = broker.Request(&rq, &status, now);
    Present();
    CHECK(broker.Poll(ticket, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_NOT_FOUND);
    // A stale image (a CPU write not yet uploaded) is not read
    g_images[11].cand.current = 0;
    now += std::chrono::milliseconds{600};
    rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0x80000, 256, 256, selector);
    ticket = broker.Request(&rq, &status, now);
    Present();
    CHECK(broker.Poll(ticket, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_MISMATCH);
    g_images[11].cand.current = 1;

    // The limits: a companion that asks every tick gets two a second and two at once, whatever it asks for
    now += std::chrono::seconds{2};
    const int before = g_submits;
    int accepted = 0, limited = 0, busy = 0;
    rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0x80000, 256, 256, selector);
    for (int tick = 0; tick < 300; ++tick) { // five seconds of ticks at 60 Hz, a present each tick
        const u64 t = broker.Request(&rq, &status, now);
        accepted += t != 0;
        limited += status == EDEN_DSMOD_TEXTURE_RATE_LIMITED;
        busy += status == EDEN_DSMOD_TEXTURE_BUSY;
        Present();
        if (t != 0) {
            CHECK(broker.Poll(t, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_READY);
        }
        now += std::chrono::microseconds{16667};
    }
    CHECK(accepted == g_submits - before && accepted >= 9 && accepted <= 11 && limited == 300 - accepted && busy == 0);
    // No present (the game paused): two tickets stay open, a third is BUSY, and each fails after its 2 s
    now += std::chrono::seconds{2};
    const u64 t1 = broker.Request(&rq, &status, now);
    const u64 t2 = broker.Request(&rq, &status, now);
    CHECK(t1 != 0 && t2 != 0);
    now += std::chrono::milliseconds{1100};
    CHECK(broker.Request(&rq, &status, now) == 0 && status == EDEN_DSMOD_TEXTURE_BUSY);
    CHECK(broker.Poll(t1, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_PENDING);
    now += std::chrono::milliseconds{1000};
    CHECK(broker.Poll(t1, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_FAILED && broker.Poll(t2, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_FAILED);
    Present(); // the late answers find no ticket and are dropped
    CHECK(broker.Open() == 0);
    // A rejected request never reaches the replay; a replay that is down answers UNSUPPORTED
    now += std::chrono::seconds{2};
    const int submits = g_submits;
    rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0, 4096, 16, 0);
    CHECK(broker.Request(&rq, &status, now) == 0 && status == EDEN_DSMOD_TEXTURE_INVALID);
    rq = Request(EDEN_DSMOD_TEXTURE_R8, 0, 16, 16, EDEN_DSMOD_TEXTURE_REQUIRE_SRGB);
    CHECK(broker.Request(&rq, &status, now) == 0 && status == EDEN_DSMOD_TEXTURE_INVALID);
    g_replay_up = false;
    rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0x80000, 256, 256, selector);
    CHECK(broker.Request(&rq, &status, now) == 0 && status == EDEN_DSMOD_TEXTURE_UNSUPPORTED && g_submits == submits);
    g_replay_up = true;
}

// ---- "sysfont:" -----------------------------------------------------------------------------------------------------------------------------------------------
std::vector<u8> ReadFile(const std::filesystem::path& path) {
    std::ifstream f{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
}
void WriteFile(const std::filesystem::path& path, const std::vector<u8>& bytes) {
    std::ofstream f{path, std::ios::binary};
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
bool SameFont(const std::vector<u8>& served, const std::vector<u8>& file) {
    // The stored form is whole words: the served bytes are the file's, padded with zeros to a multiple of four
    if (served.size() != ((file.size() + 3) & ~size_t{3}) || std::memcmp(served.data(), file.data(), file.size()) != 0) {
        return false;
    }
    for (size_t i = file.size(); i < served.size(); ++i) {
        if (served[i] != 0) {
            return false;
        }
    }
    return true;
}

void TestSysfont(const std::filesystem::path& tmp, const char* real_fonts) {
    const auto dir = tmp / "fonts";
    std::filesystem::create_directories(dir);
    std::vector<u8> standard{0, 1, 0, 0};
    for (int i = 0; i < 100001; ++i) { // an odd length: the padding matters
        standard.push_back(static_cast<u8>(i * 31 + (i >> 8)));
    }
    std::vector<u8> korean{'O', 'T', 'T', 'O'};
    for (int i = 0; i < 4096; ++i) {
        korean.push_back(static_cast<u8>(i * 5));
    }
    WriteFile(dir / "standard.ttf", standard);
    WriteFile(dir / "cjk_kr.ttf", korean);
    WriteFile(dir / "cjk_tc.ttf", std::vector<u8>(64, 0x42)); // not a font: reads as missing
    const auto root = PnShim::SysfontRootOf((dir / "standard.ttf").string(), "");
    CHECK(root != nullptr);
    const auto source = Core::Mods::MakeSysfontSource([root](u64) { return root; });
    CHECK(source.prefix == "sysfont" && source.read_bytes);
    CHECK(SameFont(source.read_bytes("sysfont:nintendo_udsg-r_std_003"), standard));
    CHECK(SameFont(source.read_bytes("sysfont:nintendo_udsg-r_ko_003.bfttf"), korean));
    CHECK(source.read_bytes("sysfont:nintendo_udjxh-db_zh-tw_003").empty());       // not a font
    CHECK(source.read_bytes("sysfont:nintendo_udsg-r_org_zh-cn_003").empty());     // no file
    CHECK(source.read_bytes("sysfont:nintendo_ext_003").empty());                  // no extension font named
    CHECK(source.read_bytes("sysfont:standard").empty());                          // not a firmware name
    // A slice, as a module reads a large font
    if (source.read_range) {
        std::vector<u8> part(1000);
        CHECK(source.read_range("sysfont:nintendo_udsg-r_std_003", 99500, part.data(), part.size()) == 508 && std::memcmp(part.data(), standard.data() + 99500, 505) == 0);
    }
    setenv("DM_SHARED_FONT", (dir / "standard.ttf").c_str(), 1);
    setenv("DM_SHARED_FONT_EXT", (dir / "cjk_kr.ttf").c_str(), 1);
    const auto env_root = PnShim::SysfontRoot();
    const auto env_source = Core::Mods::MakeSysfontSource([env_root](u64) { return env_root; });
    CHECK(env_root != nullptr && SameFont(env_source.read_bytes("sysfont:nintendo_ext_003"), korean));
    unsetenv("DM_SHARED_FONT");
    CHECK(PnShim::SysfontRoot() == nullptr);
    // The stand-ins' folder is never served under the firmware's names, whoever points the switches at it
    setenv("DM_FONT_DIR", dir.c_str(), 1);
    const auto stand_in_root = PnShim::SysfontRootOf((dir / "standard.ttf").string(), (dir / "cjk_kr.ttf").string());
    const auto stand_in = Core::Mods::MakeSysfontSource([stand_in_root](u64) { return stand_in_root; });
    CHECK(stand_in.read_bytes("sysfont:nintendo_udsg-r_std_003").empty() && stand_in.read_bytes("sysfont:nintendo_udsg-r_ko_003").empty() &&
          stand_in.read_bytes("sysfont:nintendo_ext_003").empty());
    unsetenv("DM_FONT_DIR");
    // A file over 64 MiB is not a shared font (sparse here: nothing is written)
    {
        const auto huge = tmp / "huge";
        std::filesystem::create_directories(huge);
        WriteFile(huge / "standard.ttf", {0, 1, 0, 0});
        std::filesystem::resize_file(huge / "standard.ttf", (u64{64} << 20) + 4);
        const auto huge_root = PnShim::SysfontRootOf((huge / "standard.ttf").string(), "");
        CHECK(huge_root->GetFile("nintendo_udsg-r_std_003.bfttf") == nullptr);
        std::filesystem::resize_file(huge / "standard.ttf", u64{64} << 20);
        CHECK(huge_root->GetFile("nintendo_udsg-r_std_003.bfttf") != nullptr);
    }
    if (real_fonts != nullptr) { // the player's firmware fonts as the importer wrote them
        const std::filesystem::path real{real_fonts};
        const auto real_root = PnShim::SysfontRootOf((real / "standard.ttf").string(), (real / "nintendo_ext.ttf").string());
        const auto real_source = Core::Mods::MakeSysfontSource([real_root](u64) { return real_root; });
        static constexpr std::pair<const char*, const char*> files[] = {
            {"nintendo_udsg-r_std_003", "standard.ttf"}, {"nintendo_udsg-r_org_zh-cn_003", "cjk_sc.ttf"}, {"nintendo_udsg-r_ext_zh-cn_003", "cjk_sc_ext.ttf"},
            {"nintendo_udjxh-db_zh-tw_003", "cjk_tc.ttf"}, {"nintendo_udsg-r_ko_003", "cjk_kr.ttf"}, {"nintendo_ext_003", "nintendo_ext.ttf"}};
        for (const auto& [name, file] : files) {
            const auto bytes = ReadFile(real / file);
            const auto served = real_source.read_bytes(std::string{"sysfont:"} + name);
            const bool ok = !bytes.empty() && SameFont(served, bytes);
            std::printf("sysfont: %s <- %s: %zu bytes %s\n", name, file, served.size(), ok ? "equal" : "DIFFERENT");
            CHECK(ok);
        }
    }
}

// ---- "aoc:", battery, frame totals: the fake host ---------------------------------------------------------------------------------------------------------------
int g_power_known = 0, g_power_pct = 0, g_power_chg = 0;
uint64_t g_frames = 0, g_system_frames = 0;
int64_t g_frametime_ns = 0;
int g_aoc_opens = 0;
std::filesystem::path g_aoc_container_dir;

int FakeOpen(void* user, const char* path, uint64_t* size) {
    std::error_code ec;
    const auto p = *static_cast<std::filesystem::path*>(user) / path;
    if (!std::filesystem::is_regular_file(p, ec)) {
        return -1;
    }
    *size = std::filesystem::file_size(p, ec);
    return 0;
}
int64_t FakeRead(void*, int, uint64_t, void*, uint64_t) {
    return 0; // the test only looks files up
}
void FakeClose(void*, int) {}

void TestAoc(const std::filesystem::path& tmp) {
    const auto aoc = tmp / "aoc";
    std::filesystem::create_directories(aoc / "2" / "resources");
    std::filesystem::create_directories(aoc / "5");
    WriteFile(aoc / "2" / "resources" / "dlc.txt", {'t', 'w', 'o'});
    WriteFile(aoc / "5" / "five.txt", {'5'});
    WriteFile(aoc / "notes.txt", {'x'});
    unsetenv("DM_AOC_DIR");
    CHECK(PnShim::AocRoot(0x0100000000001000) == nullptr);
    setenv("DM_AOC_DIR", aoc.c_str(), 1);
    auto root = PnShim::AocRoot(0x0100000000001000);
    CHECK(root != nullptr && root->GetFileRelative("resources/dlc.txt") != nullptr && root->GetFileRelative("five.txt") == nullptr); // the lowest index
    // A container form goes through the host (here: the fake), and a lower index wins
    g_aoc_container_dir = aoc / "5";
    WriteFile(aoc / "1.nup", {'n', 'u', 'p'});
    root = PnShim::AocRoot(0x0100000000001000);
    CHECK(g_aoc_opens == 1 && root != nullptr && root->GetFileRelative("five.txt") != nullptr);
    // The lowest index in a form that cannot be read here: nothing, never the next add-on in its place
    std::filesystem::remove(aoc / "1.nup");
    g_aoc_container_dir.clear();
    WriteFile(aoc / "1.img", {'i'});
    CHECK(PnShim::AocRoot(0x0100000000001000) == nullptr);
    // ... nor when the host cannot open the lowest index's container
    std::filesystem::remove(aoc / "1.img");
    WriteFile(aoc / "1.nsp", {'n'});
    const int opens = g_aoc_opens;
    CHECK(PnShim::AocRoot(0x0100000000001000) == nullptr && g_aoc_opens == opens);
    // The same index as a container and a folder: the container, as the host's mount takes it
    std::filesystem::remove(aoc / "1.nsp");
    std::filesystem::create_directories(aoc / "1");
    WriteFile(aoc / "1" / "folder.txt", {'f'});
    WriteFile(aoc / "1.nup", {'n'});
    g_aoc_container_dir = aoc / "5";
    root = PnShim::AocRoot(0x0100000000001000);
    CHECK(root != nullptr && root->GetFileRelative("five.txt") != nullptr && root->GetFileRelative("folder.txt") == nullptr);
    std::filesystem::remove(aoc / "1.nup");
    root = PnShim::AocRoot(0x0100000000001000);
    CHECK(root != nullptr && root->GetFileRelative("folder.txt") != nullptr);
    // Nothing at all
    std::filesystem::remove_all(aoc / "1");
    std::filesystem::remove_all(aoc / "2");
    std::filesystem::remove_all(aoc / "5");
    CHECK(PnShim::AocRoot(0x0100000000001000) == nullptr);
}

// The parts of the Android host's side that only compute (dm/peanuts/peanuts_services.h)
double g_ring_ms[PN_PERF_RING];
uint64_t g_ring_frame[PN_PERF_RING];
double RingMs(uint64_t f) {
    return g_ring_frame[f % PN_PERF_RING] == f ? g_ring_ms[f % PN_PERF_RING] : -1; // as the replay's ring: a frame that left it has no time
}
void TestHostPure(const std::filesystem::path& tmp) {
    // The add-on content spec, as the host's mount writes it
    const auto dir = tmp / "aocspec";
    std::filesystem::create_directories(dir / "4");
    WriteFile(dir / "2.nup", {'n'});
    WriteFile(dir / "3.nsp", {'n'});
    WriteFile(dir / "5.img", {'i'});
    std::filesystem::create_symlink("fd:17/Game DLC.nup", dir / "6.nup");
    std::filesystem::create_symlink("fd:18/dlc.nsp", dir / "7.nsp");
    std::filesystem::create_symlink("/nowhere/x.nup", dir / "8.nup");
    char spec[1300];
    const std::string d = dir.string();
    CHECK(pn_aoc_spec(spec, sizeof spec, d.c_str(), 2) == 1 && spec == d + "/2.nup:aoc/2");
    CHECK(pn_aoc_spec(spec, sizeof spec, d.c_str(), 3) == 1 && spec == "nsp:" + d + "/3.nsp|aoc/3");
    CHECK(pn_aoc_spec(spec, sizeof spec, d.c_str(), 6) == 1 && std::string{spec} == "fd:17/Game DLC.nup:aoc/6");
    CHECK(pn_aoc_spec(spec, sizeof spec, d.c_str(), 7) == 1 && std::string{spec} == "nsp:fd:18/dlc.nsp|aoc/7");
    CHECK(pn_aoc_spec(spec, sizeof spec, d.c_str(), 4) == 0 && pn_aoc_spec(spec, sizeof spec, d.c_str(), 5) == 0 && pn_aoc_spec(spec, sizeof spec, d.c_str(), 8) == 0 &&
          pn_aoc_spec(spec, sizeof spec, d.c_str(), 9) == 0);
    CHECK(pn_aoc_spec(spec, 8, d.c_str(), 2) == 0); // no room for the spec: not a cut one
    // The frame totals over the ring of GPU times
    const auto put = [](uint64_t f, double ms) {
        g_ring_frame[f % PN_PERF_RING] = f;
        g_ring_ms[f % PN_PERF_RING] = ms;
    };
    pn_perf_acc acc{};
    for (uint64_t f = 0; f < 5; ++f) put(f, 2.0);
    pn_perf_step(&acc, 5, RingMs);
    CHECK(acc.frames == 0 && acc.sum_ns == 0); // nothing is 9 frames old yet
    for (uint64_t f = 5; f < 40; ++f) put(f, f == 20 ? -1.0 : 2.0); // frame 20's time never came in
    pn_perf_step(&acc, 39, RingMs);
    CHECK(acc.next == 31 && acc.frames == 30 && acc.sum_ns == 30 * 2'000'000); // frames 0..30 but 20
    pn_perf_step(&acc, 39, RingMs);
    CHECK(acc.frames == 30); // the same frame again adds nothing
    for (uint64_t f = 40; f < 400; ++f) put(f, 4.0); // a step that comes 360 frames late: only what the ring still holds
    pn_perf_step(&acc, 399, RingMs);
    CHECK(acc.next == 391 && acc.frames == 30 + 55 && acc.sum_ns == 30 * 2'000'000 + 55 * 4'000'000);
    pn_perf_step(&acc, 3, RingMs); // (a counter that went back: starts over without counting twice what it cannot know)
    CHECK(acc.next == 0 && acc.frames == 85);
    // The battery: a pushed reading is believed for a minute; the kernel's lines
    CHECK(!pn_battery_pushed_fresh(-2, 0, 5) && pn_battery_pushed_fresh(80, 1000, 1000 + PN_BATTERY_FRESH_NS - 1) && !pn_battery_pushed_fresh(80, 1000, 1000 + PN_BATTERY_FRESH_NS));
    CHECK(pn_battery_pushed_fresh(-1, 1000, 2000)); // "no battery" is a reading too
    int pct = -9, chg = -9;
    CHECK(pn_battery_parse("57", "Charging", &pct, &chg) == 1 && pct == 57 && chg == 1);
    CHECK(pn_battery_parse("100", "Full", &pct, &chg) == 1 && pct == 100 && chg == 1);
    CHECK(pn_battery_parse("3", "Discharging", &pct, &chg) == 1 && pct == 3 && chg == 0 && pn_battery_parse("40", nullptr, &pct, &chg) == 1 && chg == 0);
    CHECK(pn_battery_parse("", "Full", &pct, &chg) == 0 && pn_battery_parse("101", "Full", &pct, &chg) == 0 && pn_battery_parse(nullptr, nullptr, &pct, &chg) == 0);
}

void TestHostValues() {
    // The shim's answers
    g_power_known = 0;
    CHECK(!PnShim::PowerStatus().has_battery && PnShim::PowerStatus().percentage == -1);
    g_power_known = 1, g_power_pct = 57, g_power_chg = 1;
    const auto power = PnShim::PowerStatus();
    CHECK(power.has_battery && power.percentage == 57 && power.charging);
    // Through the lifted host values, as a page or a module reads them: a game at 30 frames a second whose frames take 12.5 ms of GPU time
    s64 now = 1'000'000'000;
    Core::Mods::HostValues values;
    values.SetSource({
        .now_ns = [&now] { return now; },
        .power = []() -> Core::Mods::PowerReading {
            const auto status = PnShim::PowerStatus(); // what ReadDevicePower does with Common::GetPowerStatus
            return status.has_battery && status.percentage >= 0 ? Core::Mods::PowerReading{true, status.percentage, status.charging} : Core::Mods::PowerReading{};
        },
        .perf = [&now]() -> std::optional<Core::Mods::PerfSample> {
            const auto totals = PnShim::ReadPerfTotals(); // what the runtime does with PerfStats::GetTotals
            return Core::Mods::PerfSample{totals.game_frames, totals.system_frames, totals.frametime_ns, now / 1000};
        },
        .screen = {},
    });
    for (int tick = 0; tick < 120; ++tick) {
        values.Update();
        now += 16'666'667;
        if (tick % 2 == 0) {
            ++g_frames;
            ++g_system_frames;
            g_frametime_ns += 12'500'000;
        }
    }
    CHECK(values.GetI64("@device.battery_present") == 1 && values.GetI64("@device.battery_pct") == 57 && values.GetI64("@device.charging") == 1);
    CHECK(values.HasPerf() && values.GetI64("@perf.fps") == 30 && values.GetI64("@perf.speed_pct") == 100);
    const auto frametime = values.GetF64("@perf.frametime_ms");
    CHECK(frametime && *frametime > 12.4 && *frametime < 12.6 && values.GetI64("@perf.frametime_x10") == 125);
    std::printf("host values: battery %lld%% charging %lld, fps %lld, speed %lld%%, frametime %.2f ms\n", static_cast<long long>(*values.GetI64("@device.battery_pct")),
                static_cast<long long>(*values.GetI64("@device.charging")), static_cast<long long>(*values.GetI64("@perf.fps")),
                static_cast<long long>(*values.GetI64("@perf.speed_pct")), frametime ? *frametime : -1.0);
}

// ---- manifest patches -----------------------------------------------------------------------------------------------------------------------------------------
constexpr u32 Nop = 0xD503201F;
int ProtOf(const void* at) { // from /proc/self/maps
    std::ifstream maps{"/proc/self/maps"};
    std::string line;
    const u64 a = reinterpret_cast<u64>(at);
    while (std::getline(maps, line)) {
        u64 lo = 0, hi = 0;
        char perms[8] = "";
        if (std::sscanf(line.c_str(), "%lx-%lx %7s", &lo, &hi, perms) == 3 && a >= lo && a < hi) {
            return (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) | (perms[2] == 'x' ? PROT_EXEC : 0);
        }
    }
    return -1;
}
void TestPatches() {
    using PeanutsPatches::Verdict;
    PeanutsPatches::ForgetWritten();
    const size_t size = 0x10000;
    auto* const main = static_cast<u32*>(mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    CHECK(main != MAP_FAILED);
    for (size_t i = 0; i < size / 4; ++i) {
        main[i] = Nop;
    }
    main[0xC00 / 4] = 0xAA0003E0; // mov x0, x0: not an instruction that may change under a running thread
    mprotect(main, 0x8000, PROT_READ | PROT_EXEC);                       // the code
    mprotect(main + 0x8000 / 4, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC); // a page that was writable too stays so
    int stops = 0, parked = 0, inside_calls = 0;
    u64 inside_lo = 0, inside_hi = 0;
    bool can_stop = true;
    PeanutsPatches::Target target;
    target.base = reinterpret_cast<u64>(main);
    target.text_end = 0x9004; // the code ends in the middle of a page: the limit is the code's end, not the page's
    target.rewritten = [&](u64 addr, size_t len) { return addr < target.base + 0x200 && addr + len > target.base + 0x100; }; // the rewriter changed [0x100, 0x200)
    target.stopped = [&](const std::function<void()>& fn) {
        if (!can_stop) {
            return false;
        }
        ++stops;
        fn();
        return true;
    };
    target.stopped_in = [&](u64 lo, u64 hi) {
        ++inside_calls;
        inside_lo = lo;
        inside_hi = hi;
        return parked > 0 ? (--parked, 1) : 0;
    };
    const u32 branch[] = {0x14000010};                       // b +0x40
    const u32 mov[] = {0x52800020};                          // mov w0, #1
    const u32 three[] = {0x52800000, 0x52800001, 0xD65F03C0};
    const auto write = [&](s64 at, std::span<const u32> words, std::span<const u32> expected = {}) { return PeanutsPatches::Write(target, at, words, expected); };
    // The set that may change while a thread runs it
    CHECK(PeanutsPatches::SafeWhileRunning(Nop, 0x14000010) && PeanutsPatches::SafeWhileRunning(0x94000004, 0xD4200000) && PeanutsPatches::SafeWhileRunning(0xD4000001, 0xD5033FDF));
    CHECK(!PeanutsPatches::SafeWhileRunning(Nop, 0x52800020) && !PeanutsPatches::SafeWhileRunning(0xAA0003E0, Nop) && !PeanutsPatches::SafeWhileRunning(0xD4000000, Nop));
    // NOP -> B: one atomic store, no stop
    CHECK(write(0x400, branch).verdict == Verdict::Written && main[0x100] == 0x14000010 && stops == 0);
    // NOP -> MOV: one instruction, but not of that set: the threads are stopped
    CHECK(write(0x404, mov).verdict == Verdict::Written && main[0x101] == 0x52800020 && stops == 1);
    CHECK(write(0xC00, branch).verdict == Verdict::Written && main[0x300] == 0x14000010 && stops == 2); // the old word decides too
    // Several instructions: stopped, and asked whether a thread stands past the first
    CHECK(write(0x800, three).verdict == Verdict::Written && main[0x200] == 0x52800000 && main[0x202] == 0xD65F03C0 && stops == 3 && inside_calls == 1 &&
          inside_lo == target.base + 0x804 && inside_hi == target.base + 0x80C);
    CHECK(ProtOf(main) == (PROT_READ | PROT_EXEC)); // as it was
    // A thread parked inside: not now, nothing written; the next try writes
    parked = 1;
    auto o = write(0x900, three);
    CHECK(o.verdict == Verdict::Retry && main[0x240] == Nop && main[0x242] == Nop && stops == 4);
    CHECK(write(0x900, three).verdict == Verdict::Written && main[0x242] == 0xD65F03C0);
    // The threads cannot be stopped: not now, for one instruction as for several
    can_stop = false;
    CHECK(write(0xA00, three).verdict == Verdict::Retry && main[0x280] == Nop && write(0xA40, mov).verdict == Verdict::Retry && main[0x290] == Nop);
    CHECK(write(0xA80, branch).verdict == Verdict::Written && main[0x2A0] == 0x14000010); // (the one that needs no stop)
    can_stop = true;
    CHECK(write(0xA00, three).verdict == Verdict::Written && write(0xA40, mov).verdict == Verdict::Written);
    // Refused for good: rewritten code, out of the code, not whole instructions, nothing to write, a host that cannot tell
    CHECK(write(0x1FC, three).verdict == Verdict::Refused && main[0x7F] == Nop && main[0x80] == Nop);
    CHECK(write(0x180, mov).verdict == Verdict::Refused && main[0x60] == Nop);
    CHECK(write(0x8FFC, three).verdict == Verdict::Refused && main[0x23FF] == Nop);        // runs past the code's end (0x9004), though inside its page
    CHECK(write(0x9004, mov).verdict == Verdict::Refused && main[0x2401] == Nop && write(0x9000, mov).verdict == Verdict::Written);   // the first word past the code, and the last one in it
    CHECK(write(-4, mov).verdict == Verdict::Refused && write(0x402, mov).verdict == Verdict::Refused && write(0x440, {}).verdict == Verdict::Refused);
    // Idempotent: the same words again are done without a write or a stop; other words over a patch are refused
    const int stops_before = stops;
    o = write(0x800, three);
    CHECK(o.verdict == Verdict::Written && o.why == "already there" && stops == stops_before);
    CHECK(write(0x804, mov).verdict == Verdict::Refused && main[0x201] == 0x52800001);
    CHECK(write(0x7FC, three).verdict == Verdict::Refused && main[0x1FF] == Nop);
    // A site that no longer holds what it held when the package was loaded (its load plan patched it since) is refused; one that still does is written
    const u32 held[] = {Nop};
    mprotect(main, 0x8000, PROT_READ | PROT_WRITE | PROT_EXEC);
    main[0x500] = 0x94001234; // the plan's call of its helper
    mprotect(main, 0x8000, PROT_READ | PROT_EXEC);
    o = write(0x1400, mov, held);
    CHECK(o.verdict == Verdict::Refused && main[0x500] == 0x94001234 && o.why.find("load plan") != std::string::npos);
    CHECK(write(0x1404, mov, held).verdict == Verdict::Written && main[0x501] == 0x52800020);
    // The page that was read-write-execute is that again; the code's is read-execute
    CHECK(write(0x8010, mov).verdict == Verdict::Written && main[0x2004] == 0x52800020 && ProtOf(main + 0x8000 / 4) == (PROT_READ | PROT_WRITE | PROT_EXEC) &&
          ProtOf(main) == (PROT_READ | PROT_EXEC));
    target.rewritten = nullptr;
    CHECK(write(0xB00, mov).verdict == Verdict::Refused && main[0x2C0] == Nop);
    munmap(main, size);
    PeanutsPatches::ForgetWritten();
}

// ---- through the entry points the lifted code calls -----------------------------------------------------------------------------------------------------------
u8* g_main{};
constexpr u64 MainSize = 0x40000, TextEnd = 0x10000;
std::string g_rt_log;
void RtLog(int, const char*, const char* message) {
    g_rt_log += message ? message : "";
    g_rt_log += '\n';
}
int RtRead(void*, uint64_t addr, void* buf, size_t len) {
    const u64 base = reinterpret_cast<u64>(g_main);
    if (addr < base || addr + len > base + MainSize) return 0;
    std::memcpy(buf, reinterpret_cast<void*>(addr), len);
    return 1;
}
int RtWrite(void*, uint64_t, const void*, size_t) {
    return 0;
}
int RtRewritten(void*, uint64_t addr, size_t len) {
    const u64 off = addr - reinterpret_cast<u64>(g_main);
    return off < 0x304 && off + len > 0x300; // one rewrite site
}
struct RtGuest {
    int stops{}, resumes{}, stop_answer{1}, parked{}, inside_calls{};
    static int Stop(void* u, uint32_t) {
        auto& g = *static_cast<RtGuest*>(u);
        ++g.stops;
        return g.stop_answer;
    }
    static void Resume(void* u) {
        ++static_cast<RtGuest*>(u)->resumes;
    }
    static int Inside(void* u, uint64_t, uint64_t) {
        auto& g = *static_cast<RtGuest*>(u);
        ++g.inside_calls;
        return g.parked-- > 0 ? 1 : 0;
    }
};
u32 MainWord(u64 offset) {
    u32 w{};
    std::memcpy(&w, g_main + offset, 4);
    return w;
}
void Ticks(PnRt* rt, int n, int ms) {
    for (int i = 0; i < n; ++i) {
        pn_rt_tick(rt);
        std::this_thread::sleep_for(std::chrono::milliseconds{ms});
    }
}

void TestEntryPoints(const std::filesystem::path& tmp) {
    // Texture readback as the runtime reaches it: Tegra::GPU, behind the lifted backend and broker
    {
        g_images.clear();
        g_queued.clear();
        g_images.push_back(MakeImage(0x80000, 256, 256, VKA_RB_F_RGBA8_SRGB, true, true, 50, 0xA0));
        Tegra::GPU gpu;
        CHECK(gpu.DSModTextureReadbackAvailable());
        Core::Mods::TextureReadbackBroker broker{Core::Mods::MakeGpuTextureReadbackBackend(gpu)};
        auto now = Clock::now();
        s32 status = 99;
        EdenDsmodTextureInfo info{};
        info.struct_size = sizeof info;
        std::vector<u8> out(256 * 256 * 4);
        auto rq = Request(EDEN_DSMOD_TEXTURE_RGBA8, 0, 256, 256, EDEN_DSMOD_TEXTURE_REQUIRE_RENDER_TARGET | EDEN_DSMOD_TEXTURE_REQUIRE_SRGB);
        const u64 ticket = broker.Request(&rq, &status, now);
        CHECK(ticket != 0 && status == EDEN_DSMOD_TEXTURE_PENDING);
        Present();
        CHECK(broker.Poll(ticket, out.data(), out.size(), &info, now) == EDEN_DSMOD_TEXTURE_READY && info.gpu_address == 0x80000 && out[5] == g_images[0].texels[5]);
        g_replay_up = false;
        CHECK(!gpu.DSModTextureReadbackAvailable());
        g_replay_up = true;
    }
    // @perf.* and @device.*: Core::PerfStats and Common::GetPowerStatus
    {
        g_frames = 1234, g_system_frames = 1000, g_frametime_ns = 5'000'000'000;
        const auto totals = Core::PerfStats{}.GetTotals();
        CHECK(totals.game_frames == 1234 && totals.system_frames == 1000 && totals.frametime == std::chrono::seconds{5});
        g_power_known = 1, g_power_pct = 41, g_power_chg = 0;
        const auto power = Common::GetPowerStatus();
        const auto reading = Core::Mods::ReadDevicePower();
        CHECK(power.has_battery && power.percentage == 41 && !power.charging && reading.present && reading.pct == 41 && !reading.charging);
        g_power_known = 0;
        CHECK(!Common::GetPowerStatus().has_battery && !Core::Mods::ReadDevicePower().present);
    }
    // aoc: through the lifted source's opener
    {
        const auto aoc = tmp / "aoc-entry";
        std::filesystem::create_directories(aoc / "3");
        WriteFile(aoc / "3" / "three.txt", {'3'});
        setenv("DM_AOC_DIR", aoc.c_str(), 1);
        Core::System system;
        Core::Mods::ReleaseRomfsSources();
        const auto root = Core::Mods::OpenAocRomfs(system, 0x0100000000001000);
        CHECK(root != nullptr && root->GetFileRelative("three.txt") != nullptr);
        Core::Mods::ReleaseRomfsSources();
        unsetenv("DM_AOC_DIR");
    }
    // A package whose manifest declares patches, loaded as the host loads it; the runtime's own tick applies them
    PeanutsPatches::ForgetWritten();
    g_main = static_cast<u8*>(mmap(nullptr, MainSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    for (u64 at = 0; at < TextEnd; at += 4) {
        std::memcpy(g_main + at, &Nop, 4);
    }
    mprotect(g_main, TextEnd, PROT_READ | PROT_EXEC);
    const std::string build = std::string{"909E904AF78AC1B8"} + std::string(48, '0');
    const auto package = tmp / "patched";
    std::filesystem::create_directories(package / "dualscreen");
    std::ofstream(package / "package.json") << R"({"format":1,"type":"dual-screen-mod","title_id":"01006BB00C6F0000","name":"patched","version":"0.0.1","min_runtime":19})";
    std::ofstream(package / "dualscreen" / "manifest.json") << R"({
      "min_runtime": 19, "format": 1, "title_id": "01006BB00C6F0000", "name": "patched",
      "canvas_w": 640, "canvas_h": 360, "background": "#FF102030",
      "pages": [{"id": "one", "title": "one", "widgets": [{"type": "label", "rect": [40, 40, 560, 40], "text": "ONE", "text_scale": 6, "color": "#FFFFFFFF"}]}]})";
    std::ofstream(package / "dualscreen" / "909E904AF78AC1B8.json") << R"({"patches": [
      {"at": "main+0x400", "write": ["52800020"], "why": "one instruction"},
      {"at": "main+0x800", "write": ["52800000", "52800001", "D65F03C0"], "why": "three instructions"},
      {"at": "main+0x300", "write": ["52800020"], "why": "over rewritten code"},
      {"at": "main+0xFFFC", "write": ["52800020", "52800021"], "why": "past the code's end"},
      {"at": "main+0x900", "write": ["52800020"], "why": "optional", "optional": true}]})";
    RtGuest guest;
    const std::string package_dir = package.string();
    PnRtConfig c{};
    c.package_dir = package_dir.c_str();
    c.title_id = 0x01006BB00C6F0000ULL;
    c.build_id_hex = build.c_str();
    c.main_base = reinterpret_cast<u64>(g_main);
    c.main_size = MainSize;
    c.panel_w = 640;
    c.panel_h = 360;
    c.memory = {nullptr, &RtRead, &RtWrite, nullptr, nullptr};
    c.helper.code_rewritten = &RtRewritten;
    c.helper.text_end = TextEnd;
    c.guest = {&guest, &RtGuest::Stop, &RtGuest::Resume, &RtGuest::Inside};
    c.log = &RtLog;
    c.log_level = 2;
    guest.stop_answer = 0; // the game's threads do not stop at first
    PnRt* rt = pn_rt_create(&c);
    CHECK(rt != nullptr);
    if (rt != nullptr) {
        Ticks(rt, 3, 5);
        CHECK(MainWord(0x400) == Nop && MainWord(0x800) == Nop && g_rt_log.find("not written yet") != std::string::npos); // nothing beside the running game
        CHECK(g_rt_log.find("skipping optional patch at main+900") != std::string::npos && MainWord(0x900) == Nop);
        CHECK(g_rt_log.find("patch at main+300 (over rewritten code) not applied") != std::string::npos && MainWord(0x300) == Nop);
        CHECK(g_rt_log.find("patch at main+FFFC (past the code's end) not applied") != std::string::npos && MainWord(0xFFFC) == Nop);
        guest.stop_answer = 1;
        guest.parked = 1; // and then one stands inside the three instructions once
        std::this_thread::sleep_for(std::chrono::milliseconds{260}); // (the host's stop is not tried again at once after one was given up)
        Ticks(rt, 1, 5);
        CHECK(MainWord(0x400) == 0x52800020 && MainWord(0x800) == Nop && guest.inside_calls == 1); // the single one is in; the three wait for the thread to move on
        Ticks(rt, 3, 5);
        CHECK(MainWord(0x800) == 0x52800000 && MainWord(0x804) == 0x52800001 && MainWord(0x808) == 0xD65F03C0 && guest.resumes >= 2);
        CHECK(g_rt_log.find("patched main+400 with 1 instruction(s): one instruction") != std::string::npos &&
              g_rt_log.find("patched main+800 with 3 instruction(s): three instructions") != std::string::npos);
        const int stops = guest.stops;
        Ticks(rt, 5, 2);
        CHECK(guest.stops == stops); // done: no further stop
        pn_rt_destroy(rt);
        // The package loaded again in the same game (replaced, or the runtime started anew): its patches are found in place, nothing is written or stopped
        g_rt_log.clear();
        rt = pn_rt_create(&c);
        CHECK(rt != nullptr);
        if (rt != nullptr) {
            Ticks(rt, 3, 2);
            CHECK(guest.stops == stops && g_rt_log.find("patched main+800 with 3 instruction(s) (already there)") != std::string::npos && MainWord(0x400) == 0x52800020);
            pn_rt_destroy(rt);
        }
    }
    munmap(g_main, MainSize);
    PeanutsPatches::ForgetWritten();
}
} // namespace

// The fake host's C functions (strong definitions of the shims' weak references)
extern "C" int vka_rb_available(void) {
    return g_replay_up ? 1 : 0;
}
extern "C" int vka_rb_submit(const vka_rb_request* rq) {
    if (!g_replay_up || g_queued.size() >= VKA_RB_MAX_IN_FLIGHT || !vka_rb_in_bounds(rq->width, rq->height, rq->format)) {
        return 0;
    }
    ++g_submits;
    g_queued.push_back(*rq);
    return 1;
}
extern "C" int pn_host_power(int* percent, int* charging) {
    *percent = g_power_pct;
    *charging = g_power_chg;
    return g_power_known;
}
extern "C" void pn_host_perf_totals(uint64_t* game_frames, uint64_t* system_frames, int64_t* frametime_ns) {
    *game_frames = g_frames;
    *system_frames = g_system_frames;
    *frametime_ns = g_frametime_ns;
}
extern "C" int pn_host_aoc_open(const char*, int index, PnRtRomfs* out, char* why, size_t why_size) {
    if (index != 1 || g_aoc_container_dir.empty()) {
        std::snprintf(why, why_size, "the fake host has no such container");
        return 0;
    }
    ++g_aoc_opens;
    *out = {&g_aoc_container_dir, &FakeOpen, &FakeRead, &FakeClose, nullptr};
    return 1;
}

int main(int argc, char** argv) {
    std::error_code ec;
    const auto tmp = std::filesystem::temp_directory_path() / ("svc_test." + std::to_string(getpid()));
    std::filesystem::create_directories(tmp);
    TestReadback();
    TestSysfont(tmp, argc > 1 ? argv[1] : nullptr);
    TestAoc(tmp);
    TestHostPure(tmp);
    TestHostValues();
    TestPatches();
    TestEntryPoints(tmp);
    std::filesystem::remove_all(tmp, ec);
    std::printf("svc_test: %d checks, %d failed: %s\n", g_checks, g_failed, g_failed ? "FAILED" : "OK");
    return g_failed ? 1 : 0;
}
