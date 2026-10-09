// MM-303 — Standard MIDI File -> Akai .mpcpattern converter.
// Copyright (C) 2026 Amit Talwar <www.amitszone.com> <github.com/intelliriffer>
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. You are free to use, study, modify and redistribute this code,
// provided this copyright notice and the license are retained.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with
// this program. If not, see <https://www.gnu.org/licenses/>.

/// @file midi_to_mpcpattern.h
/// @brief Standard MIDI File -> Akai `.mpcpattern` JSON converter.
///
/// Self-contained (no JUCE, no third-party libs): parses an SMF (format 0/1)
/// and emits the JSON the Akai Force/MPC load for patterns.  Times are scaled
/// to 960 PPQ (the Force's resolution), matching the reference `mid2mpc.js`:
///
///   {"pattern":{"length":<int64max>,"events":[
///       {"1":note,"2":vel/127,"3":0,"type":2,"time":tick,"len":len,"mod":0,"modVal":0.5},
///       ... ]}}
///
/// Notes are `type:2`; CC / poly-aftertouch / pitch-bend are `type:1`.
///
/// This file is built into the plugin and also compiles standalone for the
/// test in tools/midi_to_mpcpattern_test.cpp.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace mm303 {

/// @brief Convert an SMF image to `.mpcpattern` JSON text, **format v1** — the
///        flat mid2mpc layout that loads on every Force/MPC firmware.
/// @param data  Raw SMF bytes.
/// @param size  Byte count.
/// @param out   Receives the JSON on success.
/// @return true on success (parsed + converted), false on malformed input.
bool midiToMpcPattern (const uint8_t* data, std::size_t size, std::string& out);

/// @brief Convert an SMF image to `.mpcpattern` JSON text, **format v2** — the
///        current Force layout (nested note object + modifiers).  Newer firmware
///        only; kept so the library stays useful if v1 support is dropped.
bool midiToMpcPatternV2 (const uint8_t* data, std::size_t size, std::string& out);

} // namespace mm303
