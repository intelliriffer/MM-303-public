// Host-side fidelity check for the vendored MXR Distortion+ DSP + wrapper.
// Build: c++ -O2 -std=c++17 -I plugin/src/dsp/mxr \
//            tools/mxr_golden_test.cpp plugin/src/dsp/mxr/MxrDistortion.cpp \
//            -o /tmp/mxr_golden
// Run:   /tmp/mxr_golden        (exit 0 = pass)
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <vector>

#include "MxrDistortion.h"

static double tailRms(const std::vector<float>& v)
{
    double s = 0.0;
    const size_t n = v.size() / 2;
    for (size_t i = v.size() / 2; i < v.size(); ++i) s += (double) v[i] * v[i];
    return std::sqrt(s / n);
}

static bool check(const char* label, double got, double want)
{
    const bool ok = std::fabs(got - want) <= 2e-3;
    std::printf("%-18s got=%.5f want=%.5f %s\n", label, got, want, ok ? "OK" : "FAIL");
    return ok;
}

static bool sweep(bool bassBoost, const double* want, const char* tag)
{
    const double sr = 48000.0;
    const int    N  = 48000;
    const double freq = 220.0;
    const double dists[5] = { 0.0, 0.25, 0.5, 0.75, 1.0 };
    bool ok = true;

    for (int k = 0; k < 5; ++k)
    {
        MxrDistortion d;
        d.prepare(sr, N);
        d.setOutput(0.5f);
        d.setBassBoost(bassBoost);
        d.setDistortion((float) dists[k]);

        std::vector<float> a(N), b(N);
        for (int i = 0; i < N; ++i) a[i] = b[i] = 0.3f * std::sin(2.0 * M_PI * freq * i / sr);

        float* ch[2] = { a.data(), b.data() };
        d.process(ch, 2, N);

        char lblL[64], lblR[64];
        std::snprintf(lblL, sizeof(lblL), "%s dist=%.2f L", tag, dists[k]);
        std::snprintf(lblR, sizeof(lblR), "%s dist=%.2f R", tag, dists[k]);
        ok = check(lblL, tailRms(a), want[k]) && ok;
        ok = check(lblR, tailRms(b), want[k]) && ok;
    }
    return ok;
}

int main()
{
    const double base[5] = { 0.06190, 0.09587, 0.10696, 0.11026, 0.11088 };
    const double plus[5] = { 0.10812, 0.16600, 0.18165, 0.18448, 0.18485 };
    const bool ok = sweep(false, base, "base") && sweep(true, plus, "plus");
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
