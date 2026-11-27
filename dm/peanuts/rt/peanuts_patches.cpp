// PNRT: a package manifest's "patches" applied to the running game (peanuts_patches.h).
//
// The emulator writes each patch once when main is mapped and logs it; an optional patch only with EDEN_DSMOD_PATCHES=1, a switch of developer builds. Here:
//   - an optional patch is skipped the same way (this build has no developer switches, as the emulator's release build);
//   - a patch must be whole instructions wholly below the end of main's code (text_end, as the load plan's entries are judged);
//   - it is refused where the host's rewriter changed or moved an instruction (main.orig.bin and its list say where; without them the host cannot tell and every
//     patch is refused): the game runs the rewritten code there, and the package's instructions would be written over something else;
//   - it is refused where main no longer holds what it held when the package was loaded, before its load plan was installed: the plan's patches and the helper's
//     sites are the package's own, and a manifest patch over them would cut one in two;
//   - it is written with the game's threads stopped and no thread standing inside it past its first instruction. Only a single instruction whose old and new words
//     are both of the set the architecture lets be modified while executed (B, BL, NOP, BRK, SVC, HVC, SMC, ISB) is written without a stop, as one atomic store;
//   - when the threads cannot be stopped just now, or one stands inside, nothing is written and the patch is tried again on later ticks (RetryBudget times);
//   - the pages get back the protection they had;
//   - what was written is remembered for the life of the process: the same patch again is "already there", a different one over those words is refused.
//     A package replaced while the game runs leaves its patches in main, as on the emulator (nothing ever undoes one); the new package's own patches are judged
//     against main as it then is, and against that memory.
// Inside the stop nothing allocates, logs or takes a lock: a stopped game thread may hold the allocator's.
// No published package declares patches; the code is small on purpose.
#include "peanuts_patches.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <sys/mman.h>
#include <unistd.h>

#include "common/dsmod_dev_tools.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/mods/mod_types.h"

