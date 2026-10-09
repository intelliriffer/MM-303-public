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

/// @file midi_to_mpcpattern.cpp
/// @brief Implementation of the SMF -> .mpcpattern converter (see the header).
///
/// Two output layouts:
///   v1 — flat mid2mpc clone (loads on every firmware), `type:2`, fields 1/2/3.
///   v2 — current Force layout: pattern version/quantisation/numFilterTypes and
///        a nested note object with 16 modifier slots.
///
/// Deliberately self-contained so it can be unit-tested without JUCE.

#include "midi_to_mpcpattern.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mm303 {
namespace {

// ── Minimal SMF reader ───────────────────────────────────────────────────────
struct Reader {
    const uint8_t* p;
    const uint8_t* end;

    bool ok() const { return p <= end; }
    bool eof() const { return p >= end; }
    size_t left() const { return (size_t) (end - p); }

    bool u8 (uint8_t& v)  { if (p >= end) return false; v = *p++; return true; }
    bool u16 (uint16_t& v) { if (left() < 2) return false; v = (uint16_t)((p[0] << 8) | p[1]); p += 2; return true; }
    bool u32 (uint32_t& v) { if (left() < 4) return false; v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; p += 4; return true; }
    bool bytes (const void* dst, size_t n) { if (left() < n) return false; std::memcpy ((void*) dst, p, n); p += n; return true; }
    bool skip (size_t n) { if (left() < n) return false; p += n; return true; }

