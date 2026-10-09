#pragma once
#include <cmath>
/// @file seqTranspose.h
/// @brief MIDI-note -> sequence transpose mapping (pitch class + optional octave).
///
/// The pattern is generated in the key of its "Lower Note" parameter; an
/// incoming note re-keys it by the signed pitch-class difference. When octave
/// transposition is enabled the played note's octave (relative to the base
/// octave) is added too, clamped so the shifted pattern stays in 0..127.
///
/// @param playedNote  incoming MIDI note number (0..127)
/// @param lowNote     the pattern's "Lower Note" parameter value
/// @param rangeNote   the pattern's "Range Note" (span above lowNote); 0 = ignore
/// @param baseNote    MIDI note of the base octave's C (= base-octave*12, e.g. 60
///                    for octave 5, 48 for octave 4); <= 0 disables the term
/// @param maxOctaves  octave clamp (e.g. 1 or 2); <= 0 disables the octave term
/// @return signed semitone offset (0 when the played note's pitch class equals
///         the low note's and no octave term applies).
inline int mm303TransposeShift (int playedNote, int lowNote,
                                int rangeNote = 0,
                                int baseNote = 0, int maxOctaves = 0)
{
    const int lowPC    = ((lowNote    % 12) + 12) % 12;
    const int playedPC = ((playedNote % 12) + 12) % 12;
    int shift = playedPC - lowPC;                       // -11..+11 (key lock)

    if (maxOctaves <= 0 || baseNote <= 0)
        return shift;                                   // pitch-class only

    int oct = static_cast<int> (std::floor ((playedNote - baseNote) / 12.0));
    if (oct >  maxOctaves) oct =  maxOctaves;
    if (oct < -maxOctaves) oct = -maxOctaves;

    // Shrink toward 0 until the shifted pattern stays inside 0..127.
    while (oct != 0)
    {
        const int s = shift + 12 * oct;
        if (lowNote + s >= 0 && lowNote + rangeNote + s <= 127)
            break;
        oct += (oct > 0) ? -1 : 1;
    }
    return shift + 12 * oct;
}
