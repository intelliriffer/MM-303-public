# MPC Force Preset Library — Integration Guide

## Overview

128-slot patch storage for VST2 plugins on Akai MPC Force.
Single bank file lives next to your plugin's .so (resolved via `/data/Settings/MPC/MPC.settings`).
Each slot stores your plugin's state params as raw floats.

**Files to copy into your project:**
- `preset.h` — header (namespace `MpcPreset`)
- `preset.cpp` — implementation

No dependencies beyond standard C++ (cstdio, cstring, fstream).
Works with DPF, JUCE, LV2 — any C++ framework.

---

## Critical: Force MIDI Behavior (Verified)

The Force host **never sends C0 Program Change** to VST2 plugins.
Patch selection arrives exclusively as CC messages:

| CC Number | Meaning | Value Range |
|-----------|---------|-------------|
| **0 (CC 0)** | **Patch/program number (our CC-PC)** | **0–127** |
| 32 (CC 32) | Bank select MSB | 0–127 |
| 100 (CC 100) | Sent by Force but NOT used by preset path | 0–127 |

**CC 0 is our default CC-PC.** The plugin treats incoming CC 0 as a program change.
Tell users: "Use MIDI CC 0 to select patches."

C0 handling is optional (for non-Force hosts) but will never fire on Force.

---

## DPF-Specific Requirements

In your plugin's `.cpp` (before including DistrhoPlugin headers):

```cpp
#define DISTRHO_PLUGIN_WANT_MIDI_INPUT 1          // enables MIDI in run()
#define DISTRHO_PLUGIN_WANT_PROGRAMS 1            // enables program name/count API
```

In `DistrhoPluginInfo.h`:
```cpp
#define DISTRHO_PLUGIN_WANT_MIDI_INPUT 1
#define DISTRHO_PLUGIN_WANT_PROGRAMS 1
```

Constructor: `Plugin(paramCount, 128, 0)` — second arg is program count.

Override these in your plugin class:
```cpp
void initProgramName(uint32_t index, String& name) override;  // return "0".."127"
void loadProgram(uint32_t index) override;                    // host calls this (optional path)
```

**Note:** On Force, `loadProgram()` is never called by the host (OPCODE_02 never fires).
The actual patch change path is CC 0 → your `run()` handler → `MpcPreset::load()`.
Implement `loadProgram()` anyway for non-Force host compatibility.

---

## Parameters (7 total for Kars example)

| # | Name | Range | Type | Saves in bank? | Notes |
|---|------|-------|------|----------------|-------|
| 0 | Sustain | 0–1 (bool) | State param | per slot | Your state params go here |
| 1 | Release | 0–5 s | State param | per slot | |
| 2 | Volume | 0–100 % | State param | per slot | |
| 3 | Prog# | 0–127 | Slot selector | No | Knob. Selects target slot. |
| 4 | Load | 0–1 (momentary) | Trigger | No | Spring-back. Loads slot = Prog#. ANY setParameter value (1 or 0) triggers — Force toggles 0->1->0 per tap, both values are a press. |
| 5 | Save | 0–1 (momentary) | Trigger | No | Spring-back. Saves to slot = Prog#. Any value triggers, same as Load. |
| 6 | StatusLabel | text | Display | No | Visual-only blink LED: reads 1 for ~500ms after any Load/Save, then falls back to 0. Host writes are ignored (display only). |

For your plugin: params 0..N-1 are state (saved per slot), the rest are control.
Set `kNumStateParams` to your state param count.

---

## Integration: Step by Step (DPF)

### 1. Copy library files
```
your-plugin/src/preset.h
your-plugin/src/preset.cpp
```

### 2. Add to CMakeLists.txt
Add `src/preset.cpp` to your DSP sources (it compiles into the plugin, not a separate lib).

### 3. Define CC_PC (optional)
Default is 100. Override if you want a different CC:
```cpp
#define CC_PC 91   // before #include "preset.h"
```

### 4. In your plugin .hpp
```cpp
#include "preset.h"

// Add to class:
static const int kNumStateParams = 3;  // your state param count
char fPresetDir[512];

// Override:
void initProgramName(uint32_t index, String& name) override;
void loadProgram(uint32_t index) override;

// Your preset methods:
void loadPreset(int slot);
void savePreset(int slot);
```

