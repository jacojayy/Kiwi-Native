// PNRT: texture readback (runtime 24's module extension) over the host's Vulkan replay. The lifted broker (mods/mod_texture_readback.cpp) keeps every limit of the
// extension (formats, sizes, two tickets at once, two requests a second, a 2 s timeout); this file is its backend: it hands a request to the replay
// (dm/gfx/vk_android/vka_readback.h) and turns the replay's answer into the runtime's result with the runtime's own rules, unchanged:
// VideoCore::DSMod::PickReadbackImage chooses the image among the candidates the replay lists, PackReadbackPixels makes the tight R8 / RGBA8 rows.
// Formats: exactly the emulator's. R8 from an R8 texture; four bytes a pixel, R, G, B, A in memory order, from an RGBA8 or BGRA8 texture, linear or sRGB (the bytes as
// stored, no conversion of the sRGB curve). Block-compressed, depth, float, packed and 16-bit formats, arrays, 3D and multisampled textures answer MISMATCH (by
// address) or NOT_FOUND (by size). Rows are the image's own, first row first. A colour target's sRGB or linear class is that of the view the game draws it
// through (textures of both formats over one storage share one image on this host).
// A request's gpu_address is, on this host, the address of the texture's storage in the game's memory (there is no GPU address space): a module gets it from a
// result and passes it back, as on the emulator; an address taken from the emulator's own GPU address space means nothing here.
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "../../../gfx/vk_android/vka_readback.h"
#include "common/logging.h"
#include "pnrt_services.h"
#include "video_core/surface.h"

// The replay's side (vka_readback.inc), weak: absent from a build without the Vulkan replay
extern "C" int vka_rb_available(void) __attribute__((weak));
extern "C" int vka_rb_submit(const vka_rb_request* rq) __attribute__((weak));

