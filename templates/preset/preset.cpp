/**
 * @file preset.cpp
 * @brief MPC Force Preset Library (C++): 128-slot patch storage.
 *
 * 128-slot patch storage with seek-based file access.
 * Works with DPF, JUCE, LV2 — all C++ frameworks.
 * No extern "C". Pure namespace MpcPreset.
 */

#include "preset.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>

namespace MpcPreset {

/* ---- Constants ---------------------------------------------------------- */

static constexpr uint32_t kMagic   = 0x50525354; /* "PRST" */
static constexpr uint32_t kVersion = 1;

/* ---- Helpers ----------------------------------------------------------- */

/// @brief Lowercases an ASCII string (for case-insensitive uid compare).
static std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// @brief True if the path opens as a readable file.
static bool fileExists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

/// @brief Returns the directory part of a path (before the last '/').
static std::string dirName(const std::string& path) {
    size_t p = path.rfind('/');
    return p == std::string::npos ? "" : path.substr(0, p);
}

/// @brief Returns the filename part of a path (after the last '/').
static std::string baseName(const std::string& path) {
    size_t p = path.rfind('/');
    return p == std::string::npos ? path : path.substr(p + 1);
}

/* ---- Patch file path ---------------------------------------------------- */

/// @brief Builds the bank filename "<uid-hex>-bank.bin".
static std::string makePatchName(uint32_t uid) {
    char name[32];
    snprintf(name, sizeof(name), "%08x-bank.bin", uid);
    return std::string(name);
}

/// @brief Joins the install dir and the bank filename.
static std::string patchPath(const char *dir, uint32_t uid) {
    std::string name = makePatchName(uid);
    if (dir && dir[0]) {
        return std::string(dir) + "/" + name;
    }
    return name;
}

/* ---- Header read/write --------------------------------------------------- */

struct PatchHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t uid;
    uint32_t numSlots;
    uint32_t numParams;
};

/// @brief Reads and validates the patch header (magic + version).
static bool readHeader(const std::string& path, PatchHeader& hdr) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(&hdr), sizeof(PatchHeader));
    if (!f.good()) return false;
    f.close();
    return hdr.magic == kMagic && hdr.version == kVersion;
}

/* ---- Public API ---------------------------------------------------------- */

/// @brief Builds the full path to the patch file (see preset.h).
char *getPatchPath(char *buf, size_t bufSize, const char *dir, uint32_t uid) {
    std::string path = patchPath(dir, uid);

    if (buf && bufSize > 0) {
        snprintf(buf, bufSize, "%s", path.c_str());
        return buf;
    }

    /* Allocate if buf is nullptr */
    char* result = new char[path.size() + 1];
    memcpy(result, path.c_str(), path.size() + 1);
    return result; /* caller must free */
}

/// @brief Creates the patch file if missing and verifies compatibility.
int init(const char *dir, const float *defaults, int num_params, uint32_t uid) {
    if (!dir || !dir[0] || num_params <= 0) return -1;

    std::string path = patchPath(dir, uid);

    /* If file exists and is compatible, done */
    if (fileExists(path)) {
        PatchHeader hdr;
        if (readHeader(path, hdr) &&
            hdr.uid == uid &&
            static_cast<int>(hdr.numParams) == num_params) {
            return 0; /* compatible, nothing to do */
        }
    }

    /* Create new file with defaults */
    PatchHeader hdr;
    hdr.magic     = kMagic;
    hdr.version   = kVersion;
    hdr.uid       = uid;
    hdr.numSlots  = kNumSlots;
    hdr.numParams = static_cast<uint32_t>(num_params);

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return -1;

    f.write(reinterpret_cast<const char*>(&hdr), sizeof(PatchHeader));

    /* Fill all 128 slots with default values */
    for (int slot = 0; slot < kNumSlots; slot++) {
        f.write(reinterpret_cast<const char*>(defaults), num_params * sizeof(float));
    }

    return f.good() ? 0 : -1;
}

