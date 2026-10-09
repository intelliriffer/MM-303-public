// MM-303 — Standard MIDI File -> Akai .mpcpattern converter (standalone test).
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

/// @file midi_to_mpcpattern_test.cpp
/// @brief Standalone test for the SMF -> .mpcpattern converter.
///
/// Usage: midi_to_mpcpattern_test <input.mid> [output.mpcpattern] [format]
///        format: 1 = v1 (default), 2 = v2.

#include "midi_to_mpcpattern.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main (int argc, char** argv) {
    if (argc < 2) {
        std::fprintf (stderr, "usage: %s <input.mid> [out.mpcpattern] [1|2]\n", argv[0]);
        return 2;
    }
    const int format = (argc >= 4) ? std::atoi (argv[3]) : 1;

    std::FILE* f = std::fopen (argv[1], "rb");
    if (! f) { std::fprintf (stderr, "cannot open %s\n", argv[1]); return 2; }
    std::fseek (f, 0, SEEK_END);
    const long n = std::ftell (f);
    std::fseek (f, 0, SEEK_SET);
    std::vector<uint8_t> buf ((size_t) (n > 0 ? n : 0));
    if (! buf.empty() && std::fread (buf.data(), 1, buf.size(), f) != buf.size()) {
        std::fclose (f); std::fprintf (stderr, "read error\n"); return 2;
    }
    std::fclose (f);

    std::string json;
    const bool ok = (format == 2)
        ? mm303::midiToMpcPatternV2 (buf.data(), buf.size(), json)
        : mm303::midiToMpcPattern   (buf.data(), buf.size(), json);
    if (! ok) {
        std::fprintf (stderr, "conversion FAILED for %s\n", argv[1]);
        return 1;
    }

    long events = 0;
    for (size_t i = 0; (i = json.find ("\"type\"", i)) != std::string::npos; ++i) ++events;
    std::printf ("OK: %s -> v%d, %ld events, %zu bytes\n", argv[1], format, events, json.size());

    if (argc >= 3) {
        std::FILE* o = std::fopen (argv[2], "wb");
        if (! o) { std::fprintf (stderr, "cannot write %s\n", argv[2]); return 2; }
        std::fwrite (json.data(), 1, json.size(), o);
        std::fclose (o);
        std::printf ("wrote %s\n", argv[2]);
    }
    return 0;
}
