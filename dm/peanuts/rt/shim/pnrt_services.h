// PNRT: the host services behind the shims that used to answer "unavailable" (shim_readback.cpp, shim_services.cpp). Each function is what one shim body calls:
//   video_core/gpu.h          DSModTextureReadbackAvailable / DSModRequestTextureReadback -> ReadbackAvailable / ReadbackSubmit
//   peanuts_rt.cpp            sys.sysfont_root (no sysfont_dir in the config)              -> SysfontRoot
//   mods/mod_romfs_sources.cpp BuildAoc (the PNRT branch)                                  -> AocRoot
//   shim_r5.cpp               Common::GetPowerStatus                                       -> PowerStatus
//   core/perf_stats.h         PerfStats::GetTotals                                         -> ReadPerfTotals
// The host's side is reached through weak C symbols (declared in the .cpp files): the Android host defines them (dm/gfx/vk_android/vka_readback.inc,
// dm/peanuts/peanuts_services.c); a build without them (the PC test programs, unless a test defines its own) answers "unavailable" exactly as before.
#pragma once

#include <string>

#include "common/common_types.h"
#include "common/device_power_state.h"
#include "core/file_sys/vfs/vfs_types.h"
#include "video_core/dsmod/texture_readback.h"

namespace PnShim {

/// Whether a texture readback can be asked for now (the Vulkan replay runs and the GPU has not hung).
[[nodiscard]] bool ReadbackAvailable();
/// Starts one readback without blocking. True: `callback` runs exactly once later: with pixels on the replay's hand-over thread, without on its present
/// thread, or inside a later ReadbackSubmit when no present took the request within the timeout. False: not started, never called.
bool ReadbackSubmit(const VideoCore::DSMod::ReadbackRequest& request, VideoCore::DSMod::ReadbackCallback callback);

/// The console's shared fonts for "sysfont:", as a directory of the firmware's file names ("nintendo_udsg-r_ko_003.bfttf", ...) in the firmware's stored form, made
/// on demand from the TrueType files the host serves the game as its shared fonts: DM_SHARED_FONT (the standard font), DM_SHARED_FONT_EXT (the Nintendo extension)
/// and cjk_sc.ttf / cjk_sc_ext.ttf / cjk_tc.ttf / cjk_kr.ttf beside the standard font. Never a file in the stand-ins' folder DM_FONT_DIR, and no file over
/// 64 MiB. nullptr without DM_SHARED_FONT.
[[nodiscard]] FileSys::VirtualDir SysfontRoot();
/// The same over explicit paths (tests): the standard font's file and the extension's ("" = none).
[[nodiscard]] FileSys::VirtualDir SysfontRootOf(const std::string& standard_ttf, const std::string& extension_ttf);

/// The RomFS root of the running game's add-on content for "aoc:": the lowest index of DM_AOC_DIR (a folder, or a .nup / dump the host opens), as the emulator
/// takes the lowest add-on id. nullptr when there is none, or when that index cannot be read here (a RomFS image, a container that does not open): never a
/// higher index in its place.
[[nodiscard]] FileSys::VirtualDir AocRoot(u64 program_id);

/// The device's battery: the level and charging state the app pushed (Bridge.battery), else the kernel's power supply files; has_battery false when neither answers.
[[nodiscard]] Common::PowerStatus PowerStatus();

struct PerfTotals {
    u64 game_frames{};   ///< game frames presented since the start
    u64 system_frames{}; ///< frames whose GPU time is counted in frametime_ns
    s64 frametime_ns{};  ///< the GPU's busy time summed over those frames
};
/// Running totals for "@perf.*" (all zero without a host).
[[nodiscard]] PerfTotals ReadPerfTotals();

} // namespace PnShim