namespace PnShim {
namespace {
using VideoCore::DSMod::ReadbackCandidate;
using VideoCore::DSMod::ReadbackFormat;
using VideoCore::DSMod::ReadbackRequest;
using VideoCore::DSMod::ReadbackResult;
using VideoCore::DSMod::ReadbackStatus;
using VideoCore::Surface::PixelFormat;

static_assert(VKA_RB_R8 == static_cast<u32>(ReadbackFormat::R8) && VKA_RB_RGBA8 == static_cast<u32>(ReadbackFormat::RGBA8));
static_assert(VKA_RB_READY == static_cast<s32>(ReadbackStatus::Ready) && VKA_RB_NOT_FOUND == static_cast<s32>(ReadbackStatus::NotFound) &&
              VKA_RB_MISMATCH == static_cast<s32>(ReadbackStatus::Mismatch) && VKA_RB_UNSUPPORTED == static_cast<s32>(ReadbackStatus::Unsupported) &&
              VKA_RB_FAILED == static_cast<s32>(ReadbackStatus::Failed));
static_assert(VKA_RB_MAX_SIDE == VideoCore::DSMod::ReadbackMaxSide && VKA_RB_MAX_BYTES == VideoCore::DSMod::ReadbackMaxBytes);

PixelFormat GuestFormat(u8 format_class) {
    switch (format_class) {
    case VKA_RB_F_R8:
        return PixelFormat::R8_UNORM;
    case VKA_RB_F_RGBA8:
        return PixelFormat::A8B8G8R8_UNORM;
    case VKA_RB_F_RGBA8_SRGB:
        return PixelFormat::A8B8G8R8_SRGB;
    case VKA_RB_F_BGRA8:
        return PixelFormat::B8G8R8A8_UNORM;
    case VKA_RB_F_BGRA8_SRGB:
        return PixelFormat::B8G8R8A8_SRGB;
    default:
        return PixelFormat::Invalid; // never matches a request's format family
    }
}

struct Pending {
    ReadbackRequest request;
    VideoCore::DSMod::ReadbackCallback callback;
};

// The replay's present thread, its texture cache locked: only the runtime's own rule runs here, over a fixed array (nothing is allocated under that lock).
vka_rb_pick Pick(void* user, const vka_rb_cand* c, uint32_t n) {
    const auto& pending = *static_cast<const Pending*>(user);
    std::array<ReadbackCandidate, VKA_RB_MAX_CAND> images;
    n = std::min<uint32_t>(n, VKA_RB_MAX_CAND);
    for (uint32_t i = 0; i < n; ++i) {
        images[i] = {
            .gpu_addr = c[i].addr,
            .width = c[i].width,
            .height = c[i].height,
            .format = GuestFormat(c[i].format),
            .plain_2d = c[i].plain_2d != 0,
            .current = c[i].current != 0,
            .generation = c[i].generation,
            .render_target = c[i].render_target != 0,
            .redrawn = c[i].redrawn != 0,
        };
    }
    const auto pick = VideoCore::DSMod::PickReadbackImage(pending.request, std::span<const ReadbackCandidate>{images.data(), n});
    return {static_cast<int32_t>(pick.status), static_cast<uint32_t>(pick.index), pick.candidates};
}

void Done(void* user, const vka_rb_result* r) {
    const std::unique_ptr<Pending> pending{static_cast<Pending*>(user)};
    ReadbackResult result;
    result.status = static_cast<ReadbackStatus>(r->status);
    result.gpu_addr = r->addr;
    result.width = r->width;
    result.height = r->height;
    result.generation = r->generation;
    result.candidates = r->candidates;
    if (result.status == ReadbackStatus::Ready &&
        (r->rows == nullptr ||
         !VideoCore::DSMod::PackReadbackPixels(pending->request.format, r->bgra != 0, r->width, r->height,
                                               std::span<const u8>{r->rows, r->row_bytes * r->height}, r->row_bytes, result.pixels))) {
        result.status = ReadbackStatus::Failed;
        result.pixels.clear();
    }
    try {
        pending->callback(std::move(result));
    } catch (...) {
        LOG_ERROR(Core, "peanuts_rt: texture readback: the completion callback threw");
    }
}

#if !defined(__ANDROID__)
// PC test programs only (rt_test with a real package): PNRT_FAKE_TEXTURE=<w>x<h>:<file> stands for the replay with one live texture, an sRGB RGBA8 colour target
// drawn in every frame at storage address 0x7000000, whose pixels are the file's first w*h*4 bytes (read at each request). The request goes through Pick and Done
// as a real one does and is answered before Submit returns, which the broker allows.
bool FakeSubmit(Pending* pending) {
    const char* const spec = std::getenv("PNRT_FAKE_TEXTURE");
    unsigned w = 0, h = 0;
    int at = 0;
    if (spec == nullptr || std::sscanf(spec, "%ux%u:%n", &w, &h, &at) < 2 || at == 0 || w == 0 || h == 0 || w > VKA_RB_MAX_SIDE || h > VKA_RB_MAX_SIDE) {
        return false;
    }
    static u64 generation = 0;
    std::vector<u8> pixels(size_t{w} * h * 4);
    bool have = false;
    if (std::FILE* const f = std::fopen(spec + at, "rb")) {
        have = std::fread(pixels.data(), 1, pixels.size(), f) == pixels.size();
        std::fclose(f);
    }
    const vka_rb_cand cand{0x7000000, w, h, VKA_RB_F_RGBA8_SRGB, 1, static_cast<uint8_t>(have), 1, 1, ++generation};
    const vka_rb_pick pick = Pick(pending, &cand, 1);
    vka_rb_result result{pick.status, 0, pending->request.width, pending->request.height, 0, pick.candidates, nullptr, 0, 0};
    if (pick.status == VKA_RB_READY) {
        if (pending->request.format != ReadbackFormat::RGBA8) {
            result.status = VKA_RB_UNSUPPORTED;
        } else {
            result.addr = cand.addr;
            result.generation = cand.generation;
            result.rows = pixels.data();
            result.row_bytes = size_t{w} * 4;
        }
    }
    Done(pending, &result);
    return true;
}
bool FakeWanted() {
    return std::getenv("PNRT_FAKE_TEXTURE") != nullptr;
}
#else
bool FakeSubmit(Pending*) {
    return false;
}
bool FakeWanted() {
    return false;
}
#endif
} // namespace

bool ReadbackAvailable() {
    if (&vka_rb_available != nullptr && &vka_rb_submit != nullptr) {
        return vka_rb_available() != 0;
    }
    return FakeWanted();
}

bool ReadbackSubmit(const ReadbackRequest& request, VideoCore::DSMod::ReadbackCallback callback) {
    if (!callback || !ReadbackAvailable()) {
        return false;
    }
    auto pending = std::make_unique<Pending>(Pending{request, std::move(callback)});
    if (&vka_rb_submit == nullptr) {
        if (FakeSubmit(pending.get())) {
            (void)pending.release(); // Done owned and freed it
            return true;
        }
        return false;
    }
    const vka_rb_request rq{
        .addr = request.gpu_addr,
        .width = request.width,
        .height = request.height,
        .format = static_cast<uint32_t>(request.format),
        .pick = &Pick,
        .done = &Done,
        .user = pending.get(),
    };
    if (vka_rb_submit(&rq) == 0) {
        return false; // both slots busy, or the replay stopped: the broker answers UNSUPPORTED and the module asks again
    }
    (void)pending.release(); // Done frees it: the replay answers every request it took (one no present takes within the timeout as FAILED, at a later request)
    return true;
}

} // namespace PnShim
