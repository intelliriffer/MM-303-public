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

/// @file pattern_gen.h
/// @brief Faithful extraction of AcidSequencer303::acidRandomize.

#pragma once

#include "pattern_to_midi.h"

#include <cstdint>

namespace mm303 {

/// @brief Fill `steps[0..maxSteps)` like the plugin's Generate button.
void randomizePattern (Step303* steps, int maxSteps,
                       int fill, int accentProbability, int slideProbability,
                       int tieProbability, int numberOfTones,
                       int lowerNote, int rangeNote, uint32_t& rngState);

} // namespace mm303
