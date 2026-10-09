#pragma once

// Compatibility shim for the vendored guitarix/Faust-generated DSP
// (DistortionPlus.cc / DistortionPlusPlus.cc / mxrclipping_table.h).
// Those files are copied verbatim from GxDistortionPlus.lv2; this header
// supplies the handful of macros/types the guitarix build normally provides.

#include <cstdint>
#include <cmath>
#include <algorithm>

using std::min;
using std::max;

typedef float FAUSTFLOAT;

#ifndef N_
#define N_(s) s
#endif

#define always_inline inline __attribute__((always_inline))
#define __rt_func
#define __rt_data

template <int N>
inline double faustpower(double x) { double r = 1.0; for (int i = 0; i < N; ++i) r *= x; return r; }

// Ports used by the generated `connect()` switches (cf. GxDistortionPlus.h).
enum PortIndex { DISTORTION = 0, OUTPUT = 1, BASSBOOST = 2, BYPASS = 3,
                 EFFECTS_INPUT = 4, EFFECTS_OUTPUT = 5 };
