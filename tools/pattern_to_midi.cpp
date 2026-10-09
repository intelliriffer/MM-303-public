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

/// @file pattern_to_midi.cpp

#include "pattern_to_midi.h"

#include <algorithm>

namespace mm303 {

int stepIndex (int p, int rotation, int dir, int maxSteps) {
    const int off = rotation * dir;
    return ((p + off) % maxSteps + maxSteps) % maxSteps;
}

// Faithful port of the note-generation loop in JC303::exportSequenceMidi.
// NOTE: the gate lookahead indexes `stored[next]` with the RAW position (not the
// rotation-mapped one), exactly as the current plugin does — kept so the test
// reproduces the current behaviour.
std::vector<Note303> patternToNotes (const Step303* stored, int maxSteps,
                                     int length, int rotation, int rotationDir,
                                     int stepTicks, int gateTicks, int slideExtra) {
    std::vector<Note303> out;
    if (stored == nullptr || maxSteps <= 0 || length <= 0) return out;
    length = std::min (length, maxSteps);

    for (int i = 0; i < length; ++i) {
        const Step303& s = stored[stepIndex (i, rotation, rotationDir, maxSteps)];
        if (s.rest) continue;

        int gate = gateTicks;
        for (int j = 1; j <= length; ++j) {
            const int next = stepIndex ((i + j) % length, rotation, rotationDir, maxSteps);
            if (s.slide && j == 1 && ! stored[next].rest) {
                gate = gateTicks + slideExtra;
                break;
            } else if (stored[next].tie && stored[next].rest) {
                gate = gateTicks + j * stepTicks;
            } else if (! stored[next].rest || ! stored[next].tie) {
                break;
            }
        }

        const int startTick = i * stepTicks;
        const int endTick   = std::min (startTick + gate, length * stepTicks);
        Note303 n;
        n.startTick = startTick;
        n.endTick   = endTick;
        n.note      = (int) s.note;
        n.velocity  = s.accent ? 127 : 70;
        out.push_back (n);
    }
    return out;
}

// Faithful port of AcidSequencer303::onStep's note generation (the LIVE output).
// Differences from the export: the gate lookahead uses the ROTATION-MAPPED step
// (`stepAt`), and the gate is NOT clamped at the loop end (a tie/slide can extend
// past it).
std::vector<Note303> liveToNotes (const Step303* stored, int maxSteps,
                                  int length, int rotation, int rotationDir,
                                  int stepTicks, int noteLengthTicks, int slideExtraTicks) {
    std::vector<Note303> out;
    if (stored == nullptr || maxSteps <= 0 || length <= 0) return out;
    length = std::min (length, maxSteps);

    for (int stepPos = 0; stepPos < length; ++stepPos) {
        const Step303& cur = stored[stepIndex (stepPos, rotation, rotationDir, maxSteps)];
        if (cur.rest) continue;

        int gate = noteLengthTicks;
        int nextStep = stepPos;
        for (int i = 1; i <= length; ++i) {
            nextStep = (nextStep + 1) % length;
            const Step303& ns = stored[stepIndex (nextStep, rotation, rotationDir, maxSteps)];
            if (cur.slide && i == 1 && ! ns.rest) { gate = noteLengthTicks + slideExtraTicks; break; }
            else if (ns.tie && ns.rest) { gate = noteLengthTicks + i * stepTicks; }
            else if (! ns.rest || ! ns.tie) break;
        }
        // Realtime clamp at the loop point (a boundary tie/slide stops there).
        const int toLoopEnd = (length - stepPos) * stepTicks;
        if (gate > toLoopEnd) gate = toLoopEnd;
        if (gate < 1) gate = 1;

        Note303 n;
        n.startTick = stepPos * stepTicks;
        n.endTick   = n.startTick + gate;   // == export's clamp now
        n.note      = (int) cur.note;
        n.velocity  = cur.accent ? 127 : 70;
        out.push_back (n);
    }
    return out;
}

namespace {
void put16 (std::vector<uint8_t>& b, uint16_t v) { b.push_back ((uint8_t)(v >> 8)); b.push_back ((uint8_t) v); }
void put32 (std::vector<uint8_t>& b, uint32_t v) { b.push_back ((uint8_t)(v >> 24)); b.push_back ((uint8_t)(v >> 16)); b.push_back ((uint8_t)(v >> 8)); b.push_back ((uint8_t) v); }
void vlq (std::vector<uint8_t>& b, uint32_t v) {
    uint8_t tmp[5]; int n = 0;
    tmp[n++] = (uint8_t)(v & 0x7F);
    while ((v >>= 7) != 0) tmp[n++] = (uint8_t)((v & 0x7F) | 0x80);
    while (n > 0) b.push_back (tmp[--n]);
}
} // namespace

std::vector<uint8_t> notesToSmf (const std::vector<Note303>& notes, int ppq, double bpm) {
    struct E { int tick; bool on; int note; int vel; };
    std::vector<E> evs;
    for (const auto& n : notes) {
        evs.push_back ({ n.startTick, true,  n.note, n.velocity });
        evs.push_back ({ n.endTick,   false, n.note, 0 });
    }
    std::stable_sort (evs.begin(), evs.end(), [] (const E& a, const E& b) { return a.tick < b.tick; });

    std::vector<uint8_t> trk;
    // tempo + time-signature metas
    const uint32_t tempo = (uint32_t) (60000000.0 / std::max (20.0, bpm));
    vlq (trk, 0); trk.push_back (0xFF); trk.push_back (0x51); trk.push_back (0x03);
    trk.push_back ((uint8_t)(tempo >> 16)); trk.push_back ((uint8_t)(tempo >> 8)); trk.push_back ((uint8_t) tempo);
    vlq (trk, 0); trk.push_back (0xFF); trk.push_back (0x58); trk.push_back (0x04);
    trk.push_back (4); trk.push_back (2); trk.push_back (24); trk.push_back (8);

    int last = 0;
    for (const auto& e : evs) {
        vlq (trk, (uint32_t) (e.tick - last)); last = e.tick;
        if (e.on) { trk.push_back (0x90); trk.push_back ((uint8_t) e.note); trk.push_back ((uint8_t) e.vel); }
        else      { trk.push_back (0x80); trk.push_back ((uint8_t) e.note); trk.push_back (0x00); }
    }
    vlq (trk, 0); trk.push_back (0xFF); trk.push_back (0x2F); trk.push_back (0x00);

    std::vector<uint8_t> out;
    out.push_back ('M'); out.push_back ('T'); out.push_back ('h'); out.push_back ('d');
    put32 (out, 6); put16 (out, 1); put16 (out, 1); put16 (out, (uint16_t) ppq);
    out.push_back ('M'); out.push_back ('T'); out.push_back ('r'); out.push_back ('k');
    put32 (out, (uint32_t) trk.size());
    out.insert (out.end(), trk.begin(), trk.end());
    return out;
}

} // namespace mm303
