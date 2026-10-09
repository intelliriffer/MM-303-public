/**
 * @file preset.h
 * @brief MPC Force Preset Library (C++): 128-slot patch storage.
 *
 * 128-slot patch storage with seek-based access.
 * Works with DPF, JUCE, LV2 — all C++ frameworks.
 * No extern "C". Pure namespace.
 *
 * File format: [20-byte header][128 × num_params floats]
 * Header: magic(4) + version(4) + uid(4) + num_slots(4) + num_params(4)
 * Slot N at offset: 20 + (N × num_params × 4)
 *
 * RAM cost per load/save: exactly num_params floats (one slot).
 */

#ifndef MPC_PRESET_H
#define MPC_PRESET_H

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* uint32_t */

namespace MpcPreset {

/* Number of slots per patch file. Always 128. */
constexpr int kNumSlots = 128;

/* Header size in bytes. */
constexpr int kHeaderSize = 20;

/* Patch-select CC number. Default is 0 (CC 0 — the Force's patch selector;
 * CC 100 is NOT used). Override per plugin with:
 * #define CC_PC 91  (before including preset.h)
 * The plugin's MIDI handler checks if incoming CC == CC_PC, treats value as PC. */
#ifndef CC_PC
#define CC_PC 0
#endif

/** @brief Creates the patch file if missing and verifies compatibility.
 *  @param dir        install directory (resolved from MPC.settings by the plugin)
 *  @param defaults   array of num_params float values (plugin default values)
 *  @param num_params number of state params this plugin has
 *  @param uid        this plugin's unique ID (e.g. 0x4d6d3033)
 *  @return 0 on success, -1 on error. */
int init(const char *dir, const float *defaults, int num_params, uint32_t uid);

/** @brief Loads one slot into the caller's buffer.
 *  @param dir        install directory
 *  @param slot       0–127
 *  @param params     caller's buffer, must hold num_params floats (overwritten)
 *  @param num_params number of state params this plugin has
 *  @param uid        this plugin's unique ID (for filename)
 *  @return 0 on success, -1 on error. */
int load(const char *dir, int slot, float *params, int num_params, uint32_t uid);

/** @brief Saves the caller's buffer to one slot.
 *  @param dir        install directory
 *  @param slot       0–127
 *  @param params     caller's buffer with num_params floats to write
 *  @param num_params number of state params this plugin has
 *  @param uid        this plugin's unique ID (for filename)
 *  @return 0 on success, -1 on error. */
int save(const char *dir, int slot, const float *params, int num_params, uint32_t uid);

/** @brief Checks whether an existing patch file is compatible with this plugin.
 *  @param dir             install directory
 *  @param expected_uid    plugin's UID
 *  @param expected_params plugin's current param count
 *  @return 0 if compatible, -1 if not (file missing or mismatched). */
int checkCompatibility(const char *dir, uint32_t expected_uid, int expected_params);

/** @brief Resolves the install directory from MPC.settings.
 *
 *  Parses the XML at settingsPath and finds the plugin entry matching
 *  uidHex + soBasename.
 *  @return heap-allocated string (caller must free), or nullptr on error. */
char *findInstallDir(const char *uidHex, const char *soBasename, const char *settingsPath);

/** @brief Builds the full path to the patch file.
 *  @param buf     output buffer (nullptr = allocate)
 *  @param bufSize size of buf (0 if allocating)
 *  @param dir     install directory (nullptr = current dir)
 *  @param uid     plugin's UID (filename: "[uid-hex]-bank.bin")
 *  @return buf, or a heap-allocated string if buf was nullptr. */
char *getPatchPath(char *buf, size_t bufSize, const char *dir, uint32_t uid);

} /* namespace MpcPreset */

#endif /* MPC_PRESET_H */