### 5. In your plugin .cpp — activate() (resolve dir + init bank)
```cpp
void MyPlugin::activate() {
    // Resolve install directory from MPC.settings
    char* dir = MpcPreset::findInstallDir("4b415253", "Kars.so",
                                          "/data/Settings/MPC/MPC.settings");
    if (dir) {
        std::snprintf(fPresetDir, sizeof(fPresetDir), "%s", dir);
        free(dir);
    }

    // Create bank file if missing (fills all 128 slots with defaults)
    MpcPreset::init(fPresetDir, kDefaultParams, kNumStateParams, getUniqueId());

    // Load slot 0 on startup
    loadPreset(0);
}
```

### 6. In your plugin .cpp — run() (handle CC 0)
```cpp
void MyPlugin::run(const float**, float** outputs, uint32_t frames,
                   const MidiEvent* midiEvents, uint32_t midiEventCount) {
    for (AudioMidiSyncHelper amsh(outputs, frames, midiEvents, midiEventCount);
         amsh.nextEvent();) {
        for (uint32_t i = 0; i < amsh.midiEventCount; ++i) {
            if (amsh.midiEvents[i].size > MidiEvent::kDataSize) continue;
            const uint8_t* data = amsh.midiEvents[i].data;

            switch (data[0] & 0xF0) {
            case 0xC0:  // Program Change (non-Force hosts only)
                loadPreset(data[1]);
                break;
            case 0xB0:  // CC — check for patch change
                if (data[1] == CC_PC) {
                    loadPreset(data[2]);
                }
                break;
            // ... your note-on/note-off handling
            }
        }
        // ... audio processing
    }
}
```

### 7. loadPreset / savePreset
```cpp
void MyPlugin::loadPreset(int slot) {
    if (slot < 0 || slot > 127) return;
    float params[kNumStateParams];
    if (fPresetDir[0] && MpcPreset::load(fPresetDir, slot, params, kNumStateParams, getUniqueId()) == 0) {
        // Apply loaded values to your internal state
        fMyParam1 = params[0];
        fMyParam2 = params[1];
        // Push to host UI
        setParameterValue(param0, fMyParam1);
        requestParameterValueChange(param0, fMyParam1);
    }
}

void MyPlugin::savePreset(int slot) {
    if (slot < 0 || slot > 127) return;
    float params[kNumStateParams];
    params[0] = fMyParam1;
    params[1] = fMyParam2;
    if (fPresetDir[0]) {
        MpcPreset::save(fPresetDir, slot, params, kNumStateParams, getUniqueId());
    }
}
```

### 8. initProgramName / loadProgram
```cpp
void MyPlugin::initProgramName(uint32_t index, String& name) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", (int)index);
    name = buf;
}

void MyPlugin::loadProgram(uint32_t index) {
    loadPreset((int)index);
}
```

---

## Bank File Format

```
Offset  Size   Field
------  -----  -------------------------
0       4      magic (0x50525354 "PRST")
4       4      version (1)
8       4      uid (your plugin's unique ID as uint32)
12      4      num_slots (always 128)
16      4      num_params (your state param count)
20      ...    float data: [slot 0][slot 1]...[slot 127]
```

File name: `{uid-hex}-bank.bin` (e.g., `4b415253-bank.bin`)
Total size: `20 + (128 × num_params × 4)` bytes

---

## API Reference (namespace MpcPreset)

| Function | Purpose |
|----------|---------|
| `init(dir, defaults, num_params, uid)` | Create bank file if missing. Fills all slots with defaults. |
| `load(dir, slot, params, num_params, uid)` | Read one slot into caller's buffer. Seek-based (O(1) RAM). |
| `save(dir, slot, params, num_params, uid)` | Write caller's buffer to one slot. Seek-based. |
| `checkCompatibility(dir, uid, num_params)` | Verify existing file matches this plugin. |
| `findInstallDir(uidHex, soBasename, settingsPath)` | Parse MPC.settings XML to find plugin's install dir. |
| `getPatchPath(buf, size, dir, uid)` | Get full path to bank file. |

All return 0 on success, -1/nullptr on error.
`findInstallDir` and `getPatchPath(nullptr,...)` return heap-allocated strings (caller frees).

---

## What does NOT exist

- No multi-bank support (single bank of 128)
- No patch name display in bank file
- No MIDI input port via RTMidi
- Bank select CCs (0/32) are received but ignored (single bank)

---

## Force Deployment Notes

- VST2 plugins run in 32-bit ARM compatibility layer
- Build with: `mpc-arm32-cmake` Docker image (arm32v7)
- glibc must be ≤ 2.36
- Plugin .so + bank file live in same directory (resolved from MPC.settings)
- `getMaker()` must return lowercase `"distrho"` for Force skin binding
- Version trio: `getVersion()=1000` ↔ meta `version="1.0"` ↔ version.xml `1.0.0.0`
