// peanuts_rt C API: builds the host facade (Core::System shim) from PnRtConfig, discovers the
// package with the lifted runtime's own Discover() and drives ModRuntime through the fake core
// timing (pn_rt_tick) and the AuxRouting sink (pn_rt_frame).

#include "peanuts_patches.h"
#include "pnrt_services.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/fs/path_util.h"
#include "common/hex_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/mods/mod_launch_gate.h"
#include "core/mods/mod_module.h"
#include "core/mods/mod_runtime.h"
#include "peanuts_helper.h"
#include "peanuts_rt.h"

namespace Core::Mods {
/// Friend of ModRuntime (mod_runtime.h, PNRT line): the few private members the C API needs.
struct PnRtAccess {
    static bool SetPage(ModRuntime& rt, const char* id) {
        for (size_t i = 0; i < rt.manifest.pages.size(); ++i) {
            if (rt.manifest.pages[i].id == id) {
                if (rt.current_page != i) {
                    rt.current_page = i;
                    rt.page_anim_request.reset();
                    rt.ui_signature_valid = false;
                    rt.widget_sig_page = ~size_t{0};
                }
                return true;
            }
        }
        return false;
    }
    static std::string PageId(const ModRuntime& rt) {
        if (rt.manifest.pages.empty()) {
            return "-";
        }
        return rt.manifest.pages[std::min(rt.current_page, rt.manifest.pages.size() - 1)].id;
    }
    /// The end of the heap the runtime found (HeapLow's cached bounds) follows a heap the game opened further; it never moves
    /// back. Nothing to do before the first ask: that one reads the page table's fields as they are then.
    static void RaiseHeapHigh(ModRuntime& rt, u64 high) {
        std::scoped_lock lock{rt.heap_bounds_mutex};
        if (rt.heap_low.load(std::memory_order_relaxed) != 0 && rt.heap_high.load(std::memory_order_relaxed) < high) {
            rt.heap_high.store(high, std::memory_order_release);
        }
    }
    static u64 Ticks(const ModRuntime& rt) {
        return rt.tick_count;
    }
    static const std::string& Name(const ModRuntime& rt) {
        return rt.manifest.name;
    }
    /// R5: "<module name>" when a native module instance runs, "none" when the package has no
    /// module, else the loader's / module's error text.
    static std::string Module(const ModRuntime& rt) {
        if (rt.game_module && rt.game_module_instance) {
            const auto* const api = rt.game_module->Api();
            return api && api->name ? api->name : "unnamed";
        }
        return rt.module_error.empty() ? "none" : "error: " + rt.module_error;
    }
};
} // namespace Core::Mods

struct PnRt {
    PnRtConfig config{};
    PnRtMemory memory{};
    PnRtPad pad{};
    PnRtHaptics haptics{};
    PnRtRomfs romfs{}; // R6: the host's RomFS callbacks (copied; the VFS keeps its own copy too)
    Core::System system;
    std::optional<Core::Mods::ModRuntime> runtime;
    std::string last_error;
    u64 frame_serial{0};
    u64 frames_taken{0};
    std::vector<u32> frame_scratch;
    // Live fingers (pn_rt_touch), fed to the aux touch mailbox as the full current set each call,
    // exactly as Eden's SDL3/Android hosts do (yuzu_cmd/emu_window/emu_window_sdl3_aux.cpp,
    // android/.../native_input.cpp: one SetTouch per input event, carrying every finger still
    // down, bit0 set only for a finger just starting).
    std::vector<VideoCore::DSMod::AuxTouchPoint> live_touches;
    /// live_touches is the UI thread's (pn_rt_touch) and whoever hides the page (pn_rt_set_present):
    /// both take this.
    std::mutex touch_mutex;
    /// The load plan's mailbox epoch. Eden moves it on at every pause and resume (core.cpp
    /// InvalidateDualScreenGuestMailbox): the word in the guest's mailbox, and the value a module
    /// reads as "__mailbox_epoch". pn_rt_lifecycle (any thread) writes the guest word with an atomic
    /// exchange and this atomic; pn_rt_tick copies it into the system, whose field only the tick
    /// thread touches. epoch_address is set once in pn_rt_create (0: the plan has no epoch word).
    u64 epoch_address{};
    std::atomic<u32> epoch{0};
    /// pn_rt_status's line. The runtime's page, name and module error are the tick thread's (a module that fails mid-run
    /// rewrites its error text), so the tick thread writes the line here after each tick (and pn_rt_create, pn_rt_set_page,
    /// which are that thread's too) and pn_rt_status, any thread, copies it. Both under status_mutex.
    std::mutex status_mutex;
    std::string status{"no runtime"};
};

