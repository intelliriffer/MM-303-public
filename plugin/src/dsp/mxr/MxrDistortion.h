#pragma once

#include <memory>

// JUCE-free wrapper around the vendored MXR Distortion+ DSP.
// One instance keeps independent state per channel; setBassBoost selects the
// base (DistortionPlus) or bassboost (DistortionPlusPlus) DSP, mirroring the
// original Gx LV2 plugin.
class MxrDistortion
{
public:
    MxrDistortion();
    ~MxrDistortion();

    void prepare(double sampleRate, int maxBlockSize);
    void reset();

    void setDistortion(float v);   // DISTORTION, 0..1
    void setOutput(float v);       // OUTPUT, 0..1
    void setBassBoost(bool on);

    void process(float* const* channels, int numChannels, int numSamples);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;

    MxrDistortion(const MxrDistortion&) = delete;
    MxrDistortion& operator=(const MxrDistortion&) = delete;
};
