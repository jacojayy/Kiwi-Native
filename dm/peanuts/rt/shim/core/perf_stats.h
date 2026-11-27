// PNRT shim for Eden's core/perf_stats.h: the runtime 21 @perf.* source reads GetTotals(). The
// host has no frame counters for the guest here (they stay at zero -> no @perf values published).
#pragma once

#include <chrono>

#include "common/common_types.h"
#include "pnrt_services.h"

namespace Core {

class PerfStats {
public:
    using Clock = std::chrono::steady_clock;
    struct Totals {
        u64 game_frames{};
        u64 system_frames{};
        Clock::duration frametime{};
    };
    [[nodiscard]] Totals GetTotals() const {
        const auto t = PnShim::ReadPerfTotals();
        return {t.game_frames, t.system_frames, std::chrono::nanoseconds{t.frametime_ns}};
    }
    Totals totals{};
};

} // namespace Core
