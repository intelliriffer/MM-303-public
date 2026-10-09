// MM-303 — generative pattern extraction (acidRandomize) for standalone tests.
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

/// @file pattern_gen.cpp

#include "pattern_gen.h"

namespace mm303 {
namespace {
// xorshift32 — stands in for the plugin's juce::Random.
inline uint32_t next (uint32_t& s) {
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;
    return s;
}
inline int percent (uint32_t& s) { return (int) (next (s) % 100u); }
inline int clamp (int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
} // namespace

void randomizePattern (Step303* steps, int maxSteps,
                       int fill, int accentProbability, int slideProbability,
                       int tieProbability, int numberOfTones,
                       int lowerNote, int rangeNote, uint32_t& rngState) {
    if (steps == nullptr || maxSteps <= 0) return;

    int fixTones[12] = {};
    if (numberOfTones > 0) {
        const int fixMod = 12 / numberOfTones;
        int fixCounter = 0;
        for (int i = 0; i < 12; ++i) {
            if ((i % fixMod) == 0 || i == 0) { fixTones[i] = 0; fixCounter = 0; }
            else { --fixCounter; fixTones[i] = fixCounter; }
        }
    }

    for (int i = 0; i < maxSteps; ++i) {
        steps[i] = Step303 { 36, true, false, false, false };
    }

    for (int i = 0; i < maxSteps; ++i) {
        const bool isOn = (percent (rngState) < fill);
        steps[i].rest = ! isOn;

        if (! isOn) {
            steps[i].accent = false;
            steps[i].slide  = false;
            steps[i].tie    = false;
            const int lastIdx = (i == 0) ? maxSteps - 1 : i - 1;
            if (! steps[lastIdx].rest || steps[lastIdx].tie)
                steps[i].tie = (percent (rngState) < tieProbability);
            continue;
        }

        const int highNote = clamp (0, 127, lowerNote + rangeNote);
        const int noteRange = (highNote > lowerNote) ? (highNote - lowerNote) : 1;
        int note = lowerNote + (percent (rngState) % noteRange);
        if (numberOfTones > 0)
            note = clamp (0, 127, note + fixTones[note % 12]);

        steps[i].note   = (uint8_t) note;
        steps[i].accent = (percent (rngState) < accentProbability);
        steps[i].slide  = (percent (rngState) < slideProbability);
        steps[i].tie    = false;
    }
}

} // namespace mm303
