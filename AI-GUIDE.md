# AI-GUIDE — MM-303

Entry point for a fresh session working on this repository. Read this first.

## What this is

MM-303 is a TB-303-style bass synth + acid step sequencer, packaged as a native
**VST2** plugin for the **Akai Force / MPC** standalone (armhf, glibc 2.31). It
is a derivative work based on [JC-303](https://github.com/midilab/jc303) — a
JUCE port of Robin Schmidt's [Open303](https://github.com/RobinSchmidt/Open303)
engine — with a native VST2 packaging pipeline, an MpcPreset shim, a virtual
ALSA MIDI output, and a Force skin. See [CREDITS.md](CREDITS.md).

Licensed **GPL-3.0** — see [LICENSE](LICENSE). Third-party notices:
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

## Repository map

```
MM-303/
├── build.sh          # the pipeline — main entry point (armhf)
├── recipe.sh         # fixed values (io, instrument)
├── plugin/           # the plugin source (CMake + JUCE). MODIFY HERE.
│   ├── CMakeLists.txt   # identity + MpcPreset shim wiring
│   └── src/             # dsp/, gui/, sequencer/, JC303.cpp ...
├── tools/            # build tools (Python/C)
├── templates/        # plugin-meta.xml, preset shim, rtmidi, vst2-abi headers
├── docker/           # Dockerfiles + package lists (armhf, glibc 2.31)
├── TUI-templates/    # skin templates used by tools/gen_tui_layout.py
├── skin/             # the Force touchscreen skin (images + assets.json)
├── THIRD-PARTY-NOTICES.txt
└── output/           # built packages (gitignored)
```

## Build

```sh
./build.sh
```

This builds the native VST2 in Docker, runs the device gates (`juce_smoke`,
`preset_smoke`, glibc / `DT_NEEDED` checks) and packages:

```
output/MockbaMod - VST - MM-303/
├── MM-303.so            # the plugin
├── MM-303.json          # parameter map
├── MM-303.conf          # config (capture path/format + MIDI map)
├── plugin-meta.xml
├── version.xml
├── licenses/
└── Plugin Skins/        # the Force skin (TUI.json + assets)
```

Options: `--clean` (clean, then ask to rebuild), `--no-tui`, `--no-presets`.

Requirements: **Docker** on the host (the build runs in an armhf `gcc:11`
container matching the device). No compiler/toolchain is needed on the host.
Internet access is needed on the first build (JUCE is fetched by CMake).

## How the build works

1. **Build** — `cmake` configure + `cmake --build --target MM303_VST`. The
   `MM303_VST` target links `mm303_preset_shim`; the shim exports the real VST2
   entry and adds the +4 preset controls (wired in `plugin/CMakeLists.txt`).
2. **Params** — `tools/juce_params_json.py` parses the APVTS constructor from
   `plugin/src/JC303.cpp` → `MM-303.json`.
3. **Gates** — `juce_smoke` (dlopen, `VSTPluginMain`, uid, io, param count,
   state round-trip), `preset_smoke` (preset controls), glibc + `DT_NEEDED`
   allowlist.
4. **Package** — `.so`, `.json`, `.conf`, `plugin-meta.xml`, `version.xml`,
   licenses.

## How to modify the plugin

Edit `plugin/src/**` and `plugin/CMakeLists.txt`. Identity lives in
`plugin/CMakeLists.txt`: `project(...)`, `PRODUCT_NAME`, `PLUGIN_NAME`,
`PLUGIN_CODE` (→ uid), `COMPANY_NAME`.

## How to modify presets

- The shim (`templates/preset/preset_shim.cpp` + `preset.cpp`) is linked into the
  VST2 target by `plugin/CMakeLists.txt` (`mm303_preset_shim`, `--whole-archive`).
- The +4 controls are described by `tools/preset.json` and appended to the json
  by `tools/juce_params_json.py`.

## Device constraints

- glibc ceiling **2.31**.
- No X11 / GL / GTK. Allowed device libs: `libc`, `libstdc++`, `libm`,
  `libgcc_s`, `libdl`, `libpthread`, `librt`, `libasound`, `libfontconfig`,
  `libfreetype`.
- 0 audio-in / 2 audio-out, MIDI-in (instrument).

## Common tasks

| Task | Do this |
|---|---|
| Build the package | `./build.sh` |
| Clean rebuild | `./build.sh --clean` (then `./build.sh`) |
| Skip presets | `./build.sh --no-presets` |
| Change name / uid | `plugin/CMakeLists.txt` (`PRODUCT_NAME`, `PLUGIN_NAME`, `PLUGIN_CODE`) |
| Change preset controls | `tools/preset.json` (+ `templates/preset/preset_shim.cpp`) |
| Add/remove a plugin param | `plugin/src/JC303.cpp` APVTS list; `MM-303.json` regenerates |
| Regenerate skin (TUI + Q-Links) | `./skinGen.sh` (no build), then reboot the device |

## Install (Akai Force / MPC)

1. Copy `output/MockbaMod - VST - MM-303/` to your device's Synths folder
   (e.g. `/media/<card>/Synths/`).
2. Run `vstscanner.sh` in your Synths folder (or reboot the device) so the
   plugin and its virtual MIDI port are rescanned.
3. Add MM-303 on an instrument track.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `no VST .so under plugin/build` | Build failed; check the VST2 build log. |
| `FAIL: no VSTPluginMain export` | The shim link broke (see `plugin/CMakeLists.txt`). |
| `FAIL - non-baseline DT_NEEDED` | A GUI lib got linked; the plugin must stay headless. |
| `juce_smoke` param-count mismatch | Source params changed; rebuild regenerates the json. |
| `preset_smoke` FAIL | The shim or `tools/preset.json` is broken. |
| `uid` changed unexpectedly | You changed `PLUGIN_CODE`; the uid is its hex. |

## License

GPL-3.0 — see [LICENSE](LICENSE). You are free to use, study, modify and
redistribute this software, provided the copyright notices and the license are
left intact. Copyright (C) 2026 Amit Talwar &lt;www.amitszone.com&gt;
&lt;github.com/intelliriffer&gt;.
