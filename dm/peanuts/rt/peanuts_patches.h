// PNRT: a package manifest's "patches" (instructions written over main once, Core::Mods::GuestPatch) applied to the running game: peanuts_patches.cpp.
// On the emulator ModRuntime::ApplyPatches (mod_guest_bridge.cpp, not lifted) writes them when main is mapped. Here the game already runs, as rewritten code, so a
// patch is written only where main still holds the file's own instructions, the way peanuts_helper.cpp places a load plan's patches.
#pragma once

#include <functional>
#include <span>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "peanuts_rt.h"

namespace Core {
class System;
}
namespace Core::Mods {
struct GuestPatch;
}

namespace PeanutsPatches {

/// What the host said about main, and what main holds at each patch's site now. pn_rt_create calls this once the manifest is read and BEFORE the package's load
/// plan is installed: a site that no longer holds these words when the patch is due was written by the plan or its helper, and the patch is refused.
void Configure(const PnRtConfig& config, const std::vector<Core::Mods::GuestPatch>& patches);

/// The body of ModRuntime::ApplyPatches (called every tick until `applied`): each patch of `patches` is written, skipped or refused once, with one log line saying
/// which and why; one that could not be written just now (the game's threads did not stop, or one stood inside the patch) is tried again on later ticks, up to
/// RetryBudget times. `done` and `applied` are the runtime's own patch_done / patches_applied.
void Apply(Core::System& system, const std::vector<Core::Mods::GuestPatch>& patches, std::vector<bool>& done, bool& applied);
inline constexpr u32 RetryBudget = 600; ///< about ten seconds of ticks

/// Where one patch goes and how (Apply fills it from Configure's values and the system; tests fill their own).
struct Target {
    u64 base{};     ///< main's address
    u64 text_end{}; ///< the length of main's code from its base: a patch lies wholly below it
    /// Whether [addr, addr + len) holds an instruction the host's rewriter changed or moved, or the host cannot tell.
    std::function<bool(u64 addr, size_t len)> rewritten;
    /// Runs `fn` with the game's threads stopped; false when they could not be stopped (then `fn` did not run). `fn` allocates nothing and takes no lock.
    std::function<bool(const std::function<void()>& fn)> stopped;
    /// Optional, inside `stopped`: how many stopped threads are at an instruction in [lo, hi).
    std::function<int(u64 lo, u64 hi)> stopped_in;
};
enum class Verdict { Written, Retry, Refused };
struct Outcome {
    Verdict verdict{Verdict::Refused};
    std::string why; ///< Retry / Refused: the reason. Written: empty, or "already there"
};
/// Writes `words` at base + at, or says why not (nothing is written then). `expected`, when not empty, is what the site held when the package was loaded.
/// Every word written is remembered for the life of the process: a second write of the same words is "already there", another over them is refused.
[[nodiscard]] Outcome Write(const Target& target, s64 at, std::span<const u32> words, std::span<const u32> expected = {});
/// Whether an instruction may be replaced by another while a thread may be executing it, without stopping the threads: both must be one of B, BL, NOP, BRK, SVC,
/// HVC, SMC, ISB (the set the architecture allows to be modified under concurrent execution).
[[nodiscard]] bool SafeWhileRunning(u32 old_word, u32 new_word);
/// Tests: forgets what this process wrote.
void ForgetWritten();

} // namespace PeanutsPatches