    /// Variable-length quantity (7 bits per byte, high bit = "more").
    bool vlq (uint32_t& v) {
        v = 0;
        for (int i = 0; i < 4; ++i) {
            uint8_t b;
            if (! u8 (b)) return false;
            v = (v << 7) | (uint32_t)(b & 0x7F);
            if ((b & 0x80) == 0) return true;
        }
        return false;
    }
};

// A parsed MIDI channel event (running status resolved).  `sub` mirrors the
// MIDIFILE-library subtype the reference converter switches on.
struct Ev {
    int      sub;     // 8 noteOff, 9 noteOn, 0xa polyAft, 0xb cc, 0xc prog, 0xd chanAft, 0xe bend
    uint32_t tick;    // absolute ticks within its track
    int      p1, p2, p3;
};

bool readTrack (Reader& r, std::vector<Ev>& out) {
    uint32_t len = 0;
    if (! r.u32 (len)) return false;
    if (r.left() < len) return false;

    Reader t { r.p, r.p + len };
    r.p += len;

    uint32_t abs = 0;
    int      runningStatus = -1;

    while (! t.eof()) {
        uint32_t delta = 0;
        if (! t.vlq (delta)) return false;
        abs += delta;

        uint8_t status;
        if (! t.u8 (status)) return false;

        if (status == 0xFF) {                       // meta
            uint8_t type; uint32_t mlen;
            if (! t.u8 (type) || ! t.vlq (mlen) || ! t.skip (mlen)) return false;
            runningStatus = -1;
            continue;
        }
        if (status == 0xF0 || status == 0xF7) {     // sysex
            uint32_t slen;
            if (! t.vlq (slen) || ! t.skip (slen)) return false;
            runningStatus = -1;
            continue;
        }

        if (status & 0x80)                          // a real status byte
            runningStatus = status;
        else {                                      // running status: this byte is data
            if (runningStatus < 0) return false;
            t.p -= 1;                                // rewind so readData consumes it
        }

        const int type = runningStatus & 0xF0;

        auto readData = [&] (int count, int vals[2]) -> bool {
            for (int i = 0; i < count; ++i) { uint8_t b; if (! t.u8 (b)) return false; vals[i] = b; }
            return true;
        };

        int d[2] = { 0, 0 };
        switch (type) {
            case 0x80: if (! readData (2, d)) return false; out.push_back ({ 8,  abs, d[0], d[1], 0 }); break;
            case 0x90: if (! readData (2, d)) return false; out.push_back ({ 9,  abs, d[0], d[1], 0 }); break;
            case 0xA0: if (! readData (2, d)) return false; out.push_back ({ 0xA, abs, d[0], d[1], 0 }); break;
            case 0xB0: if (! readData (2, d)) return false; out.push_back ({ 0xB, abs, d[0], d[1], 0 }); break;
            case 0xC0: if (! readData (1, d)) return false; out.push_back ({ 0xC, abs, d[0], 0, 0 }); break;
            case 0xD0: if (! readData (1, d)) return false; out.push_back ({ 0xD, abs, d[0], 0, 0 }); break;
            case 0xE0: if (! readData (2, d)) return false; out.push_back ({ 0xE, abs, d[0], d[1], 0 }); break;
            default:   return false;
        }
    }
    return true;
}

bool parseSmf (const uint8_t* data, std::size_t size, std::vector<Ev>& evs, int& tpqn) {
    if (data == nullptr || size < 14) return false;
    Reader r { data, data + size };

    char hdr[4];
    uint32_t hlen = 0;
    uint16_t format = 0, ntracks = 0, division = 0;
    if (! r.bytes (hdr, 4) || std::memcmp (hdr, "MThd", 4) != 0) return false;
    if (! r.u32 (hlen) || ! r.u16 (format) || ! r.u16 (ntracks) || ! r.u16 (division)) return false;
    if (hlen > 6) r.skip (hlen - 6);

    if (division & 0x8000) return false;      // SMPTE time division — unsupported
    tpqn = division & 0x7FFF;
    if (tpqn <= 0) return false;

    for (uint16_t tk = 0; tk < ntracks && ! r.eof(); ++tk) {
        char id[4];
        if (! r.bytes (id, 4)) break;
        if (std::memcmp (id, "MTrk", 4) != 0) break;
        if (! readTrack (r, evs)) break;
    }
    return ! evs.empty();
}

/// Match each NoteOn to the next NoteOff of the same key (mid2mpc semantics).
std::vector<double> resolveDurations (const std::vector<Ev>& evs, double factor) {
    std::vector<double> dur (evs.size(), 0.0);
    struct Open { size_t index; int note; };
    std::vector<Open> open;
    for (size_t i = 0; i < evs.size(); ++i) {
        if (evs[i].sub == 9) {
            open.push_back ({ i, evs[i].p1 });
        } else if (evs[i].sub == 8) {
            for (size_t k = 0; k < open.size(); ++k) {
                if (open[k].note == evs[i].p1) {
                    dur[open[k].index] = ((double) evs[i].tick - evs[open[k].index].tick) * factor;
                    open.erase (open.begin() + (long) k);
                    break;
                }
            }
        }
    }
    return dur;
}

void appendInt (std::string& s, long long v) {
    char buf[32];
    std::snprintf (buf, sizeof buf, "%lld", v);
    s += buf;
}

void appendNum (std::string& s, double v) {
    if (std::isfinite (v) && v == std::floor (v) && std::fabs (v) < 9.0e18) {
        appendInt (s, (long long) v);
        return;
    }
    char buf[40];
    std::snprintf (buf, sizeof buf, "%.17g", v);
    s += buf;
}

// ── v1 renderer: flat mid2mpc layout ─────────────────────────────────────────
void appendV1Event (std::string& s, int key1, double key2, int type, double time, double len) {
    s += "      {\n        \"1\": ";
    appendInt (s, key1);
    s += ",\n        \"2\": ";
    appendNum (s, key2);
    s += ",\n        \"3\": 0,\n        \"type\": ";
    appendInt (s, type);
    s += ",\n        \"time\": ";
    appendNum (s, time);
    s += ",\n        \"len\": ";
    appendNum (s, len);
    s += ",\n        \"mod\": 0,\n        \"modVal\": 0.5\n      }";
}

bool renderV1 (const std::vector<Ev>& evs, const std::vector<double>& dur, double factor, std::string& out) {
    out  = "{\n  \"pattern\": {\n    \"length\": 9223372036854776000,\n    \"events\": [\n";
    bool first = true;
    for (size_t i = 0; i < evs.size(); ++i) {
        const Ev& e = evs[i];
        if (e.sub == 8 || e.sub == 0xC || e.sub == 0xD) continue;

        if (! first) out += ",\n";
        first = false;

        if (e.sub == 9)
            appendV1Event (out, e.p1, (double) e.p2 / 127.0, 2, (double) e.tick * factor, dur[i]);
        else if (e.sub == 0xA)
            appendV1Event (out, 131, (double) e.p2 / 127.0, 1, (double) e.tick, 0);
        else if (e.sub == 0xB)
            appendV1Event (out, e.p1, (double) e.p2 / 127.0, 1, (double) e.tick, 0);
        else if (e.sub == 0xE)
            appendV1Event (out, 128, (((double) e.p2 * 128.0) + e.p1) / 16384.0, 1, (double) e.tick, 0);
    }
    out += "\n    ]\n  }\n}\n";
    return true;
}

// ── v2 renderer: current Force layout ────────────────────────────────────────
const double kV2ModifierDefaults[16] = { 0.5, 0.5, 0.0, 0.0, 0.0, 0.5, 1.0, 0.0,
                                         0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

void appendV2Note (std::string& s, int note, double velocity, double length) {
    s += "        \"version\": 1,\n        \"note\": ";
    appendInt (s, note);
    s += ",\n        \"velocity\": ";
    appendNum (s, (double) (float) velocity);
    s += ",\n        \"length\": ";
    appendNum (s, length);
    s += ",\n        \"probability\": 100,\n        \"ratchet\": 1,\n        \"articulation\": 197,\n";
    for (int i = 0; i < 16; ++i) {
        s += "        \"modifierValue";
        appendInt (s, i);
        s += "\": ";
        appendNum (s, kV2ModifierDefaults[i]);
        s += ",\n        \"modifierActiveState";
        appendInt (s, i);
        s += "\": false,\n";
    }
    s += "        \"EnumCerealisationWrapper(selectedModifierType)\": \"Tuning (coarse)\"\n";
}

bool renderV2 (const std::vector<Ev>& evs, const std::vector<double>& dur, double factor, std::string& out) {
    out  = "{\n  \"pattern\": {\n    \"length\": 9223372036854775807,\n    \"events\": [\n";
    bool first = true;
    for (size_t i = 0; i < evs.size(); ++i) {
        const Ev& e = evs[i];
        if (e.sub != 9) continue;                    // v2: notes only

        if (! first) out += ",\n";
        first = false;

        out += "      {\n        \"version\": 2,\n        \"time\": ";
        appendNum (out, (double) e.tick * factor);
        out += ",\n        \"type\": 3,\n        \"channel\": 0,\n        \"selected\": false,\n"
               "        \"muted\": false,\n        \"invented\": false,\n        \"note\": {\n";
        appendV2Note (out, e.p1, (double) e.p2 / 127.0, dur[i]);
        out += "\n        }\n      }";   // close the note object, then the event
    }
    out += "\n    ],\n    \"version\": 2,\n    \"quantisation\": {\n"
           "      \"version\": 1,\n      \"pulses\": 0,\n      \"swing\": 0,\n      \"strength\": 1\n"
           "    },\n    \"numFilterTypes\": 30\n  }\n}\n";
    return true;
}

} // namespace

bool midiToMpcPattern (const uint8_t* data, std::size_t size, std::string& out) {
    std::vector<Ev> evs;
    int tpqn = 0;
    if (! parseSmf (data, size, evs, tpqn)) return false;
    const double factor = 960.0 / (double) tpqn;
    return renderV1 (evs, resolveDurations (evs, factor), factor, out);
}

bool midiToMpcPatternV2 (const uint8_t* data, std::size_t size, std::string& out) {
    std::vector<Ev> evs;
    int tpqn = 0;
    if (! parseSmf (data, size, evs, tpqn)) return false;
    const double factor = 960.0 / (double) tpqn;
    return renderV2 (evs, resolveDurations (evs, factor), factor, out);
}

} // namespace mm303
