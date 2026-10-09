#include "MxrDistortion.h"

#include "gx_compat.h"
#include "gx_pluginlv2.h"
#include "mxrclipping_table.h"

#include "DistortionPlus.cc"
#include "DistortionPlusPlus.cc"

#undef always_inline
#undef __rt_func
#undef __rt_data
#undef N_

struct MxrDistortion::Impl
{
    DistortionPlus::Dsp     base[2];
    DistortionPlusPlus::Dsp plus[2];
    float distortion = 0.5f;
    float output     = 0.5f;
    bool  bassBoost  = false;
    bool  prepared   = false;
};

MxrDistortion::MxrDistortion() : impl(new Impl()) {}
MxrDistortion::~MxrDistortion() = default;

void MxrDistortion::prepare(double sampleRate, int /*maxBlockSize*/)
{
    const uint32_t sr = static_cast<uint32_t>(sampleRate);

    for (int ch = 0; ch < 2; ++ch)
    {
        impl->base[ch].connect_ports(DISTORTION, &impl->distortion, &impl->base[ch]);
        impl->base[ch].connect_ports(OUTPUT,     &impl->output,     &impl->base[ch]);
        impl->base[ch].set_samplerate(sr, &impl->base[ch]);

        impl->plus[ch].connect_ports(DISTORTION, &impl->distortion, &impl->plus[ch]);
        impl->plus[ch].connect_ports(OUTPUT,     &impl->output,     &impl->plus[ch]);
        impl->plus[ch].set_samplerate(sr, &impl->plus[ch]);
    }
    impl->prepared = true;
}

void MxrDistortion::reset()
{
    for (int ch = 0; ch < 2; ++ch)
    {
        impl->base[ch].clear_state(&impl->base[ch]);
        impl->plus[ch].clear_state(&impl->plus[ch]);
    }
}

void MxrDistortion::setDistortion(float v) { impl->distortion = v; }
void MxrDistortion::setOutput(float v)     { impl->output = v; }
void MxrDistortion::setBassBoost(bool on)  { impl->bassBoost = on; }

void MxrDistortion::process(float* const* channels, int numChannels, int numSamples)
{
    if (!impl->prepared || numSamples <= 0)
        return;

    const int chCount = numChannels < 2 ? numChannels : 2;

    for (int ch = 0; ch < chCount; ++ch)
    {
        float* const io = channels[ch];
        if (impl->bassBoost)
            impl->plus[ch].mono_audio(numSamples, io, io, &impl->plus[ch]);
        else
            impl->base[ch].mono_audio(numSamples, io, io, &impl->base[ch]);
    }
}
