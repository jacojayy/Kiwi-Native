// PNRT: the smaller host services (pnrt_services.h): the console's shared fonts for "sysfont:", the add-on content RomFS for "aoc:", the battery for "@device.*"
// and the frame counters for "@perf.*". Each answers in the form the lifted code already expects from the emulator, so no lifted file changes its logic.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "common/logging.h"
#include "core/file_sys/vfs/vfs.h"
#include "peanuts_rt.h"
#include "pnrt_services.h"

// The host's side (dm/peanuts/peanuts_services.c), weak: absent from a build without the host
extern "C" int pn_host_power(int* percent, int* charging) __attribute__((weak));
extern "C" void pn_host_perf_totals(uint64_t* game_frames, uint64_t* system_frames, int64_t* frametime_ns) __attribute__((weak));
extern "C" int pn_host_aoc_open(const char* aoc_dir, int index, PnRtRomfs* out, char* why, size_t why_size) __attribute__((weak));

namespace PnShim {
namespace {

// ---- "sysfont:" -----------------------------------------------------------------------------------------------------------------------------------------
// The lifted source (mods/mod_sysfont_source.cpp) opens "<firmware file name>.bfttf" in the directory it is given and decrypts the firmware's stored form: an 8-byte
// header (the magic and the font's length) and the TrueType bytes, every 32-bit word XORed with one key. The host has the same fonts already decrypted (the importer
// writes them as .ttf from the player's firmware), so each file is served back in the stored form, made as it is read: nothing is kept in memory, and the lifted
// code's own decryption gives the TrueType bytes it would get from the firmware (padded with zeros to a whole word).
constexpr std::array<u8, 4> StoredKey{0x49, 0x62, 0x18, 0x06};    // the XOR key as the stored bytes meet it
constexpr std::array<u8, 4> StoredMagic{0x36, 0xF8, 0x1A, 0x1E};  // the first stored word (0x18029A7F under the key)
constexpr u64 MaxFontBytes = u64{64} << 20;

bool LooksLikeFont(const std::filesystem::path& path) {
    std::ifstream f{path, std::ios::binary};
    std::array<char, 4> tag{};
    if (!f.read(tag.data(), 4)) {
        return false;
    }
    static constexpr std::array<std::array<char, 4>, 4> known{{{0, 1, 0, 0}, {'O', 'T', 'T', 'O'}, {'t', 'r', 'u', 'e'}, {'t', 't', 'c', 'f'}}};
    return std::find(known.begin(), known.end(), tag) != known.end();
}

class StoredFontFile final : public FileSys::VfsFile {
public:
    StoredFontFile(std::string name_, std::filesystem::path path_, u64 ttf_size_) : name{std::move(name_)}, path{std::move(path_)}, ttf_size{ttf_size_} {}
    std::string GetName() const override {
        return name;
    }
    std::size_t GetSize() const override {
        return 8 + static_cast<std::size_t>((ttf_size + 3) & ~u64{3});
    }
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override {
        const std::size_t size = GetSize();
        if (offset >= size) {
            return 0;
        }
        length = std::min(length, size - offset);
        std::memset(data, 0, length);
        // The header: the magic, then the length under the key
        std::array<u8, 8> header{};
        std::memcpy(header.data(), StoredMagic.data(), 4);
        const u32 len = static_cast<u32>(ttf_size);
        for (int i = 0; i < 4; ++i) {
            header[4 + i] = static_cast<u8>(len >> (8 * i)) ^ StoredKey[i];
        }
        std::size_t done = 0;
        for (; offset + done < 8 && done < length; ++done) {
            data[done] = header[offset + done];
        }
        if (done < length) {
            const u64 at = offset + done - 8; // in the TrueType bytes
            std::ifstream f{path, std::ios::binary};
            if (!f || !f.seekg(static_cast<std::streamoff>(at))) {
                return 0;
            }
            f.read(reinterpret_cast<char*>(data + done), static_cast<std::streamsize>(length - done)); // past the file's end: the zeros of the padding
            if (at + static_cast<u64>(f.gcount()) < std::min<u64>(ttf_size, at + (length - done))) {
                return 0; // the file shrank or cannot be read: no half a font
            }
            for (std::size_t i = done; i < length; ++i) {
                data[i] ^= StoredKey[(offset + i) & 3];
            }
        }
        return length;
    }
    FileSys::VirtualDir GetContainingDirectory() const override {
        return nullptr;
    }

private:
    std::string name;
    std::filesystem::path path;
    u64 ttf_size;
};

class SysfontDir final : public FileSys::VfsDirectory {
public:
    explicit SysfontDir(std::map<std::string, std::filesystem::path, std::less<>> files_) : files{std::move(files_)} {}
    std::string GetName() const override {
        return "sysfont";
    }
    std::vector<FileSys::VirtualFile> GetFiles() const override {
        std::vector<FileSys::VirtualFile> out;
        for (const auto& [name, path] : files) {
            if (auto file = GetFile(name)) {
                out.push_back(std::move(file));
            }
        }
        return out;
    }
    std::vector<FileSys::VirtualDir> GetSubdirectories() const override {
        return {};
    }
    FileSys::VirtualFile GetFile(std::string_view name) const override {
        const auto it = files.find(name);
        if (it == files.end()) {
            return nullptr;
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(it->second, ec);
        // A font of a believable size (the firmware's largest is under 8 MiB; the lifted source reads the whole file into memory)
        if (ec || size < 12 || size > MaxFontBytes || !LooksLikeFont(it->second)) {
            return nullptr;
        }
        return std::make_shared<StoredFontFile>(it->first, it->second, size);
    }
    FileSys::VirtualDir GetSubdirectory(std::string_view) const override {
        return nullptr;
    }

private:
    std::map<std::string, std::filesystem::path, std::less<>> files; // the firmware's file name -> the host's TrueType file
};

// ---- "aoc:" ---------------------------------------------------------------------------------------------------------------------------------------------
// DM_AOC_DIR holds one entry per add-on content index (dm/host/hle_fs.c aoc_dir): "<index>" a folder with that add-on's RomFS, "<index>.nup" or "<index>.nsp" a
// container or dump (or a link to one) the host reads in place, "<index>.img" a RomFS image.
struct AocEntry {
    int index{};
    bool folder{};
    std::string name;
};

std::vector<AocEntry> ListAoc(const std::filesystem::path& dir) {
    std::vector<AocEntry> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{dir, ec}, end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        char* rest = nullptr;
        const long index = std::strtol(name.c_str(), &rest, 10);
        if (name.empty() || name[0] < '1' || name[0] > '9' || index > 2000) {
            continue;
        }
        const std::string_view ext{rest};
        if (ext.empty()) {
            std::error_code dir_ec;
            if (std::filesystem::is_directory(it->path(), dir_ec)) {
                out.push_back({static_cast<int>(index), true, name});
            }
        } else if (ext == ".nup" || ext == ".nsp" || ext == ".img") {
            out.push_back({static_cast<int>(index), false, name});
        }
    }
    // The lowest index first; of one index's forms the one the host's own mount takes: .nup, .nsp, .img, then the folder (hle_fs.c aoc_dir)
    const auto rank = [](const AocEntry& e) { return e.folder ? 3 : e.name.ends_with(".nup") ? 0 : e.name.ends_with(".nsp") ? 1 : 2; };
    std::sort(out.begin(), out.end(), [&](const AocEntry& a, const AocEntry& b) { return a.index != b.index ? a.index < b.index : rank(a) < rank(b); });
    return out;
}

} // namespace

namespace {
/// Whether `file` lies in the folder of the host's stand-in fonts (DM_FONT_DIR: an open font and a made-up icon font under the names standard.ttf and
/// nintendo_ext.ttf). Such a file is never served under a firmware font's name, whoever pointed DM_SHARED_FONT at it.
bool IsStandIn(const std::filesystem::path& file) {
    const char* const stand_ins = std::getenv("DM_FONT_DIR");
    if (stand_ins == nullptr || stand_ins[0] == 0 || file.empty()) {
        return false;
    }
    std::error_code a, b;
    const auto dir = std::filesystem::weakly_canonical(file.parent_path(), a), other = std::filesystem::weakly_canonical(stand_ins, b);
    return !a && !b && dir == other;
}
} // namespace

FileSys::VirtualDir SysfontRootOf(const std::string& standard_ttf, const std::string& extension_ttf) {
    if (standard_ttf.empty()) {
        return nullptr;
    }
    const std::filesystem::path standard{standard_ttf};
    const std::filesystem::path beside = standard.parent_path();
    std::map<std::string, std::filesystem::path, std::less<>> files;
    if (IsStandIn(standard)) {
        LOG_INFO(Core, "peanuts_rt: sysfont: DM_SHARED_FONT is in the stand-in fonts' folder (DM_FONT_DIR): not served as firmware fonts");
    } else {
        files = {
            {"nintendo_udsg-r_std_003.bfttf", standard},
            {"nintendo_udsg-r_org_zh-cn_003.bfttf", beside / "cjk_sc.ttf"},
            {"nintendo_udsg-r_ext_zh-cn_003.bfttf", beside / "cjk_sc_ext.ttf"},
            {"nintendo_udjxh-db_zh-tw_003.bfttf", beside / "cjk_tc.ttf"},
            {"nintendo_udsg-r_ko_003.bfttf", beside / "cjk_kr.ttf"},
            // (nintendo_ext2_003 is not among the fonts the host has: it reads as missing, as on a console without it)
        };
    }
    if (!extension_ttf.empty() && !IsStandIn(extension_ttf)) {
        files.emplace("nintendo_ext_003.bfttf", std::filesystem::path{extension_ttf});
    }
    return std::make_shared<SysfontDir>(std::move(files));
}

FileSys::VirtualDir SysfontRoot() {
    const char* const standard = std::getenv("DM_SHARED_FONT");
    const char* const extension = std::getenv("DM_SHARED_FONT_EXT");
    if (standard == nullptr || standard[0] == 0) {
        LOG_INFO(Core, "peanuts_rt: sysfont: no DM_SHARED_FONT (no firmware fonts in the app): \"sysfont:\" reads as missing");
        return nullptr;
    }
    return SysfontRootOf(standard, extension != nullptr ? extension : "");
}

FileSys::VirtualDir AocRoot(u64 program_id) {
    const char* const base = std::getenv("DM_AOC_DIR");
    if (base == nullptr || base[0] == 0) {
        LOG_INFO(Core, "DSMod: aoc romfs of {:016X} unavailable (no DM_AOC_DIR)", program_id);
        return nullptr;
    }
    const auto entries = ListAoc(base);
    if (entries.empty()) {
        LOG_INFO(Core, "DSMod: aoc romfs of {:016X} unavailable (no add-on content)", program_id);
        return nullptr;
    }
    // The lowest index, in the form the host's own mount takes (ListAoc's order), and nothing else: when it cannot be read here the answer is "none", never a
    // higher add-on in its place (a companion would read another add-on's files as this one's)
    const auto& entry = entries.front();
    FileSys::VirtualDir root;
    std::string why;
    if (entry.folder) {
        root = FileSys::OpenHostDirectory(std::filesystem::path{base} / entry.name);
        why = "the folder does not open";
    } else if (entry.name.ends_with(".img")) {
        why = "a RomFS image is not read for a companion";
    } else if (&pn_host_aoc_open != nullptr) {
        PnRtRomfs provider{};
        char reason[256] = "";
        if (pn_host_aoc_open(base, entry.index, &provider, reason, sizeof reason) != 0) {
            root = FileSys::OpenProviderDirectory(provider);
        }
        why = reason[0] ? reason : "it does not open";
    } else {
        why = "this build cannot read a container";
    }
    if (!root) {
        LOG_WARNING(Core, "DSMod: aoc romfs of {:016X} unavailable: add-on content {} ({}), the lowest of {} in DM_AOC_DIR: {}", program_id, entry.index,
                    entry.name, entries.size(), why);
        return nullptr;
    }
    LOG_INFO(Core, "DSMod: aoc romfs of {:016X}: add-on content {} ({}), the lowest of {} in DM_AOC_DIR, opened", program_id, entry.index, entry.name,
             entries.size());
    return root;
}

Common::PowerStatus PowerStatus() {
    int percent = -1, charging = 0;
    if (&pn_host_power != nullptr && pn_host_power(&percent, &charging) != 0 && percent >= 0) {
        return Common::PowerStatus{std::min(percent, 100), charging != 0, true};
    }
    return Common::PowerStatus{-1, false, false}; // "no battery": the @device.* keys read as before
}

PerfTotals ReadPerfTotals() {
    PerfTotals totals;
    if (&pn_host_perf_totals != nullptr) {
        uint64_t game = 0, system = 0;
        int64_t frametime = 0;
        pn_host_perf_totals(&game, &system, &frametime);
        totals = {game, system, frametime};
    }
    return totals;
}

} // namespace PnShim