namespace {
/// The tick thread only: reads the runtime, then stores the line under the lock.
void RefreshStatus(PnRt& rt) {
    char line[512];
    if (!rt.runtime) {
        std::snprintf(line, sizeof(line), "no runtime");
    } else {
        const auto& r = *rt.runtime;
        std::snprintf(line, sizeof(line), "%s page=%s tick=%llu frames=%llu module=%s err=%s", Core::Mods::PnRtAccess::Name(r).c_str(),
                      Core::Mods::PnRtAccess::PageId(r).c_str(), static_cast<unsigned long long>(Core::Mods::PnRtAccess::Ticks(r)),
                      static_cast<unsigned long long>(rt.frames_taken), Core::Mods::PnRtAccess::Module(r).c_str(),
                      rt.last_error.empty() ? "-" : rt.last_error.c_str());
    }
    std::scoped_lock lock{rt.status_mutex};
    rt.status = line;
}
} // namespace

namespace {

bool MemRead(void* user, u64 addr, void* buf, std::size_t len) {
    auto* const m = static_cast<PnRtMemory*>(user);
    return m->read != nullptr && m->read(m->user, addr, buf, len) != 0;
}
bool MemWrite(void* user, u64 addr, const void* buf, std::size_t len) {
    auto* const m = static_cast<PnRtMemory*>(user);
    return m->write != nullptr && m->write(m->user, addr, buf, len) != 0;
}
u8* MemPointer(void* user, u64 addr, std::size_t len) {
    auto* const m = static_cast<PnRtMemory*>(user);
    return m->pointer_at != nullptr ? m->pointer_at(m->user, addr, len) : nullptr;
}
void PadButton(void* user, std::size_t player, int button, bool pressed) {
    auto* const p = static_cast<PnRtPad*>(user);
    if (p->set_button) p->set_button(p->user, player, button, pressed ? 1 : 0);
}
void PadStick(void* user, std::size_t player, int stick, float x, float y) {
    auto* const p = static_cast<PnRtPad*>(user);
    if (p->set_stick) p->set_stick(p->user, player, stick, x, y);
}

// Eden's HapticEvent{strength, kind, flags} -> amplitude/duration. Eden itself hands this exact
// triple to a frontend callback (android/.../native.cpp PlayAuxHaptic -> NativeLibrary.onAuxHaptic)
// and lets Kotlin pick an amplitude/duration from the Android Vibrator API; there is no portable
// C++ table to lift, so this is PNRT's own reasonable mapping (not reproduced from Eden).
constexpr std::array<std::pair<float, uint32_t>, 6> kHapticAmplitudeMs{{
    {0.0f, 0u},   // Off
    {0.20f, 18u}, // Light
    {0.35f, 22u}, // Click
    {0.50f, 32u}, // Confirm
    {0.80f, 45u}, // Heavy
    {0.65f, 55u}, // Reject
}};

std::array<u8, 0x20> ParseBuildId(const char* hex) {
    std::array<u8, 0x20> out{};
    if (hex == nullptr) {
        return out;
    }
    std::string text{hex};
    if (text.size() % 2 != 0) {
        text.push_back('0');
    }
    const auto bytes = Common::HexStringToVector(text, false);
    std::copy_n(bytes.begin(), std::min(bytes.size(), out.size()), out.begin());
    return out;
}

} // namespace

