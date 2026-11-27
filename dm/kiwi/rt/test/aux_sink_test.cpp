// Host test of the UiSink hooks in shim/video_core/dsmod/aux_routing.h (the PNRT hunks).
//
// On this host the page reaches the screen through the sink alone: nothing calls TakeUi, so the
// accumulated dirty box (ui_dirty) is never consumed. A partial publish must hand the sink the box
// of that publish, not everything accumulated since the first whole-page publish.
//
//   aux_sink_test          exit 0 = every check passed
#include <array>
#include <cstdio>
#include <memory>
#include <vector>

#include "video_core/dsmod/aux_routing.h"

namespace {

struct Seen {
    int full{};
    int partial{};
    std::array<s32, 4> last{};
    u64 last_area{};
};

void OnFull(void* user, u32, u32, const u32*) {
    static_cast<Seen*>(user)->full++;
}

void OnPartial(void* user, u32, u32, const u32*, const s32 rect[4]) {
    auto& seen = *static_cast<Seen*>(user);
    seen.partial++;
    seen.last = {rect[0], rect[1], rect[2], rect[3]};
    seen.last_area = static_cast<u64>(rect[2]) * static_cast<u64>(rect[3]);
}

int failures = 0;

void Check(bool ok, const char* what) {
    std::printf("aux_sink_test: %s: %s\n", ok ? "ok" : "FAILED", what);
    failures += !ok;
}

} // namespace

int main() {
    constexpr u32 W = 1240, H = 1080;
    auto aux = std::make_unique<VideoCore::DSMod::AuxRouting>();
    Seen seen;
    aux->SetUiSink({&seen, &OnFull, &OnPartial, nullptr});

    std::vector<u32> page(static_cast<size_t>(W) * H, 0xFF102030u);
    aux->PublishUi(W, H, page);
    Check(seen.full == 1 && seen.partial == 0, "the whole-page publish reaches the sink once, whole");

    // Three small partial publishes at different places, each changing its pixels.
    const std::array<std::array<s32, 4>, 3> rects{{{100, 200, 48, 32}, {600, 500, 64, 64}, {1100, 1000, 40, 24}}};
    u32 colour = 0xFFE06060u;
    for (const auto& rect : rects) {
        for (s32 y = rect[1]; y < rect[1] + rect[3]; ++y) {
            for (s32 x = rect[0]; x < rect[0] + rect[2]; ++x) {
                page[static_cast<size_t>(y) * W + static_cast<size_t>(x)] = colour;
            }
        }
        colour += 0x00101010u;
        const int before = seen.partial;
        aux->PublishUiPartial(W, H, page, rect);
        std::printf("aux_sink_test: partial publish [%d,%d,%d,%d] -> sink [%d,%d,%d,%d]\n", rect[0],
                    rect[1], rect[2], rect[3], seen.last[0], seen.last[1], seen.last[2],
                    seen.last[3]);
        Check(seen.partial == before + 1, "a partial publish reaches the sink once");
        // The sink's box holds the publish's rect and is no larger than it (the tile diff may
        // trim it to the pixels that changed, never widen it past the rect asked for).
        const bool inside = seen.last[0] >= rect[0] && seen.last[1] >= rect[1] &&
                            seen.last[0] + seen.last[2] <= rect[0] + rect[2] &&
                            seen.last[1] + seen.last[3] <= rect[1] + rect[3];
        Check(inside && seen.last_area > 0, "the sink is handed this publish's box, not the accumulated one");
    }

    // A publish that changes nothing does not reach the sink, and leaves the next one's box alone.
    const int before = seen.partial;
    aux->PublishUiPartial(W, H, page, rects[0]);
    Check(seen.partial == before, "an unchanged partial publish is not forwarded");

    // TakeUi still sees everything accumulated (the consumer path of a host that uses it).
    std::vector<u32> out;
    u32 w{}, h{};
    u64 serial{};
    Check(aux->TakeUi(out, w, h, serial) && w == W && h == H && out == page,
          "TakeUi returns the whole current page");

    std::printf("aux_sink_test: %s\n", failures == 0 ? "OK" : "FAILED");
    return failures == 0 ? 0 : 1;
}
