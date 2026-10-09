#pragma once
/// @file seqRotate.h
/// @brief Non-destructive pattern rotation: playback position -> stored index.
///
/// The stored pattern is always SEQ303_STEP_MAX steps; playback plays a window
/// of `length` steps starting at `rotation` (rotated by the global `dir`).
/// `rotation = 0` reads the original pattern unchanged.
///
/// @param p         playback position in [0, maxSteps)
/// @param rotation  signed rotation (e.g. -63..+63); 0 = no rotation
/// @param dir       global direction (+1 forward, -1 backward)
/// @param maxSteps  stored pattern size (SEQ303_STEP_MAX)
/// @return the stored step index in [0, maxSteps).
inline int mm303StepIndex (int p, int rotation, int dir, int maxSteps)
{
    const int off = rotation * dir;
    return ((p + off) % maxSteps + maxSteps) % maxSteps;
}