extern "C" {

PnRt* pn_rt_create(const PnRtConfig* config) {
    if (config == nullptr || config->package_dir == nullptr) {
        return nullptr;
    }
    if (config->log != nullptr) {
        Common::Log::SetSink(config->log);
    }
    Common::Log::SetMinLevel(static_cast<Common::Log::Level>(std::clamp(config->log_level, 0, 5)));
    // Eden's launch gate, where Eden reads it: core.cpp skips discovery and the runtime, and the loader's SelectLoadPlanPackage
    // selects no plan. Here both happen below this line (Discover, PeanutsHelper::Install), so one check serves.
    if (Core::Mods::CompanionsDisabledForLaunch()) {
        LOG_INFO(Core, "peanuts_rt: companions are off for this launch: no package is selected, no load plan is applied");
        return nullptr;
    }

    Settings::values.language_index.SetValue(
        config->language >= 1 && config->language <= 20
            ? static_cast<Settings::Language>(config->language - 1)
            : Settings::Language::EnglishAmerican);

    auto rt = std::make_unique<PnRt>();
    rt->config = *config;
    rt->memory = config->memory;
    rt->pad = config->pad;
    rt->haptics = config->haptics;
    auto& sys = rt->system;
    PnShim::SetGuest(config->guest); // before the helper below and the runtime: both may ask for a stop
    sys.title_id = config->title_id;
    sys.memory.SetBackend({&rt->memory, &MemRead, &MemWrite, &MemPointer});
    sys.input.pad.SetSink({&rt->pad, &PadButton, &PadStick});
    PnShim::SetPadReader(config->pad);
    sys.fs_controller.load_root = FileSys::MakeSyntheticDir("load", {config->package_dir});
    rt->romfs = config->romfs;
    if (config->romfs_dir != nullptr) {
        sys.romfs_root = FileSys::OpenHostDirectory(config->romfs_dir);
        if (!sys.romfs_root) {
            LOG_WARNING(Core, "peanuts_rt: romfs_dir '{}' is not a directory", config->romfs_dir);
        }
    }
    // R6: a RomFS served by host callbacks (a .nup container) when there is no directory.
    if (!sys.romfs_root && rt->romfs.open != nullptr && rt->romfs.read != nullptr) {
        sys.romfs_root = FileSys::OpenProviderDirectory(rt->romfs);
        LOG_INFO(Core, "peanuts_rt: game romfs served by the host's provider callbacks{}",
                 rt->romfs.list != nullptr ? "" : " (no directory listing)");
    }
    // The mods' folders over the game's RomFS, as the game reads them ("romfs:"); "base:" keeps the game's own files
    sys.romfs_base_root = sys.romfs_root;
    if (config->romfs_overlay_dirs != nullptr && *config->romfs_overlay_dirs != 0) {
        std::vector<FileSys::VirtualDir> layers;
        const std::string all{config->romfs_overlay_dirs};
        for (size_t at = 0; at <= all.size();) {
            const size_t end = std::min(all.find(':', at), all.size());
            if (end > at) {
                if (auto d = FileSys::OpenHostDirectory(all.substr(at, end - at))) layers.push_back(std::move(d));
            }
            at = end + 1;
        }
        if (!layers.empty()) {
            const size_t n = layers.size();
            if (sys.romfs_root) layers.push_back(sys.romfs_root);
            sys.romfs_root = FileSys::MakeLayeredDir(std::move(layers));
            LOG_INFO(Core, "peanuts_rt: game romfs under {} mod folder(s): {}", n, all);
        }
    }
    if (config->save_dir != nullptr) {
        sys.save_root = FileSys::OpenHostDirectory(config->save_dir);
    }
    if (config->sysfont_dir == nullptr) {
        sys.sysfont_root = PnShim::SysfontRoot(); // the firmware fonts the host serves the game (DM_SHARED_FONT), for "sysfont:"
    } else {
        sys.sysfont_root = FileSys::OpenHostDirectory(config->sysfont_dir);
    }
    // R5: the data directory behind Common::FS::GetEdenPath (persisted flags, "user:" assets) and
    // the private staging directory native modules are copied to before dlopen (mod_module.cpp
    // refuses to run on Android without one).
    {
        std::filesystem::path data = config->data_dir != nullptr ? config->data_dir : "";
        if (data.empty()) {
            std::error_code ec;
            data = std::filesystem::temp_directory_path(ec) / "peanuts_rt";
        }
        Common::FS::SetEdenDir(data);
        std::error_code ec;
        std::filesystem::create_directories(data / "dualscreen-code", ec);
        Core::Mods::SetModuleCacheDirectory(data / "dualscreen-code");
    }
    auto& aux = sys.gpu.DSModAux();
    aux.width.store(config->panel_w != 0 ? config->panel_w : 1280);
    aux.height.store(config->panel_h != 0 ? config->panel_h : 720);
    aux.present.store(true);
    aux.SetUiSink({config->ui.user, config->ui.publish_ui, config->ui.publish_ui_partial,
                   config->ui.clear_ui});
    aux.SetHapticSink([haptics = &rt->haptics](const VideoCore::DSMod::AuxRouting::HapticEvent& e) {
        if (haptics->pulse == nullptr) {
            return;
        }
        const size_t idx = std::min<size_t>(e.strength, kHapticAmplitudeMs.size() - 1);
        const auto [amplitude, ms] = kHapticAmplitudeMs[idx];
        if (amplitude <= 0.0f || ms == 0) {
            return;
        }
        haptics->pulse(haptics->user, amplitude, ms, (e.flags & 1) ? 1 : 0, static_cast<int>(e.kind));
    });

    const auto build_id = ParseBuildId(config->build_id_hex);
    auto manifest = Core::Mods::ModRuntime::Discover(sys, config->title_id, build_id);
    if (!manifest) {
        LOG_ERROR(Core, "peanuts_rt: no usable dual-screen package in '{}' for title {:016X}",
                  config->package_dir, config->title_id);
        return nullptr;
    }
    // The package's load plan (its mailbox and guest helper), before the runtime loads the package's module: the module
    // takes the mailbox from the system when it is configured (mod_module_services.cpp)
    // A manifest's patches: what main holds at their sites is noted before the load plan writes anything (peanuts_patches.h)
    PeanutsPatches::Configure(*config, manifest->patches);
    if (config->helper.code_rewritten != nullptr) {
        const auto helper = PeanutsHelper::Install(sys, *config, build_id);
        if (!helper.installed && !helper.error.empty()) {
            LOG_WARNING(Core, "peanuts_rt: the package's load plan is not installed (no guest helper, no mailbox): {}", helper.error);
        }
        if (helper.installed) {
            rt->epoch_address = helper.epoch_address;
            rt->epoch.store(helper.epoch, std::memory_order_release);
        }
    }
    rt->runtime.emplace(sys, std::move(*manifest));
    rt->runtime->SetMainMemoryParameters(config->main_base, config->main_size);
    rt->runtime->Initialize();
    RefreshStatus(*rt);
    return rt.release();
}

void pn_rt_destroy(PnRt* rt) {
    delete rt;
}

void pn_rt_tick(PnRt* rt) {
    if (rt == nullptr) {
        return;
    }
    // The game's heap, once it has one (it opens it after this runtime starts): ModRuntime::HeapLow asks the page table for it.
    // A game that opens more of it later (SetHeapSize again) moves the end: a module's sweep to get_heap_end then covers the new part
    if (rt->memory.heap_range != nullptr) {
        if (auto* const process = rt->system.ApplicationProcess(); process != nullptr) {
            constexpr u64 Slack = 0x40000000ULL; // the same margin HeapLow adds
            uint64_t base = 0, open = 0;
            rt->memory.heap_range(rt->memory.user, &base, &open);
            auto& pt = process->GetPageTable();
            if (base != 0 && open != 0 && pt.GetHeapRegionStart() == 0) {
                pt.normal_memory_size = open;
                pt.heap_size = open + Slack;
                pt.heap_start = base;
            } else if (base != 0 && base == pt.GetHeapRegionStart() && open > pt.GetNormalMemorySize()) {
                pt.heap_size = open + Slack;
                pt.normal_memory_size = open;
                if (rt->runtime) {
                    Core::Mods::PnRtAccess::RaiseHeapHigh(*rt->runtime, base + open + Slack);
                }
            }
        }
    }
    // The mailbox epoch as pn_rt_lifecycle left it, before the module's tick asks for it
    if (rt->epoch_address != 0) {
        const u32 epoch = rt->epoch.load(std::memory_order_acquire);
        if (rt->system.GetDualScreenGuestMailboxEpoch() != epoch) {
            const auto [address, size] = rt->system.GetDualScreenGuestMailbox();
            rt->system.SetDualScreenGuestMailbox(address, size, epoch);
        }
    }
    PnShim::NoteTick(); // the pad gate's liveness (pn_rt_pad_gate)
    rt->system.core_timing.FireAll();
    RefreshStatus(*rt);
}

void pn_rt_set_panel(PnRt* rt, uint32_t w, uint32_t h) {
    if (rt == nullptr || w == 0 || h == 0) {
        return;
    }
    auto& aux = rt->system.gpu.DSModAux(); // two atomics, as Eden's renderer stores them when its second window resizes
    aux.width.store(w);
    aux.height.store(h);
}

void pn_rt_set_companions_disabled(int disabled) {
    Core::Mods::SetCompanionsDisabledForLaunch(disabled != 0);
}

void pn_rt_set_present(PnRt* rt, int shown) {
    if (rt == nullptr) {
        return;
    }
    auto& aux = rt->system.gpu.DSModAux();
    if (shown) {
        aux.present.store(true);
        return;
    }
    // As Eden when its second surface goes (vk_dsmod_aux.cpp, aux_routing.h): hidden first, then no finger down. The
    // mailbox keeps each lost finger as "ended" for the runtime's next look at it, so a held pointer ends, it is not forgotten.
    aux.present.store(false);
    std::scoped_lock lock{rt->touch_mutex};
    rt->live_touches.clear();
    aux.SetTouch(std::span<const VideoCore::DSMod::AuxTouchPoint>{});
}

void pn_rt_lifecycle(PnRt* rt, int suspended) {
    if (!suspended) {
        PnShim::NoteTick(); // the game runs again before the first tick does: the pad gate holds across the pause
    }
    if (rt == nullptr || rt->epoch_address == 0) {
        return;
    }
    // Pause and resume both: one step each, never 0 (0 reads as "no epoch"), as Eden's InvalidateDualScreenGuestMailbox.
    // The helper's code may read the word at any moment: one atomic store of the whole word.
    u32 next = rt->epoch.load(std::memory_order_acquire);
    u32 want;
    do {
        want = next + 1 == 0 ? 1 : next + 1;
    } while (!rt->epoch.compare_exchange_weak(next, want, std::memory_order_acq_rel));
    __atomic_store_n(reinterpret_cast<u32*>(rt->epoch_address), want, __ATOMIC_RELEASE);
}

int pn_rt_frame(PnRt* rt, uint32_t* w, uint32_t* h, uint32_t* out_rgba, size_t out_capacity) {
    if (rt == nullptr) {
        return 0;
    }
    u64 serial = rt->frame_serial;
    u32 fw{}, fh{};
    if (!rt->system.gpu.DSModAux().TakeUi(rt->frame_scratch, fw, fh, serial)) {
        return 0;
    }
    if (w) *w = fw;
    if (h) *h = fh;
    if (out_rgba == nullptr) {
        return 1; // size query only: the frame stays "new" for the next call
    }
    if (out_capacity < rt->frame_scratch.size()) {
        return 0;
    }
    std::copy(rt->frame_scratch.begin(), rt->frame_scratch.end(), out_rgba);
    rt->frame_serial = serial;
    ++rt->frames_taken;
    return 1;
}

void pn_rt_touch(PnRt* rt, uint32_t finger_id, uint32_t x, uint32_t y, int down) {
    if (rt == nullptr) {
        return;
    }
    std::scoped_lock lock{rt->touch_mutex};
    auto& touches = rt->live_touches;
    const auto it = std::find_if(touches.begin(), touches.end(), [&](const auto& p) {
        return p.finger_id == finger_id;
    });
    if (!down) {
        // Matches Eden's own hosts (yuzu_cmd SDL3 aux window, Android native_input.cpp): a
        // released finger is simply left out of the next SetTouch call, never flagged with
        // attribute bit1 ("end") -- that bit is for a guest-side touch pass-through, unused here.
        if (it != touches.end()) {
            touches.erase(it);
        }
    } else if (it == touches.end()) {
        // Newly down: attribute bit0 marks the start, exactly once (AuxRouting::GetTouch's latch
        // rule clears it for the runtime after the first poll that sees it).
        touches.push_back(VideoCore::DSMod::AuxTouchPoint{finger_id, x, y, 1u, 0});
    } else {
        it->x = x;
        it->y = y;
        it->attributes = 0u; // already down: a move, not a new start
    }
    if (touches.size() > VideoCore::DSMod::AuxRouting::MaxTouch) {
        touches.resize(VideoCore::DSMod::AuxRouting::MaxTouch);
    }
    rt->system.gpu.DSModAux().SetTouch(touches);
}

int pn_rt_set_page(PnRt* rt, const char* page_id) {
    if (rt == nullptr || page_id == nullptr || !rt->runtime) {
        return 0;
    }
    const bool found = Core::Mods::PnRtAccess::SetPage(*rt->runtime, page_id);
    RefreshStatus(*rt);
    return found ? 1 : 0;
}

size_t pn_rt_status(PnRt* rt, char* buf, size_t n) {
    if (buf == nullptr || n == 0) {
        return 0;
    }
    if (rt == nullptr) {
        return static_cast<size_t>(std::snprintf(buf, n, "no runtime"));
    }
    // The line the tick thread left (RefreshStatus): nothing of the runtime is read here
    std::scoped_lock lock{rt->status_mutex};
    const int len = std::snprintf(buf, n, "%s", rt->status.c_str());
    return len < 0 ? 0 : std::min(static_cast<size_t>(len), n - 1);
}

} // extern "C"
