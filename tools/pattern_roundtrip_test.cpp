// MM-303 — random pattern -> MIDI -> .mpcpattern round-trip validation.
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

/// @file pattern_roundtrip_test.cpp
/// @brief Generate random patterns, run the Capture algorithm -> SMF ->
///        .mpcpattern, and verify the pattern file matches the source notes.
///
/// Usage: pattern_roundtrip_test [iterations] [seed]

#include "midi_to_mpcpattern.h"
#include "pattern_gen.h"
#include "pattern_to_midi.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace mm303;

namespace {

double field (const std::string& block, const char* key) {
    const std::string k = std::string ("\"") + key + "\":";
    auto p = block.find (k);
    if (p == std::string::npos) return -1.0e18;
    p = block.find (':', p) + 1;
    return std::strtod (block.c_str() + p, nullptr);
}

struct PE { int note = 0, type = 0; double time = 0, len = 0; };

std::vector<PE> parseV1 (const std::string& json) {
    std::vector<PE> out;
    auto e = json.find ("\"events\":");
    if (e == std::string::npos) return out;
    size_t i = json.find ('[', e);
    if (i == std::string::npos) return out;
    while (true) {
        auto b = json.find ('{', i);
        if (b == std::string::npos) break;
        auto end = json.find ('}', b);
        if (end == std::string::npos) break;
        const std::string block = json.substr (b, end - b + 1);
        PE pe;
        pe.note = (int) field (block, "1");
        pe.type = (int) field (block, "type");
        pe.time = field (block, "time");
        pe.len  = field (block, "len");
        out.push_back (pe);
        i = end + 1;
    }
    return out;
}

} // namespace

int main (int argc, char** argv) {
    const int iters = (argc >= 2) ? std::atoi (argv[1]) : 200;
    uint32_t seed   = (argc >= 3) ? (uint32_t) std::strtoul (argv[2], nullptr, 0) : 0x12345678u;

    const int MAX = 64, STEP = 240, GATE = 120, SLIDE = 200, PPQ = 960;
    int totalFail = 0, totalNotes = 0, firstBad = -1;
    int onsetFail = 0, firstOnset = -1;

    for (int it = 0; it < iters; ++it) {
        // Random gen params + length + rotation.
        const int fill    = 20 + (int) (seed % 70); seed = seed * 1664525u + 1013904223u;
        const int accentP = (int) (seed % 80);      seed = seed * 1664525u + 1013904223u;
        const int slideP  = (int) (seed % 50);      seed = seed * 1664525u + 1013904223u;
        const int tieP    = (int) (seed % 40);      seed = seed * 1664525u + 1013904223u;
        const int tones   = 1 + (int) (seed % 12);  seed = seed * 1664525u + 1013904223u;
        const int lowNote = (int) (seed % 60);      seed = seed * 1664525u + 1013904223u;
        const int range   = 1 + (int) (seed % 24);  seed = seed * 1664525u + 1013904223u;
        const int length  = 1 + (int) (seed % 64);  seed = seed * 1664525u + 1013904223u;
        const int rot     = (int) (seed % 127) - 63; seed = seed * 1664525u + 1013904223u;

        std::vector<Step303> stored (MAX);
        randomizePattern (stored.data(), MAX, fill, accentP, slideP, tieP, tones, lowNote, range, seed);

        const auto notes = patternToNotes (stored.data(), MAX, length, rot, +1, STEP, GATE, SLIDE);
        totalNotes += (int) notes.size();

        // Export vs LIVE onsets: these MUST be identical. Any difference is the
        // "extra onset at the start" bug.
        {
            const auto live = liveToNotes (stored.data(), MAX, length, rot, +1, STEP, GATE, SLIDE);
            bool bad = (live.size() != notes.size());
            if (! bad)
                for (size_t k = 0; k < notes.size(); ++k)
                    if (live[k].startTick != notes[k].startTick || live[k].endTick != notes[k].endTick
                        || live[k].note != notes[k].note || live[k].velocity != notes[k].velocity) { bad = true; break; }
            if (bad) {
                ++onsetFail;
                if (firstOnset < 0) {
                    firstOnset = it;
                    std::printf ("ONSET MISMATCH iter %d (len=%d rot=%d): export=%zu live=%zu\n",
                                 it, length, rot, notes.size(), live.size());
                    for (size_t k = 0; k < std::max (notes.size(), live.size()) && k < 10; ++k) {
                        std::printf ("   [%zu] export(%s)  live(%s)\n", k,
                                     k < notes.size() ? (std::to_string (notes[k].startTick) + "/" + std::to_string (notes[k].note)).c_str() : "-",
                                     k < live.size()  ? (std::to_string (live[k].startTick) + "/" + std::to_string (live[k].note)).c_str() : "-");
                    }
                }
            }
        }

        const auto smf = notesToSmf (notes, PPQ, 128.0);
        std::string json;
        if (! midiToMpcPattern (smf.data(), smf.size(), json)) {
            // An empty pattern has no notes -> the converter (like mid2mpc) has
            // nothing to emit and returns false. That is expected, not a failure.
            if (notes.empty()) continue;
            std::printf ("iter %d: conversion FAILED (len=%d rot=%d fill=%d notes=%zu smf=%zu)\n",
                         it, length, rot, fill, notes.size(), smf.size());
            ++totalFail;
            if (firstBad < 0) firstBad = it;
            { FILE* fp = std::fopen ("/tmp/fail.mid", "wb"); if (fp) { std::fwrite (smf.data(), 1, smf.size(), fp); std::fclose (fp); } }
            continue;
        }
        const auto pev = parseV1 (json);

        // Compare: the pattern file's note events must equal the source notes.
        std::vector<PE> want;
        for (const auto& n : notes)
            want.push_back ({ n.note, 2, (double) n.startTick, (double) (n.endTick - n.startTick) });

        bool ok = (pev.size() == want.size());
        if (ok) {
            for (size_t k = 0; k < pev.size(); ++k) {
                if (pev[k].note != want[k].note || pev[k].type != want[k].type
                    || std::fabs (pev[k].time - want[k].time) > 1e-6
                    || std::fabs (pev[k].len  - want[k].len)  > 1e-6) { ok = false; break; }
            }
        }
        if (! ok) {
            ++totalFail;
            if (firstBad < 0) firstBad = it;
            if (totalFail <= 3) {
                std::printf ("iter %d MISMATCH: src notes=%zu, pattern events=%zu (len=%d rot=%d fill=%d)\n",
                             it, want.size(), pev.size(), length, rot, fill);
                for (size_t k = 0; k < std::max (want.size(), pev.size()) && k < 8; ++k) {
                    std::printf ("   [%zu] src(%s)  pat(%s)\n", k,
                                 k < want.size() ? (std::to_string (want[k].time) + "/" + std::to_string (want[k].note) + "/" + std::to_string (want[k].len)).c_str() : "-",
                                 k < pev.size()  ? (std::to_string ((long) pev[k].time) + "/" + std::to_string (pev[k].note) + "/" + std::to_string ((long) pev[k].len)).c_str() : "-");
                }
            }
        }
    }

    std::printf ("\nroundtrip: %d iters, %d notes, %d converter mismatches, %d export-vs-live onset mismatches%s\n",
                 iters, totalNotes, totalFail, onsetFail,
                 (firstBad >= 0 || firstOnset >= 0) ? " (SEE ABOVE)" : "");
    return (totalFail == 0 && onsetFail == 0) ? 0 : 1;
}
