# MM-303

A TB-303-style bass synthesiser + acid step sequencer, packaged as a native
**VST2** plugin for the **Akai Force** and **Akai MPC** standalone
(armhf, glibc 2.31).

## Derivative work & license

MM-303 is a **derivative work**. It is based on
[JC-303](https://github.com/midilab/jc303) — a JUCE port of Robin Schmidt's
[Open303](https://github.com/RobinSchmidt/Open303) engine — with a native VST2
packaging pipeline, an MpcPreset shim, a virtual ALSA MIDI output, a Force skin,
and the set of added features listed below.

It is licensed **GPL-3.0** (see [LICENSE](LICENSE)). You are **free to use,
study, modify and redistribute** this software, **provided the copyright notices
and the license are left intact**. Copyright (C) 2026 **Amit Talwar**
&lt;www.amitszone.com&gt; &lt;github.com/intelliriffer&gt;, together with the
upstream authors (see [CREDITS.md](CREDITS.md)). Third-party components keep
their own licenses — full texts in
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

## MM-303 — Main Extra Features

These are the additions MM-303 makes on top of the upstream JC-303 / Open303:

1. **Distortion engine replaced by Distortion+ (GxDistortionPlus)** — the MXR
   Distortion+ overdrive, replacing the original distortion (cheaper on CPU).
2. **Tune ±7 semitones (perfect fifths).**
3. **Red clipping indicator LED on all tabs.**
4. **Green LED indicator** to acknowledge a momentary action
   (Save / Load / Re-Gen / Clear / Capture).
5. **DSP Bypass** (Mute button next to the Play button) — disables the DSP/synth
   to save CPU if you only need its MIDI output to drive another synth.
6. **MIDI Output.** The internal sequencer pattern is also emitted on a
   `MockbaMod MM-303` MIDI port. A new port is created per instance, so the next
   port is `MockbaMod MM-303 2`, etc. *Experimental — untested; more than one
   instance may crash.*
7. **Capture.** Saves the current internal sequence as a MIDI file, or as a
   `.mpcpattern` (v1, old firmware) / `.mpcpattern` v2 (new firmware) file. You
   choose the format and the output folder in the config file.
8. **Custom MIDI Mapping.** If you use an external controller, the MIDI mapping
   for the main functions can be defined in the config file.
9. **Config file.** Many settings are configurable via a plain-text config file;
   a default one is created on first run if missing.
10. **0.01–20 Hz LFO rate.** The original JC-303 / Open303 had a lower range of
    0.1 Hz (10 s max); MM-303 goes down to 0.01 Hz (100 s), so a tri-peak at
    around 50 s.
11. **64-step pattern.** Based on my own Harpie4t / Riffmaker 4T apps, the maximum
    length is raised to 64 steps. Every regeneration produces a 64-step pattern;
    the pattern only plays/loops up to the Length parameter — which leads to
    Rotate.
12. **Rotate ±63 steps.** Rotate shifts the pattern in either direction. Given a
    single pattern it yields many variations: with rotation the pattern does not
    wrap at `Length`, it wraps at the full 64 steps, so each rotation brings in a
    new step and alters the pattern completely — letting you jump to a value and
    back, playing the same pattern as before. Map different rotation values to a
    Q-Link or a pad macro (low and high same value) to jump between phrases.
    Rotation queues and applies at the beginning of a pattern, never in-between.
13. **TRANSP.** When using the internal sequencer, any MIDI note to MM-303
    determines its pitch (C–B) — just the note — and, based on the Sequencer Low
    Note, applies a pitch offset to the output, letting you change pattern pitch
    while it plays.
14. **OCTAVE.** Works only when TRANSP is enabled: pitch up/down by up to 2
    octaves based on incoming MIDI notes. The middle octave 5 (note 60) is the
    default base reference, configurable in the config file.
15. **128-slot preset bank.** 128 snapshot slots capture the current state of most
    parameters and recall it — including the pattern, its rotation and length.
    Transpose/Octave are **not** saved (they are real-time performance controls).
    - **CC#0 (Bank MSB):** send the slot number as the value to load that snapshot.
      Note: Akai filters Program Change messages, so they never reach the plugin.
      If you assign a value to the bank in clip settings it only transmits while
      the Force is stopped — and while running Akai filters that out too. A
      workaround is to place the message via List View as the first event of a
      clip and double the clip length, looping only the second half; or use a
      separate MIDI track with a clip (loop off) holding these bank-change
      messages. (Needs public testing.)
    - **Prog# box:** the red box before the Load button is the program-number box.
      Select it and use the data dial to choose the slot, then press Load or Save
      to load/save a snapshot there. The top Preset menu is also populated with
      slot numbers (`#000`, `#001`, … `#127`); selecting one loads that slot
      instantly and updates Prog#.

## 🛠️ Build Requirements

This project was developed and tested **only using Docker** on **macOS**.

- **Supported build pipeline:** use the provided `Dockerfile` and `build.sh`.
- **Other platforms:** native builds for Windows or other environments are **not**
  officially supported or tested. Users are responsible for adapting the build
  chain to their own systems.

What you need:
- **Docker** (the build runs in an armhf `gcc:11` container matching the device).
  - macOS / Windows: install [Docker Desktop](https://www.docker.com/products/docker-desktop/).
  - Linux: install Docker Engine + armhf emulation
    (`docker run --privileged --rm tonistiigi/binfmt --install arm`).
- Internet access on first build (JUCE is fetched by CMake).
- No compiler/toolchain needed on the host.

## Build

```sh
./build.sh
```

This builds the native VST2 in Docker, runs the device gates (`juce_smoke`,
`preset_smoke`, `midi_layout_check`, glibc / `DT_NEEDED` checks) and packages:

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

Options: `--clean` (clean everything, then ask to rebuild), `--no-tui`,
`--no-presets`.

## Install (Akai Force / MPC)

Copy the `MockbaMod - VST - MM-303/` folder to your Synths folder, then run
`vstscanner.sh` in your Synths folder.

## Configuration — `MM-303.conf`

Sits next to the `.so` (created on first launch). Plain text, one `key = value`
per line, `#` comments anywhere, blank lines ignored.

```
# Where the Capture button writes files (default: <plugin dir>/Midi Clips)
capture-path = /media/662522/303

# What Capture writes: 0 = MIDI (.mid), 1 = .mpcpattern v1, 2 = .mpcpattern v2
captureFormat = 0

# Positional transpose reference (used when Transpose is on)
base-octave = 5
octave-max = 2

# MIDI CC map: <param-id> = <cc>. Responds on any channel.
# Delete a line to make that parameter NOT midi-mappable.
cutoff = 74
resonance = 71
```

- Switch parameters: CC `< 5` = off, `>= 5` = on.
- The default config is embedded from `templates/MM-303.conf` at build time.

## Skin / TUI

`skin/` holds the touchscreen skin (images + `assets.json`); the parameter pages
are generated from `TUI-templates/layout.conf` by `tools/gen_tui_layout.py` and
assembled into the package's `Plugin Skins/` by `build.sh`.

## Credits

See [CREDITS.md](CREDITS.md). In short: Open303 (MIT), JC-303 (GPL-3.0),
GxDistortionPlus (GPL-2.0-or-later), math_approx (BSD-3-Clause), JUCE
(GPL-3.0 / commercial), RtMidi (MIT), Xaymar vst2sdk (BSD-2-Clause),
mpc-vst-plugins (MIT). Full texts: [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).

## License

GPL-3.0 — see [LICENSE](LICENSE). You are free to use, study, modify and
redistribute this software, provided the copyright notices and the license are
left intact. Copyright (C) 2026 Amit Talwar &lt;www.amitszone.com&gt;
&lt;github.com/intelliriffer&gt;.
