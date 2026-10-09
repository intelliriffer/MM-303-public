// MM-303 — pattern -> MIDI extraction (Capture algorithm) + a tiny SMF writer.
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

/// @file pattern_to_midi.h
/// @brief The Capture note-generation algorithm, extracted from
///        JC303::exportSequenceMidi so it can be built and tested standalone.

#pragma once

#include <cstdint>
#include <vector>

namespace mm303 {

/// One stored step (the plugin's StepData303).
struct Step303 {
    uint8_t note   = 36;
    bool    rest   = false;
    bool    accent = false;
    bool    slide  = false;
    bool    tie    = false;
};

/// One note-on/note-off pair in 960-PPQ ticks.
struct Note303 {
    int startTick = 0;
    int endTick   = 0;
    int note      = 36;
    int velocity  = 70;
};

/// @brief Stored index for playback position `p` (mirrors seqRotate.h).
int stepIndex (int p, int rotation, int dir, int maxSteps);

/// @brief The Capture note generation, faithful to JC303::exportSequenceMidi.
/// @param stored     stored pattern (maxSteps entries)
/// @param maxSteps   stored size (SEQ303_STEP_MAX = 64)
/// @param length     loop length (1..maxSteps)
/// @param rotation   signed rotation
/// @param rotationDir +1 / -1
/// @param stepTicks  ticks per 16th step (960 PPQ -> 240)
/// @param gateTicks  base gate (50%) in ticks
/// @param slideExtra slide overhang in ticks
std::vector<Note303> patternToNotes (const Step303* stored, int maxSteps,
                                     int length, int rotation, int rotationDir,
                                     int stepTicks, int gateTicks, int slideExtra);

/// @brief The note generation the LIVE sequencer actually plays (faithful to
///        AcidSequencer303::onStep): rotation-aware gate lookahead, NOT clamped
///        at the loop end (a tie/slide may extend past it).
std::vector<Note303> liveToNotes (const Step303* stored, int maxSteps,
                                  int length, int rotation, int rotationDir,
                                  int stepTicks, int noteLengthTicks, int slideExtraTicks);

/// @brief Serialise notes to a Standard MIDI File (format 1, one track, ppq).
std::vector<uint8_t> notesToSmf (const std::vector<Note303>& notes, int ppq, double bpm);

} // namespace mm303
