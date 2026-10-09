# Credits

MM-303 is a **derivative work**. It is based on
[JC-303](https://github.com/midilab/jc303) — a JUCE port of Robin Schmidt's
[Open303](https://github.com/RobinSchmidt/Open303) engine — and is built on the
work of others. It is licensed **GPL-3.0**; you are free to use, study, modify
and redistribute it, provided the copyright notices and the license are left
intact.

**Copyright (C) 2026 Amit Talwar <www.amitszone.com> <github.com/intelliriffer>**
and the upstream authors below. All third-party copyrights remain with their
owners and must be preserved.

With thanks:

## Engines / DSP

- **Open303** — Robin Schmidt ([github.com/RobinSchmidt/Open303](https://github.com/RobinSchmidt/Open303)).
  The 303 synth engine (`plugin/src/dsp/open303/`). **MIT License.**
- **JC-303** — midilab ([github.com/midilab/jc303](https://github.com/midilab/jc303)).
  The JUCE plugin this project is derived from. **GPL-3.0.**
- **GxDistortionPlus** — the [guitarix](https://guitarix.org/) project.
  MXR Distortion+ overdrive DSP (`plugin/src/dsp/mxr/`). **GPL-2.0-or-later.**
- **math_approx** — Jatin Chowdhury ([github.com/Chowdhury-DSP/math_approx](https://github.com/Chowdhury-DSP/math_approx)).
  `tanh<7>` approximation inlined in `dfl_DiodeLadderFilter.h`. **BSD-3-Clause.**

## Frameworks / libraries

- **JUCE** — Raw Material Software ([juce.com](https://juce.com/)). Framework
  (fetched by CMake). **GPL-3.0 / commercial.**
- **RtMidi** — Gary P. Scavone ([github.com/thestk/rtmidi](https://github.com/thestk/rtmidi)).
  Virtual ALSA MIDI output (`templates/rtmidi/`). **MIT.**

## ABI / headers

- **VST2 ABI declarations** — clean-room, BSD-2-Clause, from
  xaymar-vst2 ([github.com/Xaymar/vst2sdk](https://github.com/Xaymar/vst2sdk)),
  under `templates/vst2-abi/`. Clean-room declarations — no proprietary host-SDK code is used or vendored.

## Device / ABI references

- **mpc-vst-plugins** — sd88me ([github.com/sd88me/mpc-vst-plugins](https://github.com/sd88me/mpc-vst-plugins)).
  The VST2 binary ABI layout used here (the `AEffect` struct, events, time
  info) was cross-checked against this project's hand-written wrapper
  (`wrapper/vst2_wrap.c`), and its on-device route (native VST2 + Force
  skin / Q-Links) informed the packaging. **MIT License.**

## 

Full license texts and notices: [THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt).