namespace PeanutsPatches {
namespace {
struct Host {
    bool set{};
    u64 base{}, size{}, text_end{};
    void* user{};
    int (*code_rewritten)(void*, uint64_t, size_t){};
    void* guest_user{};
    int (*stopped_in)(void*, uint64_t, uint64_t){};
    std::vector<std::vector<u32>> held; ///< per patch of the manifest: the words at its site when the package was loaded (empty: the site is not in main's code)
    std::vector<u32> attempts;          ///< per patch: tries that ended in "not now"
};
Host g_host;
std::mutex g_written_mutex;
std::map<u64, u32> g_written; ///< address -> the word this process wrote there

bool InText(const Target& target, s64 at, u64 n) {
    return at >= 0 && at % 4 == 0 && n != 0 && static_cast<u64>(at) < target.text_end && n <= target.text_end - static_cast<u64>(at);
}
/// The protection of [addr, addr + len) when all of it is mapped with one; -1 otherwise.
int ProtectionOf(u64 addr, u64 len) {
    std::FILE* const f = std::fopen("/proc/self/maps", "re");
    if (f == nullptr) {
        return -1;
    }
    char line[512];
    int prot = -1;
    u64 covered = addr;
    while (covered < addr + len && std::fgets(line, sizeof line, f) != nullptr) {
        u64 lo = 0, hi = 0;
        char perms[8] = "";
        if (std::sscanf(line, "%" SCNx64 "-%" SCNx64 " %7s", &lo, &hi, perms) != 3 || hi <= covered || lo > covered) {
            continue; // (the file is in address order: a mapping that starts past `covered` means a hole, found below)
        }
        const int p = (perms[0] == 'r' ? PROT_READ : 0) | (perms[1] == 'w' ? PROT_WRITE : 0) | (perms[2] == 'x' ? PROT_EXEC : 0);
        if (prot != -1 && p != prot) {
            prot = -1;
            break;
        }
        prot = p;
        covered = hi;
    }
    std::fclose(f);
    return covered >= addr + len ? prot : -1;
}
} // namespace

bool SafeWhileRunning(u32 old_word, u32 new_word) {
    const auto safe = [](u32 w) {
        return (w & 0x7C000000) == 0x14000000 ||    // B, BL
               w == 0xD503201F ||                   // NOP
               (w & 0xFFE0001F) == 0xD4200000 ||    // BRK
               ((w & 0xFFE0001C) == 0xD4000000 && (w & 3) != 0) || // SVC, HVC, SMC
               (w & 0xFFFFF0FF) == 0xD50330DF;      // ISB
    };
    return safe(old_word) && safe(new_word);
}

void ForgetWritten() {
    std::scoped_lock lock{g_written_mutex};
    g_written.clear();
}

void Configure(const PnRtConfig& config, const std::vector<Core::Mods::GuestPatch>& patches) {
    g_host = {};
    g_host.set = true;
    g_host.base = config.main_base;
    g_host.size = config.main_size;
    g_host.text_end = config.helper.text_end <= config.main_size ? config.helper.text_end : 0;
    g_host.user = config.helper.user;
    g_host.code_rewritten = config.helper.code_rewritten;
    g_host.guest_user = config.guest.user;
    g_host.stopped_in = config.guest.stopped_in;
    g_host.held.resize(patches.size());
    g_host.attempts.assign(patches.size(), 0);
    Target bounds;
    bounds.text_end = g_host.text_end;
    for (size_t i = 0; i < patches.size(); ++i) {
        const u64 n = patches[i].words.size() * sizeof(u32);
        if (g_host.base == 0 || !InText(bounds, patches[i].at, n)) {
            continue;
        }
        const int prot = ProtectionOf(g_host.base + static_cast<u64>(patches[i].at), n);
        if (prot < 0 || (prot & PROT_READ) == 0) {
            continue;
        }
        g_host.held[i].resize(patches[i].words.size());
        std::memcpy(g_host.held[i].data(), reinterpret_cast<const void*>(g_host.base + static_cast<u64>(patches[i].at)), n);
    }
}

Outcome Write(const Target& target, s64 at, std::span<const u32> words, std::span<const u32> expected) {
    const u64 n = words.size() * sizeof(u32);
    if (words.empty()) {
        return {Verdict::Refused, "it has no instructions"};
    }
    if (!InText(target, at, n)) {
        return {Verdict::Refused, "it is not whole instructions inside main's code"};
    }
    const u64 address = target.base + static_cast<u64>(at);
    if (!target.rewritten || target.rewritten(address, n)) {
        return {Verdict::Refused, "this host's rewriter changed or moved the code there (or its list is missing: build the game again)"};
    }
    const u64 ps = static_cast<u64>(sysconf(_SC_PAGESIZE));
    const u64 page = address & ~(ps - 1), len = ((address + n + ps - 1) & ~(ps - 1)) - page;
    const int prot = ProtectionOf(page, len);
    if (prot < 0 || (prot & PROT_READ) == 0) {
        return {Verdict::Refused, "the code's pages are not mapped readable with one protection"};
    }
    auto* const site = reinterpret_cast<u32*>(address);
    std::scoped_lock lock{g_written_mutex};
    // What this process wrote there before: the same words are done, anything else would be a patch over a patch
    bool same = true, touched = false;
    for (size_t i = 0; i < words.size(); ++i) {
        const auto it = g_written.find(address + i * sizeof(u32));
        touched = touched || it != g_written.end();
        same = same && it != g_written.end() && it->second == words[i] && __atomic_load_n(site + i, __ATOMIC_ACQUIRE) == words[i];
    }
    if (touched) {
        return same ? Outcome{Verdict::Written, "already there"} : Outcome{Verdict::Refused, "an earlier patch of this session wrote other instructions there"};
    }
    bool holds_new = true, need_stop = words.size() != 1;
    for (size_t i = 0; i < words.size(); ++i) {
        const u32 now = __atomic_load_n(site + i, __ATOMIC_ACQUIRE);
        holds_new = holds_new && now == words[i];
        need_stop = need_stop || !SafeWhileRunning(now, words[i]);
        if (!expected.empty() && (expected.size() != words.size() || now != expected[i]) && now != words[i]) {
            return {Verdict::Refused, "main does not hold there what it held when the package was loaded: the package's load plan or helper wrote it"};
        }
    }
    if (!holds_new) {
        // 0 written, 1 a thread stands inside, else the errno of the page protection
        int result = 0;
        const auto store = [&] {
            if (words.size() > 1 && target.stopped_in && target.stopped_in(address + 4, address + n) > 0) {
                result = 1;
                return;
            }
            if (mprotect(reinterpret_cast<void*>(page), len, prot | PROT_WRITE) != 0) {
                result = errno > 1 ? errno : EACCES;
                return;
            }
            for (size_t i = 0; i < words.size(); ++i) {
                __atomic_store_n(site + i, words[i], __ATOMIC_RELEASE);
            }
            mprotect(reinterpret_cast<void*>(page), len, prot); // as it was
            __builtin___clear_cache(reinterpret_cast<char*>(address), reinterpret_cast<char*>(address + n));
        };
        if (!need_stop) {
            store(); // one instruction, old and new both of the set that may change under a running thread: it runs one or the other
        } else {
            const std::function<void()> fn{store}; // made before the stop
            if (!target.stopped || !target.stopped(fn)) {
                return {Verdict::Retry, "the game's threads could not be stopped"};
            }
        }
        if (result == 1) {
            return {Verdict::Retry, "a game thread stood inside the patched instructions"};
        }
        if (result != 0) {
            return {Verdict::Refused, std::string{"the code's pages cannot be made writable ("} + std::strerror(result) + ")"};
        }
    }
    for (size_t i = 0; i < words.size(); ++i) {
        g_written[address + i * sizeof(u32)] = words[i];
    }
    return {Verdict::Written, holds_new ? "already there" : ""};
}

void Apply(Core::System& system, const std::vector<Core::Mods::GuestPatch>& patches, std::vector<bool>& done, bool& applied) {
    if (applied || patches.empty()) {
        return;
    }
    const char* const opt_in = Common::DSMod::DevEnvironment("EDEN_DSMOD_PATCHES");
    const bool apply_optional = opt_in != nullptr && opt_in[0] == '1';
    const bool configured = g_host.set && g_host.code_rewritten != nullptr && g_host.base != 0 && g_host.text_end != 0 && g_host.held.size() == patches.size();
    Target target;
    if (configured) {
        target.base = g_host.base;
        target.text_end = g_host.text_end;
        target.rewritten = [](u64 addr, size_t len) { return g_host.code_rewritten(g_host.user, addr, len) != 0; };
        target.stopped = [&system](const std::function<void()>& fn) { return system.RunWithGuestThreadsSuspended(fn); };
        if (g_host.stopped_in != nullptr) {
            target.stopped_in = [](u64 lo, u64 hi) { return g_host.stopped_in(g_host.guest_user, lo, hi); };
        }
    }
    done.resize(patches.size(), false);
    bool waiting = false;
    for (size_t index = 0; index < patches.size(); ++index) {
        if (done[index]) {
            continue;
        }
        const auto& patch = patches[index];
        if (patch.optional && !apply_optional) {
            LOG_INFO(Core, "DSMod: skipping optional patch at main+{:X} ({})", patch.at, patch.why);
            done[index] = true;
            continue;
        }
        Outcome outcome{Verdict::Refused, "this host gave no way to check main's code"};
        if (configured) {
            outcome = g_host.held[index].empty() ? Outcome{Verdict::Refused, "it is not whole instructions inside main's code"}
                                                 : Write(target, patch.at, patch.words, g_host.held[index]);
        }
        if (outcome.verdict == Verdict::Retry && ++g_host.attempts[index] < RetryBudget) {
            if (g_host.attempts[index] == 1) {
                LOG_INFO(Core, "DSMod: patch at main+{:X} ({}) not written yet: {}; tried again on later ticks", patch.at, patch.why, outcome.why);
            }
            waiting = true;
            continue;
        }
        done[index] = true; // written, or refused for good
        if (outcome.verdict == Verdict::Written) {
            LOG_INFO(Core, "DSMod: patched main+{:X} with {} instruction(s){}: {}", patch.at, patch.words.size(),
                     outcome.why.empty() ? "" : " (already there)", patch.why);
        } else {
            LOG_WARNING(Core, "DSMod: patch at main+{:X} ({}) not applied: {}{}", patch.at, patch.why, outcome.why,
                        outcome.verdict == Verdict::Retry ? " (given up after many ticks)" : "");
        }
    }
    applied = !waiting;
}

} // namespace PeanutsPatches
