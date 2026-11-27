// PNRT: a package's load plan (mailbox, patches of main, guest helper code) applied to the running game: kiwi_helper.cpp
#pragma once

#include <array>
#include <string>

#include "common/common_types.h"
#include "kiwi_rt.h"

namespace Core {
class System;
}

namespace KiwiHelper {

struct Result {
    bool installed{};  ///< the plan is in place and the mailbox is set on the system
    std::string error; ///< why not, when the package has a plan for this title; empty when it has none
    u64 epoch_address{}; ///< the mailbox's epoch word (0: the plan has none), for the bump at pause and resume
    u32 epoch{};         ///< the value written there at install
};

/// Reads the selected package's load plan for this build of main and applies it to the running process.
[[nodiscard]] Result Install(Core::System& system, const PnRtConfig& config, const std::array<u8, 0x20>& build_id);

} // namespace KiwiHelper