/// @brief Loads one slot into the caller's buffer.
int load(const char *dir, int slot, float *params, int num_params, uint32_t uid) {
    if (!dir || !dir[0]) return -1;
    if (slot < 0 || slot >= kNumSlots) return -1;
    if (!params || num_params <= 0) return -1;

    std::string path = patchPath(dir, uid);

    PatchHeader hdr;
    if (!readHeader(path, hdr)) return -1;
    if (hdr.uid != uid) return -1;
    if (static_cast<int>(hdr.numParams) != num_params) return -1;

    /* Seek to slot data and read */
    std::ifstream f(path, std::ios::binary);
    if (!f) return -1;

    f.seekg(kHeaderSize + slot * num_params * static_cast<long>(sizeof(float)));
    f.read(reinterpret_cast<char*>(params), num_params * sizeof(float));

    return f.good() ? 0 : -1;
}

/// @brief Saves the caller's buffer into one slot.
int save(const char *dir, int slot, const float *params, int num_params, uint32_t uid) {
    if (!dir || !dir[0]) return -1;
    if (slot < 0 || slot >= kNumSlots) return -1;
    if (!params || num_params <= 0) return -1;

    std::string path = patchPath(dir, uid);

    PatchHeader hdr;
    if (!readHeader(path, hdr)) return -1;
    if (hdr.uid != uid) return -1;
    if (static_cast<int>(hdr.numParams) != num_params) return -1;

    /* Seek to slot data and write */
    std::fstream f(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) return -1;

    f.seekp(kHeaderSize + slot * num_params * static_cast<long>(sizeof(float)));
    f.write(reinterpret_cast<const char*>(params), num_params * sizeof(float));

    return f.good() ? 0 : -1;
}

/// @brief Checks that an existing patch file matches this plugin's uid/count.
int checkCompatibility(const char *dir, uint32_t expected_uid, int expected_params) {
    if (!dir || !dir[0]) return -1;

    std::string path = patchPath(dir, expected_uid);
    if (!fileExists(path)) return -1;

    PatchHeader hdr;
    if (!readHeader(path, hdr)) return -1;
    if (hdr.uid != expected_uid) return -1;
    if (static_cast<int>(hdr.numParams) != expected_params) return -1;

    return 0;
}

/// @brief Resolves the install directory from MPC.settings.
char *findInstallDir(const char *uidHex, const char *soBasename, const char *settingsPath) {
    if (!uidHex || !soBasename || !settingsPath) return nullptr;

    std::ifstream f(settingsPath);
    if (!f) return nullptr;

    /* Read entire file */
    std::stringstream ss;
    ss << f.rdbuf();
    std::string xml = ss.str();
    f.close();

    std::string wantUid = toLower(std::string(uidHex));
    std::string soBase  = std::string(soBasename);

    /* Find <PLUGIN ...> entries */
    size_t pos = 0;
    while ((pos = xml.find("<PLUGIN", pos)) != std::string::npos) {
        size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;

        std::string span = xml.substr(pos, end - pos + 1);
        pos = end + 1;

        /* Extract uid */
        std::string uidStr;
        size_t uidPos = span.find("uid=\"");
        if (uidPos != std::string::npos) {
            size_t vStart = uidPos + 5;
            size_t vEnd   = span.find('"', vStart);
            if (vEnd != std::string::npos) {
                uidStr = span.substr(vStart, vEnd - vStart);
            }
        }
        if (toLower(uidStr) != wantUid) continue;

        /* Extract file */
        std::string file;
        size_t filePos = span.find("file=\"");
        if (filePos != std::string::npos) {
            size_t vStart = filePos + 6;
            size_t vEnd   = span.find('"', vStart);
            if (vEnd != std::string::npos) {
                file = span.substr(vStart, vEnd - vStart);
            }
        }
        if (baseName(file) != soBase) continue;

        /* Found it — return dirname */
        std::string dir = dirName(file);
        if (dir.empty()) continue;

        char* result = new char[dir.size() + 1];
        memcpy(result, dir.c_str(), dir.size() + 1);
        return result; /* caller must free */
    }

    return nullptr;
}

} /* namespace MpcPreset */
