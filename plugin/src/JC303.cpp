/// @file JC303.cpp
/// @brief MM-303 processor implementation: APVTS wiring, the real-time render
///        pipeline (sequencer -> engine -> overdrive) and state/config handling.
#include "JC303.h"
#include GUI_THEME_HEADER
#include "seqTranspose.h"
#include "seqRotate.h"
#include "midi_to_mpcpattern.h"
#include <unistd.h>

// Bypass fade times (seconds). Out is fast (well under one 128-sample buffer at
// 44.1 kHz = 2.9 ms); in is slower so a resumed engine's transition is masked.
#define MM303_BYPASS_FADE_OUT 0.002
#define MM303_BYPASS_FADE_IN  0.030

// Optional hook provided by the MpcPreset shim (templates/preset/preset_shim.cpp)
// when it is linked: blinks the Status LED for momentary trigger controls.
extern "C" void mm303ShimBlinkStatus(void) __attribute__((weak));
// Optional hook: lights the shim's Clip LED when the output clips.
extern "C" void mm303ShimClipStatus(void) __attribute__((weak));
// Optional hook: the shim's resolved plugin install directory (so we can write
// "Midi Clips" next to the .so).
extern "C" const char* mm303ShimPluginDir(void) __attribute__((weak));

//==============================================================================
/// @brief Builds the APVTS parameter layout and wires up listeners + engine.
JC303::JC303()
     : AudioProcessor (BusesProperties()
                     #if ! JucePlugin_IsMidiEffect
                      #if ! JucePlugin_IsSynth
                       .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                      #endif
                       .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                     #endif
                       ),
       parameters (*this, nullptr, juce::Identifier("APVTS"), {
            std::make_unique<juce::AudioParameterFloat> ("waveform",
                                                        "Waveform",
                                                        0.0f,
                                                        1.0f,
                                                        1.0f),
            std::make_unique<juce::AudioParameterFloat> ("tuning",
                                                        "Tuning",
                                                        0.0f,
                                                        1.0f,
                                                        0.5f),
            std::make_unique<juce::AudioParameterFloat> ("cutoff",
                                                        "Cutoff",
                                                        0.0f,
                                                        1.0f,
                                                        0.0f),
            std::make_unique<juce::AudioParameterFloat> ("resonance",
                                                        "Resonance",
                                                        0.0f,
                                                        1.0f,
                                                        0.92f),
            std::make_unique<juce::AudioParameterFloat> ("envmod",
                                                        "EnvMod",
                                                        0.0f,
                                                        1.0f,
                                                        0.0f),
            std::make_unique<juce::AudioParameterFloat> ("decay",
                                                        "Decay",
                                                        0.0f,
                                                        1.0f,
                                                        0.29f),
            std::make_unique<juce::AudioParameterFloat> ("accent",
                                                        "Accent",
                                                        0.0f,
                                                        1.0f,
                                                        0.78f),
            std::make_unique<juce::AudioParameterFloat> ("volume",
                                                        "Volume",
                                                        0.0f,
                                                        1.0f,
                                                        0.75f),
            // MODs parameters
            std::make_unique<juce::AudioParameterFloat> ("normalDecay",
                                                        "Normal Decay",
                                                        0.0f,
                                                        1.0f,
                                                        0.3f),
            std::make_unique<juce::AudioParameterFloat> ("accentDecay",
                                                        "Accent Decay",
                                                        0.0f,
                                                        1.0f,
                                                        0.03f),
            std::make_unique<juce::AudioParameterFloat> ("feedbackFilter",
                                                        "Filt. FeedBack",
                                                        0.0f,
                                                        1.0f,
                                                        0.63f),
            std::make_unique<juce::AudioParameterFloat> ("softAttack",
                                                        "Soft Attack",
                                                        0.0f,
                                                        1.0f,
                                                        0.26f),
            std::make_unique<juce::AudioParameterFloat> ("slideTime",
                                                        "Slide time",
                                                        0.0f,
                                                        1.0f,
                                                        0.33f),
            std::make_unique<juce::AudioParameterFloat> ("sqrDriver",
                                                        "Square Driver",
                                                        0.0f,
                                                        1.0f,
                                                        0.25f),
            std::make_unique<juce::AudioParameterBool> ("switchModState",
                                                        "Switch Mod",
                                                        false),
            // LFO parameters
            std::make_unique<juce::AudioParameterChoice> ("lfoWaveform",
                                                        "LFO Wave",
                                                        juce::StringArray{ "Triangle", "Saw Up", "Saw Down", "Square", "Random", "Pink Noise" },
                                                        0),
            std::make_unique<juce::AudioParameterFloat> ("lfoRate",
                                                        "LFO Rate",
                                                        juce::NormalisableRange<float> (0.0f, 1.0f),
                                                        0.25f,
                                                        juce::AudioParameterFloatAttributes()
                                                            .withStringFromValueFunction (
                                                                [] (float v, int)
                                                                {
                                                                    const double hz = linToExp (v, 0.0, 1.0, 0.01, 20.0);
                                                                    const int dp = (hz < 0.1) ? 4 : (hz < 1.0 ? 3 : 2);
                                                                    return juce::String (hz, dp) + " Hz";
                                                                })),
            std::make_unique<juce::AudioParameterFloat> ("lfoDepth",
                                                        "LFO Depth",
                                                        juce::NormalisableRange<float> (0.0f, 1.0f),
                                                        0.0f,
                                                        juce::AudioParameterFloatAttributes()
                                                            .withStringFromValueFunction (
                                                                [] (float v, int)
                                                                {
                                                                    if (v <= 0.001f) return juce::String ("off");
                                                                    const int pct = juce::roundToInt ((v * 2.0f - 1.0f) * 100.0f);
                                                                    if (pct == 0) return juce::String ("0");
                                                                    return (pct > 0 ? juce::String ("+")
                                                                                    : juce::String())
                                                                           + juce::String (pct) + "%";
                                                                })),
            std::make_unique<juce::AudioParameterChoice> ("lfoDestination",
                                                        "LFO Destination",
                                                        juce::StringArray{ "Cutoff", "Volume", "Pitch" },
                                                        0),
            // overdrive (MXR Distortion+)
            std::make_unique<juce::AudioParameterBool> ("switchOverdriveState",
                                                        "Overdrive",
                                                        false),
            std::make_unique<juce::AudioParameterFloat> ("overdriveLevel",
                                                        "Drive",
                                                        0.0f,
                                                        1.0f,
                                                        0.5f),
            std::make_unique<juce::AudioParameterFloat> ("overdriveOutput",
                                                        "Output",
                                                        0.0f,
                                                        1.0f,
                                                        0.5f),
            std::make_unique<juce::AudioParameterBool> ("overdriveBassBoost",
                                                        "Bass Boost",
                                                        false),
            std::make_unique<juce::AudioParameterFloat> ("overdriveDryWet",
                                                        "Dry/Wet",
                                                        0.0f,
                                                        1.0f,
                                                        0.35f),
            // filter model selection
            std::make_unique<juce::AudioParameterChoice> ("filterType",
                                                        "Filter Model",
                                                        juce::StringArray{ "TeeBee", "Diode Octave", "Diode" },
                                                        FILTER_TEEBEE),
            std::make_unique<juce::AudioParameterFloat> ("filterDrive",
                                                        "Filter Drive",
                                                        0.0f,
                                                        1.0f,
                                                        0.5f),   // ~4.5 dB into the diode saturator by default
            std::make_unique<juce::AudioParameterFloat> ("bassComp",
                                                        "Bass Comp",
                                                        0.0f,
                                                        1.0f,
                                                        0.1f),   // light passband/bass makeup by default
            // generative sequencer parameters
            std::make_unique<juce::AudioParameterFloat> ("seqGenerativeFill",
                                                    "Seq Generative Fill",
                                                    0.0f,
                                                    100.0f,
                                                    80.0f),
            std::make_unique<juce::AudioParameterFloat> ("seqGenerativeAccentProbability",
                                                    "Seq Generative Accent Prob",
                                                    0.0f,
                                                    100.0f,
                                                    50.0f),
            std::make_unique<juce::AudioParameterFloat> ("seqGenerativeSlideProbability",
                                                    "Seq Generative Slide Prob",
                                                    0.0f,
                                                    100.0f,
                                                    30.0f),
            std::make_unique<juce::AudioParameterFloat> ("seqGenerativeTieProbability",
                                                    "Seq Generative Tie Prob",
                                                    0.0f,
                                                    100.0f,
                                                    100.0f),
            std::make_unique<juce::AudioParameterInt> ("numberOfTones",
                                                    "Number Of Tones",
                                                    1,
                                                    12,
                                                    4),
            std::make_unique<juce::AudioParameterFloat> ("lowerNote",
                                                    "Lower Note",
                                                    juce::NormalisableRange<float> (0.0f, 127.0f),
                                                    20.0f,
                                                    juce::AudioParameterFloatAttributes()
                                                        .withStringFromValueFunction(
                                                            [] (float v, int)
                                                            {
                                                                return juce::MidiMessage::getMidiNoteName(
                                                                    juce::jlimit (0, 127, juce::roundToInt (v)),
                                                                    true, true, 3);
                                                            })),
            std::make_unique<juce::AudioParameterFloat> ("rangeNote",
                                                    "Range Note",
                                                    juce::NormalisableRange<float> (0.0f, 127.0f),
                                                    48.0f,
                                                    juce::AudioParameterFloatAttributes()
                                                        .withStringFromValueFunction(
                                                            [] (float v, int)
                                                            {
                                                                return juce::MidiMessage::getMidiNoteName(
                                                                    juce::jlimit (0, 127, juce::roundToInt (v)),
                                                                    true, true, 3);
                                                            })),
            //std::make_unique<juce::AudioParameterFloat> ("seqHarmonizer",
            //                                            "Seq Harmonizer",
            //                                            0.0f,
            //                                            13.0f,
            //                                            0.0f),
            std::make_unique<juce::AudioParameterChoice> ("seqSyncMode",
                                                        "Seq Sync Mode",
                                                        juce::StringArray{ "Internal", "Host" },
                                                        0,
                                                        juce::AudioParameterChoiceAttributes().withAutomatable (false)),
            std::make_unique<juce::AudioParameterChoice> ("seqStartMode",
                                                        "Seq Start Mode",
                                                        juce::StringArray{ "Transport", "Note Trigger" },
                                                        0,
                                                        juce::AudioParameterChoiceAttributes().withAutomatable (false)),
            std::make_unique<juce::AudioParameterInt> ("seqLength",
                                                        "Seq Length",
                                                        1,
                                                        64,
                                                        16),
            std::make_unique<juce::AudioParameterInt> ("seqTempo",
                                                        "Seq Tempo BPM",
                                                        20,
                                                        300,
                                                        120),
            std::make_unique<juce::AudioParameterBool> ("seqPlayState",
                                                        "Seq Play State",
                                                        false,
                                                        juce::AudioParameterBoolAttributes().withAutomatable (false)),
            std::make_unique<juce::AudioParameterBool> ("seqGenerate",
                                                        "Seq Generate",
                                                        false,
                                                        juce::AudioParameterBoolAttributes().withAutomatable (false)),
            std::make_unique<juce::AudioParameterBool> ("seqClear",
                                                        "Seq Clear",
                                                        false,
                                                        juce::AudioParameterBoolAttributes().withAutomatable (false)),
            std::make_unique<juce::AudioParameterBool> ("seqCapture",
                                                        "Capture",
                                                        false,
                                                        juce::AudioParameterBoolAttributes().withAutomatable (false)),
            std::make_unique<juce::AudioParameterBool> ("transpose",
                                                        "Transpose",
                                                        false,
                                                        juce::AudioParameterBoolAttributes().withAutomatable (false)),
            std::make_unique<juce::AudioParameterInt> ("rotation",
                                                        "Rotate",
                                                        -63,
                                                        63,
                                                        0,
                                                        juce::AudioParameterIntAttributes()
                                                            .withStringFromValueFunction (
                                                                [] (int v, int)
                                                                {
                                                                    return (v > 0 ? juce::String ("+")
                                                                                  : juce::String())
                                                                           + juce::String (v);
                                                                })),
            std::make_unique<juce::AudioParameterBool> ("octaveTranspose",
                                                        "Octave",
                                                        false,
                                                        juce::AudioParameterBoolAttributes().withAutomatable (false)),
            std::make_unique<juce::AudioParameterBool> ("bypass",
                                                        "Bypass",
                                                        false)
        })
{
    // assign a pointer to use it around for each parameter
    waveForm = parameters.getRawParameterValue("waveform");
    tuning = parameters.getRawParameterValue("tuning");
    cutoffFreq = parameters.getRawParameterValue("cutoff");
    resonance = parameters.getRawParameterValue("resonance");
    envelopMod = parameters.getRawParameterValue("envmod");
    decay = parameters.getRawParameterValue("decay");
    accent = parameters.getRawParameterValue("accent");
    volume = parameters.getRawParameterValue("volume");
    // MODs parameters
    switchModState = parameters.getRawParameterValue("switchModState");
    normalDecay = parameters.getRawParameterValue("normalDecay");
    accentDecay = parameters.getRawParameterValue("accentDecay");
    feedbackFilter = parameters.getRawParameterValue("feedbackFilter");
    softAttack = parameters.getRawParameterValue("softAttack");
    slideTime = parameters.getRawParameterValue("slideTime");
    sqrDriver = parameters.getRawParameterValue("sqrDriver");
    // LFO parameters
    lfoWaveform = parameters.getRawParameterValue("lfoWaveform");
    lfoRate = parameters.getRawParameterValue("lfoRate");
    lfoDepth = parameters.getRawParameterValue("lfoDepth");
    lfoDestination = parameters.getRawParameterValue("lfoDestination");
    // overdrive parameters
    switchOverdriveState = parameters.getRawParameterValue("switchOverdriveState");
    overdriveLevel = parameters.getRawParameterValue("overdriveLevel");
    overdriveOutput = parameters.getRawParameterValue("overdriveOutput");
    overdriveBassBoost = parameters.getRawParameterValue("overdriveBassBoost");
    overdriveDryWet = parameters.getRawParameterValue("overdriveDryWet");
    // filter model
    filterType = parameters.getRawParameterValue("filterType");
    filterDrive = parameters.getRawParameterValue("filterDrive");
    bassComp = parameters.getRawParameterValue("bassComp");
    // generative sequencer parameters
    seqGenerativeFill = parameters.getRawParameterValue("seqGenerativeFill");
    seqGenerativeAccentProbability = parameters.getRawParameterValue("seqGenerativeAccentProbability");
    seqGenerativeSlideProbability = parameters.getRawParameterValue("seqGenerativeSlideProbability");
    seqGenerativeTieProbability = parameters.getRawParameterValue("seqGenerativeTieProbability");
    numberOfTones = parameters.getRawParameterValue("numberOfTones");
    lowerNote = parameters.getRawParameterValue("lowerNote");
    rangeNote = parameters.getRawParameterValue("rangeNote");
    //seqHarmonizer = parameters.getRawParameterValue("seqHarmonizer");
    seqLength = parameters.getRawParameterValue("seqLength");
    seqSyncMode = parameters.getRawParameterValue("seqSyncMode");
    seqStartMode = parameters.getRawParameterValue("seqStartMode");
    seqTempo = parameters.getRawParameterValue("seqTempo");
    seqPlayState = parameters.getRawParameterValue("seqPlayState");
    seqGenerate = parameters.getRawParameterValue("seqGenerate");
    seqClear = parameters.getRawParameterValue("seqClear");
    seqCapture = parameters.getRawParameterValue("seqCapture");
    transpose = parameters.getRawParameterValue("transpose");
    rotation = parameters.getRawParameterValue("rotation");
    octaveTranspose = parameters.getRawParameterValue("octaveTranspose");
    bypass = parameters.getRawParameterValue("bypass");

    // force initial user values(some hosts migth not do it using value tree state)
    setParameter(WAVEFORM, *waveForm);
    setParameter(TUNING, *tuning);
    setParameter(CUTOFF, *cutoffFreq);
    setParameter(RESONANCE, *resonance);
    setParameter(ENVMOD, *envelopMod);
    setParameter(DECAY, *decay);
    setParameter(ACCENT, *accent);
    setParameter(VOLUME, *volume);
    setDevilMod(*switchModState);
    setParameter(NORMAL_DECAY, *normalDecay);
    setParameter(ACCENT_DECAY, *accentDecay);
    setParameter(FEEDBACK_HPF, *feedbackFilter);
    setParameter(SOFT_ATTACK, *softAttack);
    setParameter(SLIDE_TIME, *slideTime);
    setParameter(TANH_SHAPER_DRIVE, *sqrDriver);
    // LFO parameters
    setParameter(LFO_WAVEFORM, *lfoWaveform);
    setParameter(LFO_RATE, *lfoRate);
    setParameter(LFO_DEPTH, *lfoDepth);
    setParameter(LFO_DESTINATION, *lfoDestination);
    // overdrive parameters
    setParameter(OVERDRIVE_LEVEL, *overdriveLevel);
    setParameter(OVERDRIVE_OUTPUT, *overdriveOutput);
    setParameter(OVERDRIVE_BASS_BOOST, *overdriveBassBoost);
    setParameter(OVERDRIVE_DRY_WET, *overdriveDryWet);
    if (*switchModState)
    {
        open303Core.setFilterType(static_cast<FilterType>((int) *filterType));
        setParameter(FILTER_DRIVE, *filterDrive);
        setParameter(BASS_COMP, *bassComp);
    }

    // Add parameter listeners
    parameters.addParameterListener("waveform", this);
    parameters.addParameterListener("tuning", this);
    parameters.addParameterListener("cutoff", this);
    parameters.addParameterListener("resonance", this);
    parameters.addParameterListener("envmod", this);
    parameters.addParameterListener("decay", this);
    parameters.addParameterListener("accent", this);
    parameters.addParameterListener("volume", this);
    parameters.addParameterListener("normalDecay", this);
    parameters.addParameterListener("accentDecay", this);
    parameters.addParameterListener("feedbackFilter", this);
    parameters.addParameterListener("softAttack", this);
    parameters.addParameterListener("slideTime", this);
    parameters.addParameterListener("sqrDriver", this);
    parameters.addParameterListener("switchModState", this);
    // LFO parameter listeners
    parameters.addParameterListener("lfoWaveform", this);
    parameters.addParameterListener("lfoRate", this);
    parameters.addParameterListener("lfoDepth", this);
    parameters.addParameterListener("lfoDestination", this);
    // overdrive parameter listeners
    parameters.addParameterListener("overdriveLevel", this);
    parameters.addParameterListener("overdriveOutput", this);
    parameters.addParameterListener("overdriveBassBoost", this);
    parameters.addParameterListener("overdriveDryWet", this);
    parameters.addParameterListener("filterType", this);
    parameters.addParameterListener("filterDrive", this);
    parameters.addParameterListener("bassComp", this);
    // generative sequencer parameter listener
    parameters.addParameterListener("seqPlayState", this);
    parameters.addParameterListener("seqGenerate", this);
    parameters.addParameterListener("seqClear", this);
    parameters.addParameterListener("seqCapture", this);
    parameters.addParameterListener("transpose", this);
    parameters.addParameterListener("rotation", this);
    parameters.addParameterListener("octaveTranspose", this);
    parameters.addParameterListener("bypass", this);
    //parameters.addParameterListener("seqHarmonizer", this);
    parameters.addParameterListener("seqLength", this);
    parameters.addParameterListener("seqSyncMode", this);
    parameters.addParameterListener("seqStartMode", this);
    parameters.addParameterListener("seqTempo", this);

    // ── Sequencer callback ────────────────────────────────────────────────────
    // Runs on the audio thread. Stores each event into _pendingNotes[] so
    // processBlock can deliver it to Open303 at the correct sample offset
    // inside the existing render loop.
    _sequencer.onNoteEvent = [this] (const Acid303Event& ev)
    {
        if (_pendingCount < kPendingMax)
            _pendingNotes[_pendingCount++] = { ev.type, ev.note, ev.velocity, ev.sampleOffset };
    };

    // Reset held-note tracking whenever the sequencer silences its note stack
    // (stop, mute, clearTrack, acidRandomize) so no stale gate is left open.
    _sequencer.onAllNotesOff = [this]
    {
        _heldNote         = -1;
        _lastStepHadSlide = false;
    };

    // Sequence defaults are driven by the APVTS params (seqSyncMode/seqStartMode/seqTempo)
    // and the AcidSeq303 child (stepLength) on state restore.
    _sequencer.setTrackLength (16);
}

/// @brief Removes every APVTS parameter listener registered by the constructor.
JC303::~JC303()
{
    _exportQuit.store (true);
    if (_exportLive) { pthread_join (_exportTid, nullptr); _exportLive = false; }
    parameters.removeParameterListener("waveform", this);
    parameters.removeParameterListener("tuning", this);
    parameters.removeParameterListener("cutoff", this);
    parameters.removeParameterListener("resonance", this);
    parameters.removeParameterListener("envmod", this);
    parameters.removeParameterListener("decay", this);
    parameters.removeParameterListener("accent", this);
    parameters.removeParameterListener("volume", this);
    parameters.removeParameterListener("normalDecay", this);
    parameters.removeParameterListener("accentDecay", this);
    parameters.removeParameterListener("feedbackFilter", this);
    parameters.removeParameterListener("softAttack", this);
    parameters.removeParameterListener("slideTime", this);
    parameters.removeParameterListener("sqrDriver", this);
    parameters.removeParameterListener("switchModState", this);
    // LFO parameter listeners
    parameters.removeParameterListener("lfoWaveform", this);
    parameters.removeParameterListener("lfoRate", this);
    parameters.removeParameterListener("lfoDepth", this);
    parameters.removeParameterListener("lfoDestination", this);
    // overdrive parameter listeners
    parameters.removeParameterListener("overdriveLevel", this);
    parameters.removeParameterListener("overdriveOutput", this);
    parameters.removeParameterListener("overdriveBassBoost", this);
    parameters.removeParameterListener("overdriveDryWet", this);
    parameters.removeParameterListener("filterType", this);
    parameters.removeParameterListener("filterDrive", this);
    parameters.removeParameterListener("bassComp", this);
    // generative sequencer
    parameters.removeParameterListener("seqPlayState", this);
    parameters.removeParameterListener("seqGenerate", this);
    parameters.removeParameterListener("seqClear", this);
    parameters.removeParameterListener("seqCapture", this);
    //parameters.removeParameterListener("seqHarmonizer", this);
    parameters.removeParameterListener("seqLength", this);
    parameters.removeParameterListener("seqSyncMode", this);
    parameters.removeParameterListener("seqStartMode", this);
    parameters.removeParameterListener("seqTempo", this);
    parameters.removeParameterListener("transpose", this);
    parameters.removeParameterListener("octaveTranspose", this);
    parameters.removeParameterListener("rotation", this);
    parameters.removeParameterListener("bypass", this);
}

/// @brief APVTS listener: maps a changed parameter onto the engine/sequencer.
///
/// Runs on whichever thread changed the parameter (message or audio); engine
/// updates go through setParameter(), sequencer sync/tempo through the
/// sequencer's atomic setters.
void JC303::parameterChanged(const juce::String& parameterID, float newValue)
{
    // Map parameter ID to enum and update immediately or set flag
    if (parameterID == "waveform") {
        setParameter(WAVEFORM, newValue);
    }
    else if (parameterID == "tuning") {
        setParameter(TUNING, newValue);
    }
    else if (parameterID == "cutoff") {
        setParameter(CUTOFF, newValue);
    }
    else if (parameterID == "resonance") {
        setParameter(RESONANCE, newValue);
    }
    else if (parameterID == "envmod") {
        setParameter(ENVMOD, newValue);
    }
    else if (parameterID == "decay") {
        setParameter(DECAY, newValue);
    }
    else if (parameterID == "accent") {
        setParameter(ACCENT, newValue);
    }
    else if (parameterID == "volume") {
        setParameter(VOLUME, newValue);
    }
    else if (parameterID == "switchModState") {
        setDevilMod(newValue > 0.5f);
    }
    else if (parameterID == "normalDecay" && *switchModState) {
        setParameter(NORMAL_DECAY, newValue);
    }
    else if (parameterID == "accentDecay" && *switchModState) {
        setParameter(ACCENT_DECAY, newValue);
    }
    else if (parameterID == "feedbackFilter" && *switchModState) {
        setParameter(FEEDBACK_HPF, newValue);
    }
    else if (parameterID == "softAttack" && *switchModState) {
        setParameter(SOFT_ATTACK, newValue);
    }
    else if (parameterID == "slideTime" && *switchModState) {
        setParameter(SLIDE_TIME, newValue);
    }
    else if (parameterID == "sqrDriver" && *switchModState) {
        setParameter(TANH_SHAPER_DRIVE, newValue);
    }
    // LFO parameters
    else if (parameterID == "lfoWaveform") {
        setParameter(LFO_WAVEFORM, newValue);
    }
    else if (parameterID == "lfoRate") {
        setParameter(LFO_RATE, newValue);
    }
    else if (parameterID == "lfoDepth") {
        setParameter(LFO_DEPTH, newValue);
    }
    else if (parameterID == "lfoDestination") {
        setParameter(LFO_DESTINATION, newValue);
    }
    // overdrive parameter
    else if (parameterID == "overdriveLevel") {
        setParameter(OVERDRIVE_LEVEL, newValue);
    }
    else if (parameterID == "overdriveOutput") {
        setParameter(OVERDRIVE_OUTPUT, newValue);
    }
    else if (parameterID == "overdriveBassBoost") {
        setParameter(OVERDRIVE_BASS_BOOST, newValue);
    }
    else if (parameterID == "overdriveDryWet") {
        setParameter(OVERDRIVE_DRY_WET, newValue);
    }
    else if (parameterID == "filterType" && *switchModState) {
        open303Core.setFilterType(static_cast<FilterType>((int) newValue));
    }
    else if (parameterID == "filterDrive" && *switchModState) {
        setParameter(FILTER_DRIVE, newValue);
    }
    else if (parameterID == "bassComp" && *switchModState) {
        setParameter(BASS_COMP, newValue);
    }
    else if (parameterID == "seqPlayState") {
        // Host mode + host transport: the transport owns play/stop, so ignore
        // user edits here (seqPlayState is mirrored from the transport).
        if (_sequencer.getSyncMode() == AcidSequencer303::SyncMode::Host
            && _hostTransportSeen)
            return;
        _seqCommand.store (newValue > 0.5f ? static_cast<int>(SeqCommand::Play)
                                           : static_cast<int>(SeqCommand::Stop),
                           std::memory_order_release);
    }
    else if (parameterID == "seqGenerate") {
        if (newValue > 0.5f) {
            _seqCommand.store (static_cast<int>(SeqCommand::Generate), std::memory_order_release);
            if (mm303ShimBlinkStatus) mm303ShimBlinkStatus();
        }
    }
    else if (parameterID == "seqClear") {
        if (newValue > 0.5f) {
            _seqCommand.store (static_cast<int>(SeqCommand::Clear), std::memory_order_release);
            if (mm303ShimBlinkStatus) mm303ShimBlinkStatus();
        }
    }
    else if (parameterID == "seqCapture") {
        if (newValue > 0.5f) {
            _exportRequested.store (true);   // file write happens off the audio thread
            if (mm303ShimBlinkStatus) mm303ShimBlinkStatus();
        }
    }
    else if (parameterID == "transpose") {
        // Off returns to untransposed; on re-derives the last note's shift.
        if (newValue <= 0.5f)
            _sequencer.setTranspose(0);
        else if (_lastTransposeNote >= 0)
            _sequencer.setTranspose((int8_t) transposeShiftFor(_lastTransposeNote));
    }
    else if (parameterID == "octaveTranspose") {
        // Re-derive so turning Octave off drops the octave term immediately.
        if (transpose != nullptr && transpose->load() > 0.5f
            && _lastTransposeNote >= 0)
            _sequencer.setTranspose((int8_t) transposeShiftFor(_lastTransposeNote));
    }
    else if (parameterID == "rotation") {
        // Queued; the sequencer latches it at the next cycle start.
        _sequencer.setRotation (juce::roundToInt (newValue));
    }
    //else if (parameterID == "seqHarmonizer") {
    //    uint8_t seqHarmony = static_cast<uint8_t>(*seqHarmonizer);
    //    if (seqHarmony == 0) {
    //        _sequencer.setTune(0);
    //    } else {
    //        _sequencer.setTemperament(seqHarmony - 1);
    //        _sequencer.setTune(1);
    //    }
    //}
    else if (parameterID == "seqLength") {
        if (auto* p = dynamic_cast<juce::AudioParameterInt*>(parameters.getParameter("seqLength")))
            _sequencer.setTrackLength(static_cast<uint8_t>(p->get()));
    }
    else if (parameterID == "seqSyncMode") {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter("seqSyncMode")))
            _sequencer.setSyncMode((AcidSequencer303::SyncMode) p->getIndex());
    }
    else if (parameterID == "seqStartMode") {
        if (auto* p = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter("seqStartMode")))
            _sequencer.setStartMode((AcidSequencer303::StartMode) p->getIndex());
    }
    else if (parameterID == "seqTempo") {
        if (auto* p = dynamic_cast<juce::AudioParameterInt*>(parameters.getParameter("seqTempo")))
            _sequencer.setTempo((float) p->get());
    }
}

/// @brief Consumes a sequencer command queued by the UI thread.
///
/// Must run on the audio thread, after _pendingCount has been reset, so all
/// Open303 calls and sequencer emits stay on the audio thread while the device
/// is rendering.
void JC303::applySeqCommands()
{
    const int cmd = _seqCommand.exchange (static_cast<int>(SeqCommand::None),
                                          std::memory_order_acq_rel);
    if (cmd == static_cast<int>(SeqCommand::None))
        return;

    switch (static_cast<SeqCommand>(cmd))
    {
    case SeqCommand::Play:
        _heldNote         = -1;
        _lastStepHadSlide = false;
        // Note Trigger mode must wait for an incoming note; the PLAY control
        // only arms it (do not start here).
        if (_sequencer.getStartMode() == AcidSequencer303::StartMode::TransportStart)
            _sequencer.start();
        break;

    case SeqCommand::Stop:
        _heldNote         = -1;
        _lastStepHadSlide = false;
        _sequencer.stop();
        break;

    case SeqCommand::Clear:
        _sequencer.clearTrack();
        break;

    case SeqCommand::Generate:
        _sequencerMuted.store (true, std::memory_order_release);

        for (int i = 0; i <= 127; i++)
            open303Core.noteOn (i, 0, 0);
        _heldNote         = -1;
        _lastStepHadSlide = false;

        _sequencer.acidRandomize(
            static_cast<uint8_t>(*seqGenerativeFill),
            static_cast<uint8_t>(*seqGenerativeAccentProbability),
            static_cast<uint8_t>(*seqGenerativeSlideProbability),
            static_cast<uint8_t>(*seqGenerativeTieProbability),
            static_cast<uint8_t>(*numberOfTones),
            static_cast<uint8_t>(*lowerNote),
            static_cast<uint8_t>(*rangeNote)
        );

        _sequencerMuted.store (false, std::memory_order_release);
        break;

    case SeqCommand::None:
    default:
        break;
    }
}

/// @brief Sequence transpose shift for an incoming note (see seqTranspose.h).
int JC303::transposeShiftFor (int note) const
{
    const bool oct = octaveTranspose != nullptr && octaveTranspose->load() > 0.5f;
    return mm303TransposeShift (note,
                                juce::roundToInt (lowerNote->load()),
                                juce::roundToInt (rangeNote->load()),
                                _baseOctave * 12,
                                oct ? _octaveMax : 0);
}

/// @brief Applies one engine parameter (see Open303Parameters) by index.
void JC303::setParameter (Open303Parameters index, float value)
{
  if( index < 0 || index >= OPEN303_NUM_PARAMETERS )
    return;

	switch(index)
	{
    case WAVEFORM:
        open303Core.setWaveform(
            linToLin(value, 0.0, 1.0,   0.0,      1.0)
        );
        break;
    case TUNING:
        // Master tune matching the TB-303 range: +/-700 cents (+/-7 semitones,
        // a perfect fifth) around A4 = 440 Hz at the centre => 440*2^-7/12 ..
        // 440*2^+7/12 (~293.66 .. 659.26 Hz).
        open303Core.setTuning(
            linToExp(value, 0.0, 1.0,  293.66,   659.26)
        );
        break;
    case CUTOFF:
        open303Core.setCutoff(
            linToExp(value, 0.0, 1.0, 314.0,    2394.0)
        );
        break;
    case RESONANCE:
        open303Core.setResonance(
            linToLin(value, 0.0, 1.0,   0.0,    100.0)
        );
        break;
    case ENVMOD:
        open303Core.setEnvMod(
            linToLin(value, 0.0, 1.0,    0.0,   100.0)
        );
        break;
    case DECAY:
        open303Core.setDecay(
            linToExp(value, 0.0, 1.0,  decayMin,  decayMax)
        );
        break;
    case ACCENT:
        open303Core.setAccent(
            linToLin(value, 0.0, 1.0,   0.0,    100.0)
        );
        break;
    case VOLUME:
        open303Core.setVolume(
            linToLin(value, 0.0, 1.0, -60.0,      0.0)
        );
        break;

    // Overdrive - MXR Distortion+ (vendored GxDistortionPlus DSP)
    case OVERDRIVE_LEVEL:
        overdrive.setDistortion(value);
        break;
    case OVERDRIVE_OUTPUT:
        overdrive.setOutput(value);
        break;
    case OVERDRIVE_BASS_BOOST:
        overdrive.setBassBoost(value > 0.5f);
        break;
    case OVERDRIVE_DRY_WET:
        overdriveMix.setWetMixProportion(value);
        break;

    //
    // MODS (mostly based on devilfish mod)
    // BUT DONT! dont expect a devilfish clone sound or mail me about!
    // https://www.firstpr.com.au/rwi/dfish/Devil-Fish-Manual.pdf
    //
    case NORMAL_DECAY:
        open303Core.setAmpDecay(
            linToLin(value, 0.0, 1.0, 30.0,      3000.0)
        );
        break;
    case ACCENT_DECAY:
        open303Core.setAccentDecay(
            linToLin(value, 0.0, 1.0, 30.0,      3000.0)
        );
        break;
    case FEEDBACK_HPF:
        open303Core.setFeedbackHighpass(
            linToExp(value, 0.0, 1.0,  350.0,    100.0)
        );
        break;
    case SOFT_ATTACK:
        open303Core.setNormalAttack(
            linToExp(value, 0.0, 1.0,  0.3,    3000.0)
        );
        break;
    case SLIDE_TIME:
        open303Core.setSlideTime(
            linToLin(value, 0.0, 1.0, 2.0, 360.0)
        );
        break;
    case TANH_SHAPER_DRIVE:
        open303Core.setTanhShaperDrive(
            linToLin(value, 0.0, 1.0,   25.0,     80.0)
        );
        break;
    case FILTER_DRIVE:
        // 0..1 -> 0..9 dB into the diode ladder's saturating input stage
        // (matches the DB303 plugin's tuned range; the unity-makeup shaper
        // here has no headroom scaling, so it already runs hot per-dB)
        open303Core.setFilterDrive(
            linToLin(value, 0.0, 1.0,   0.0,     9.0)
        );
        break;
    case BASS_COMP:
        // 0..1 diode-ladder passband (bass) compensation, applied directly
        open303Core.setPassbandCompensation(value);
        break;

    // LFO parameters
    case LFO_WAVEFORM:
        open303Core.setLfoWaveform((int) value);
        break;
    case LFO_RATE:
        open303Core.setLfoRate(
            linToExp(value, 0.0, 1.0, 0.01, 20.0)
        );
        break;
    case LFO_DEPTH:
        open303Core.setLfoDepth(value);
        break;
    case LFO_DESTINATION:
        open303Core.setLfoDestination((int) value);
        break;
	}
}

/// @brief Toggles/restores the original 303 vs Devil-Fish mod parameter set.
void JC303::setDevilMod(bool mode)
{
    if (mode == true) {
        decayMin = 30.0;
        decayMax = 3000.0;
        setParameter(NORMAL_DECAY, *normalDecay);
        setParameter(ACCENT_DECAY, *accentDecay);
        setParameter(FEEDBACK_HPF, *feedbackFilter);
        setParameter(SOFT_ATTACK, *softAttack);
        setParameter(SLIDE_TIME, *slideTime);
        setParameter(TANH_SHAPER_DRIVE, *sqrDriver);
        open303Core.setFilterType(static_cast<FilterType>((int) *filterType));
        setParameter(FILTER_DRIVE, *filterDrive);
        setParameter(BASS_COMP, *bassComp);
        open303Core.setLfoOn(true);
    } else if (mode == false) {
        open303Core.setFilterType(FILTER_TEEBEE);
        open303Core.setLfoOn(false);
        decayMin = 200.0;
        decayMax = 2000.0;
        open303Core.setAmpDecay(1230.0);
        open303Core.setAccentDecay(200.0);
        open303Core.setFeedbackHighpass(150.0);
        open303Core.setNormalAttack(3.0);
        open303Core.setSlideTime(60.0);
        open303Core.setTanhShaperDrive(36.9);
    }
}

//==============================================================================
const juce::String JC303::getName() const
{
    return JucePlugin_Name;
}

bool JC303::acceptsMidi() const
{
   #if JucePlugin_WantsMidiInput
    return true;
   #else
    return false;
   #endif
}

bool JC303::producesMidi() const
{
    // The sequencer always outputs MIDI so the DAW can route it to other instruments.
    return true;
}

bool JC303::isMidiEffect() const
{
   #if JucePlugin_IsMidiEffect
    return true;
   #else
    return false;
   #endif
}

double JC303::getTailLengthSeconds() const
{
    return 0.0;
}

int JC303::getNumPrograms()
{
    return 1;
}

int JC303::getCurrentProgram()
{
    return 0;
}

void JC303::setCurrentProgram (int index)
{
    juce::ignoreUnused (index);
}

const juce::String JC303::getProgramName (int index)
{
    juce::ignoreUnused (index);
    return {};
}

void JC303::changeProgramName (int index, const juce::String& newName)
{
    juce::ignoreUnused (index, newName);
}

//==============================================================================
/// @brief Prepares the engine, the overdrive and the sequencer.
///
/// Also the first safe point to locate MM-303.conf (the shim has resolved the
/// plugin dir by now), so the MIDI CC map / capture path are loaded here.
void JC303::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    _sr = sampleRate;
    _bypassGain.reset (sampleRate, MM303_BYPASS_FADE_OUT);

    // Off-audio-thread worker for the Capture button's .mid file write
    // (started here, not in the constructor: the host constructs the processor
    // during plugin scan, where spawning a thread is unsafe).
    if (! _exportLive)
        _exportLive = (pthread_create (&_exportTid, nullptr, &JC303::exportRun, this) == 0);

    // init open303
    open303Core.setSampleRate(sampleRate);
    // init MXR Distortion+ overdrive
    overdrive.prepare(sampleRate, samplesPerBlock);
    // init overdrive dry/wet processor
    overdriveMix.prepare ({ sampleRate, (uint32_t) samplesPerBlock, 2 });
    overdriveMix.setMixingRule (juce::dsp::DryWetMixingRule::sin3dB);

    // ── Sequencer ─────────────────────────────────────────────────────────────
    // 960 PPQN is the JUCE default and is compatible with all major DAWs.
    // The sequencer adapts all pulse maths to this resolution.
    _sequencer.prepare (sampleRate, 960.0);
    _pendingCount     = 0;
    _wasHostPlaying   = false;
    _hostTransportSeen = false;
    _haveLastPpq      = false;
    _lastHostPpq      = 0.0;
    _heldNote         = -1;
    _lastStepHadSlide = false;
    _recHeldNote      = -1;
    _sustainArmed     = true;
    _recWasOn         = false;

    // Config (capture path + MIDI CC map). The shim's plugin dir is resolved by
    // now, so this is the first safe point to locate MM-303.conf.
    loadMidiConfig();
    _sequencer.setClampLoopEnd (_clampLoopEnd);   // config "lastNote"
}

void JC303::releaseResources()
{
    // Stop sequencer and flush any held notes when playback stops
    _sequencer.stop();
}

bool JC303::isBusesLayoutSupported (const BusesLayout& layouts) const
{
  #if JucePlugin_IsMidiEffect
    juce::ignoreUnused (layouts);
    return true;
  #else
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
     && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

   #if ! JucePlugin_IsSynth
    if (layouts.getMainOutputChannelSet() != layouts.getMainInputChannelSet())
        return false;
   #endif

    return true;
  #endif
}

/// @brief Renders the 303 engine into samples [beginSample, endSample).
void JC303::render303(juce::AudioBuffer<float>& buffer, int beginSample, int endSample)
{
    if (_bypassDsp) return;   // Bypass (MIDI-only): no audio DSP
    auto* monoChannel = buffer.getWritePointer(0);
    for (auto sample = beginSample; sample < endSample; ++sample)
        monoChannel[sample] = (float) open303Core.getSample();
}

/// @brief Clocks the sequencer and renders it sample-accurately into the buffer.
///
/// Applies queued sequence commands, ticks the sequencer, dispatches its note
/// events (and incoming host MIDI) into Open303, and externally forwards the
/// sequencer's MIDI to the host. Runs on the audio thread.
void JC303::renderMidi (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    const int numSamples = buffer.getNumSamples();
    int currentSample = 0;

    // Reset the pending-event queue FIRST: the transport stop edge (below) and
    // applySeqCommands() both emit NoteOff/AllNotesOff events that must survive
    // into this buffer, otherwise gates hang when the sequencer stops.
    _pendingCount = 0;

    // ── Gather host transport info ────────────────────────────────────────────
    double ppqAtStart    = 0.0;
    double hostBpm       = static_cast<double> (_sequencer.getTempo());
    bool   hostIsPlaying = false;
    bool   hostHasPpq    = false;

    if (auto* ph = getPlayHead())
    {
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm())         hostBpm      = *bpm;
            if (auto ppq = pos->getPpqPosition()) { ppqAtStart = *ppq; hostHasPpq = true; }
            hostIsPlaying = pos->getIsPlaying();
        }
    }

    // Hardware hosts (Akai Force / MPC) expose VST2 tempo but do not report
    // transport or ppq.  Latch once a real transport is seen so a DAW still
    // stops when its transport stops, while a tempo-only host falls back to a
    // tempo-locked free-run controlled by PLAY / Note-Trigger.
    const bool ppqAdvancing = _haveLastPpq && hostHasPpq && ppqAtStart != _lastHostPpq;
    _haveLastPpq = hostHasPpq;
    _lastHostPpq = ppqAtStart;
    if (hostIsPlaying || ppqAdvancing)
        _hostTransportSeen = true;

    const bool hostHasTransport = _hostTransportSeen;
    const bool hostRunning      = hostHasTransport && (hostIsPlaying || ppqAdvancing);

    // ── Detect host play start / stop edge (Host sync mode) ──────────────────
    // Start depends on Start Mode: TransportStart begins on host play, while
    // Note Trigger waits for an incoming note (it must NOT start on play).
    // Stop is always host-driven: transport stop stops the sequencer either way.
    if (_sequencer.getSyncMode() == AcidSequencer303::SyncMode::Host)
    {
        if (hostRunning && ! _wasHostPlaying)
        {
            if (_sequencer.getStartMode() == AcidSequencer303::StartMode::TransportStart)
                _sequencer.start();
        }
        else if (! hostRunning && _wasHostPlaying)
        {
            _sequencer.stop();
            _heldNote         = -1;
            _lastStepHadSlide = false;
        }
    }
    _wasHostPlaying = hostRunning;

    // In Host mode the transport owns play/stop: mirror it into seqPlayState so
    // the Force shows the control as on (playing) / off (stopped).  Manual
    // changes are ignored there (see parameterChanged).
    if (_sequencer.getSyncMode() == AcidSequencer303::SyncMode::Host && hostHasTransport)
    {
        if (auto* p = parameters.getParameter ("seqPlayState"))
        {
            const float want = hostRunning ? 1.0f : 0.0f;
            if (std::abs (p->getValue() - want) > 0.5f)
                p->setValueNotifyingHost (want);
        }
    }

    // ── Tick sequencer — populates _pendingNotes[] via the callback ───────────
    // Apply any sequencer command queued from the UI thread (play/stop/
    // generate/clear).  Running here — after the pending queue was reset and
    // before the clock tick — keeps every note event the sequencer emits
    // (including the note-alls-off flush on generate/clear) on the audio
    // thread and inside this buffer's event queue.
    applySeqCommands();

    _sequencer.processBlock (midiMessages,
                             numSamples,
                             hostRunning,
                             hostHasTransport,
                             hostHasPpq,
                             ppqAtStart,
                             hostBpm,
                             /*shufflePulses=*/ 0);

    // ── Sort pending sequencer events by sample position ──────────────────────
    // Typically 0–2 events per buffer; insertion sort is ideal here.
    for (int i = 1; i < _pendingCount; ++i)
    {
        PendingNote key = _pendingNotes[i];
        int j = i - 1;
        while (j >= 0 && _pendingNotes[j].sampleOffset > key.sampleOffset)
        {
            _pendingNotes[j + 1] = _pendingNotes[j];
            --j;
        }
        _pendingNotes[j + 1] = key;
    }

    // ── dispatchSeqNote ───────────────────────────────────────────────────────
    //
    // Bridges sequencer note events to Open303.
    //
    // TIE (rest+tie steps):
    //   The sequencer never emits a NoteOn for a tied rest — it just extends
    //   the gate countdown of the previous note.  Ties are fully transparent
    //   here; no special handling is needed.
    //
    // SLIDE:
    //   The slide flag lives on the step that *sends* the glide, not the one
    //   that receives it.  We save it in _lastStepHadSlide after each NoteOn
    //   so the next NoteOn can pass slide=1 to Open303, which performs the
    //   portamento without retriggering the envelope.
    //
    // WRAP-AROUND TIE (same pitch re-firing at pattern boundary):
    //   If the last step(s) of a pattern are rest+tie, the sequencer extends
    //   the gate past the pattern end.  When step 0 then fires with the same
    //   pitch still ringing, we skip the re-trigger so there is no click.
    //   _lastStepHadSlide is preserved so the glide carry still works.
    //
    // NoteOff:
    //   Always forwarded unconditionally.  This is the key fix — suppressing
    //   NoteOffs to handle slide caused stuck notes because it prevented the
    //   gate from closing.  Open303 handles note-stealing internally; we just
    //   need to close every gate we opened.
    //
    // MIDI output buffer — built up during rendering, replaces midiMessages
    // at the end so the DAW receives sample-accurate note events on the
    // plugin's MIDI output port.
    juce::MidiBuffer midiOut;

    auto dispatchSeqNote = [&] (const PendingNote& ev)
    {
        const int samplePos = juce::jlimit (0, numSamples - 1, ev.sampleOffset);

        if (ev.type == Acid303EventType::NoteOn)
        {
            if (_sequencerMuted.load(std::memory_order_acquire))
                return;

            if (_heldNote >= 0 && ev.note == static_cast<uint8_t>(_heldNote))
            {
                // Wrap-around tie: same pitch already ringing from a gate
                // extension across the pattern boundary — do not re-trigger.
                // Keep _lastStepHadSlide as-is so slide carry is unaffected.
            }
            else
            {
                // Normal new note, or slide into a different pitch.
                // _lastStepHadSlide was set by the previous NoteOn dispatch.
                const int slide = _lastStepHadSlide ? 1 : 0;
                open303Core.noteOn (ev.note, ev.velocity, slide);
                _heldNote = static_cast<int>(ev.note);

                // MIDI out: NoteOn
                midiOut.addEvent (juce::MidiMessage::noteOn  (1, ev.note, static_cast<uint8_t>(ev.velocity)),
                                  samplePos);
            }

            // Save the slide flag of the step we just played so the *next*
            // NoteOn dispatch knows whether to use slide=1.
            _lastStepHadSlide = _sequencer.slideOn (_sequencer.getCurrentStep());
        }
        else // NoteOff — always forward, never drop
        {
            open303Core.noteOn (ev.note, 0, 0);

            // MIDI out: NoteOff
            midiOut.addEvent (juce::MidiMessage::noteOff (1, ev.note),
                              samplePos);

            if (ev.note == static_cast<uint8_t>(_heldNote))
            {
                _heldNote         = -1;
                _lastStepHadSlide = false;
            }
        }
    };

    // ── Interleaved render: sequencer events + external MIDI ──────────────────
    // Walk sequencer events and incoming MIDI together in time order.
    // Render an audio sub-block before each event, then dispatch to Open303.
    // External MIDI note messages are forwarded only when the sequencer is
    // not running, so keyboard play still works when the sequencer is off.

    int seqIdx = 0;

    for (const auto midiMetadata : midiMessages)
    {
        const auto message        = midiMetadata.getMessage();
        const auto samplePosition = midiMetadata.samplePosition;

        if (samplePosition < currentSample || samplePosition >= numSamples)
            continue;

        // MIDI CC -> parameter mapping (MM-303.conf).  Any channel.
        if (message.isController() && ! _ccMap.empty())
        {
            const auto it = _ccMap.find (message.getControllerNumber());
            if (it != _ccMap.end() && it->second != nullptr)
            {
                auto* p   = it->second;
                const int ccv = message.getControllerValue();
                // Switch (boolean) params: any value >= 5 is ON; only < 5 turns
                // them OFF (guards against controller jitter around 0).
                if (dynamic_cast<juce::AudioParameterBool*> (p) != nullptr)
                    p->setValueNotifyingHost (ccv < 5 ? 0.0f : 1.0f);
                else
                    p->setValueNotifyingHost (ccv / 127.0f);
            }
        }

        // Flush sequencer events that fall before this MIDI message
        while (seqIdx < _pendingCount
               && _pendingNotes[seqIdx].sampleOffset <= samplePosition)
        {
            const PendingNote& ev = _pendingNotes[seqIdx];
            const int evPos = juce::jlimit (0, numSamples - 1, ev.sampleOffset);

            if (evPos > currentSample)
            {
                render303 (buffer, currentSample, evPos);
                currentSample = evPos;
            }

            dispatchSeqNote (ev);
            ++seqIdx;
        }

        // Render audio up to this external MIDI message
        if (samplePosition > currentSample)
        {
            render303 (buffer, currentSample, samplePosition);
            currentSample = samplePosition;
        }

        // ── Rec mode (works whether or not the sequencer is playing) ─────────
        // Reset per-press state each time rec is (re)enabled so a stale held
        // note or sustain latch from a previous session can't leak in.
        const bool recOnNow = _sequencer.isRecOn();
        if (recOnNow && ! _recWasOn)
        {
            _recHeldNote   = -1;
            _sustainArmed  = true;
        }
        _recWasOn = recOnNow;

        if (recOnNow)
        {
            if (message.isNoteOn())
            {
                // A second note struck while the previous one is still held
                // records with slide (legato) — the record step is set below
                // _recHeldNote, i.e. the newly pressed note carries the slide.
                const bool legato = (_recHeldNote >= 0
                                     && _recHeldNote != message.getNoteNumber());
                _sequencer.recNote (static_cast<uint8_t> (message.getNoteNumber()),
                                    message.getVelocity() >= SEQ303_ACCENT_VELOCITY_THRESHOLD,
                                    legato);
                _recHeldNote = message.getNoteNumber();
            }
            else if (message.isNoteOff() && message.getNoteNumber() == _recHeldNote)
            {
                _recHeldNote = -1;
            }
            else if (message.isController() && message.getControllerNumber() == 64)
            {
                // Sustain pedal: a 127 tap records one rest, then waits for a
                // 0 to re-arm before the next 127 can record another rest.
                if (message.getControllerValue() == 127 && _sustainArmed)
                {
                    _sequencer.recRest();
                    _sustainArmed = false;
                }
                else if (message.getControllerValue() == 0)
                {
                    _sustainArmed = true;
                }
            }
        }

        // External MIDI note handling.
        if (! _sequencer.isRunning())
        {
            // Sequencer stopped: play the synth directly (unchanged behaviour).
            if (message.isNoteOn())
            {
                open303Core.noteOn (message.getNoteNumber(), message.getVelocity(), 0);
            }
            else if (message.isNoteOff())
            {
                open303Core.noteOn (message.getNoteNumber(), 0, 0);
            }
            else if (message.isAllNotesOff())
            {
                for (int i = 0; i <= 127; i++)
                    open303Core.noteOn (i, 0, 0);
            }
        }
        else if (transpose != nullptr && transpose->load() > 0.5f)
        {
            // Sequencer running + Transpose on: the note sets the key and
            // is consumed — no synth voice, no MIDI out.  The note stack keeps
            // each sounding note's emitted pitch, so its NoteOff still matches
            // (no hanging notes when the key changes mid-note).
            if (message.isNoteOn())
            {
                _lastTransposeNote = message.getNoteNumber();
                _sequencer.setTranspose ((int8_t) transposeShiftFor (_lastTransposeNote));
            }
        }
    }

    // Flush any remaining sequencer events after the last MIDI message
    while (seqIdx < _pendingCount)
    {
        const PendingNote& ev = _pendingNotes[seqIdx];
        const int evPos = juce::jlimit (0, numSamples - 1, ev.sampleOffset);

        if (evPos > currentSample)
        {
            render303 (buffer, currentSample, evPos);
            currentSample = evPos;
        }

        dispatchSeqNote (ev);
        ++seqIdx;
    }

    // Render remaining samples
    render303 (buffer, currentSample, numSamples);

    // Replace the incoming MIDI buffer with the sequencer's output so the
    // DAW receives sample-accurate NoteOn/NoteOff on the plugin's MIDI output.
    // Incoming transport/clock messages are intentionally not forwarded.
    midiMessages.swapWith (midiOut);
}

/// @brief Real-time audio callback: sequencer + engine, then the overdrive.
///
/// Clears unused output channels, runs renderMidi() (which renders the synth),
/// applies the MXR Distortion+ when enabled, and copies mono to all channels.
void JC303::processBlock (juce::AudioBuffer<float>& buffer,
                          juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    const auto numSamples = buffer.getNumSamples();

    // clear buffer
    auto totalNumInputChannels  = getTotalNumInputChannels();
    auto totalNumOutputChannels = getTotalNumOutputChannels();
    for (auto i = totalNumInputChannels; i < totalNumOutputChannels; ++i)
        buffer.clear (i, 0, numSamples);

    // Bypass (MIDI-only): fast fade OUT, then stop the synth/overdrive DSP
    // (real CPU drop). On un-bypass the DSP restarts and the output fades IN
    // slowly (~100 ms) so the resumed engine's transition is masked — no click,
    // and the voice is preserved (it is not reset). A state load reuses the
    // same fade to mask its abrupt parameter/pattern change.
    const bool bypassed = (bypass != nullptr && bypass->load() > 0.5f);
    const bool muted = bypassed || _stateMuteRequest.load();
    if (muted != _mutedLast) {
        _bypassGain.reset (_sr, muted ? MM303_BYPASS_FADE_OUT : MM303_BYPASS_FADE_IN);
        _bypassGain.setTargetValue (muted ? 0.0f : 1.0f);
        _mutedLast = muted;
        _bypassDsp = false;   // run the DSP during the fade-in
    }
    if (bypassed && ! _bypassGain.isSmoothing() && _bypassGain.getCurrentValue() <= 0.0f)
        _bypassDsp = true;    // stop the DSP only once fully faded out
    if (! _bypassGain.isSmoothing())
        _stateMuteReady.store (muted && _bypassGain.getCurrentValue() <= 0.0f);

    // sequencer tick + MIDI handling + sample-accurate audio render
    renderMidi (buffer, midiMessages);

    if (! _bypassDsp) {
        // MXR Distortion+ overdrive
        if (*switchOverdriveState) {
            overdriveMix.pushDrySamples(buffer);
            overdrive.process (buffer.getArrayOfWritePointers(),
                               buffer.getNumChannels(), numSamples);
            overdriveMix.mixWetSamples(buffer);
        }

        // copy mono channel to stereo
        for (int ch = 1; ch < buffer.getNumChannels(); ++ch)
            buffer.copyFrom(ch, 0, buffer, 0, 0, numSamples);
    }

    // bypass / state-load fade (silent when full)
    _bypassGain.applyGain (buffer, numSamples);
    if (_bypassDsp)
        buffer.clear();   // fully bypassed: silent (MIDI-only)

    // ── Clip indicator ────────────────────────────────────────────────────────
    // Light the shim's Clip LED when the final output exceeds 0 dBFS (|x| >= 1).
    // Rising-edge only (the shim holds it ~150 ms), so we don't poke it each block.
    bool clipped = false;
    for (int ch = 0; ch < buffer.getNumChannels() && ! clipped; ++ch)
    {
        const float* d = buffer.getReadPointer (ch);
        for (int i = 0; i < numSamples; ++i)
            if (std::abs (d[i]) >= 1.0f) { clipped = true; break; }
    }
    if (clipped && ! _clipWas && mm303ShimClipStatus)
        mm303ShimClipStatus();
    _clipWas = clipped;
}

//==============================================================================
bool JC303::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* JC303::createEditor()
{
    return new JC303Editor (*this, parameters);
}

//==============================================================================
// Default MM-303.conf content is embedded at build time from the tracked
// templates/MM-303.conf (single source of truth; falls back to this header).
#include "MM303DefaultConfig.h"

/// @brief Loads "<plugin dir>/MM-303.conf" once: capture path + MIDI CC map.
///
/// Writes the embedded default template first if the file is missing, then
/// parses "key = value" lines. Parsed exactly once per plugin instance.
void JC303::loadMidiConfig()
{
    if (_configLoaded)
        return;

    juce::File dir;
    if (mm303ShimPluginDir)
    {
        const char* p = mm303ShimPluginDir();
        if (p != nullptr && *p != '\0')
            dir = juce::File (juce::String (p));
    }
    if (! dir.isDirectory())
        dir = juce::File::getCurrentWorkingDirectory();

    const juce::File cfg = dir.getChildFile ("MM-303.conf");
    if (! cfg.existsAsFile())
        cfg.replaceWithText (juce::String (kDefaultConfigText));

    _configLoaded = true;   // only parse once (even if read fails)

    juce::StringArray lines;
    cfg.readLines (lines);

    for (const auto& raw : lines)
    {
        const int hash = raw.indexOfChar ('#');
        const juce::String line = (hash >= 0 ? raw.substring (0, hash) : raw).trim();
        if (line.isEmpty())
            continue;

        const int eq = line.indexOfChar ('=');
        if (eq < 0)
            continue;

        const juce::String key = line.substring (0, eq).trim();
        const juce::String val = line.substring (eq + 1).trim();
        if (key.isEmpty())
            continue;

        if (key == "capture-path")
        {
            _capturePath = val;
            continue;
        }

        if (key == "base-octave")
        {
            // Base octave number (3..6); the octave reference is base-octave * 12.
            _baseOctave = juce::jlimit (3, 6, val.getIntValue());
            continue;
        }

        if (key == "octave-max")
        {
            // Max octaves an incoming note may shift the sequence (1..2).
            _octaveMax = juce::jlimit (1, 2, val.getIntValue());
            continue;
        }

        if (key == "captureFormat")
        {
            // What Capture writes: 0 = MIDI, 1 = .mpcpattern v1, 2 = .mpcpattern v2.
            _captureFormat = juce::jlimit (0, 2, val.getIntValue());
            continue;
        }

        if (key == "lastNote")
        {
            // 0 (default) = a tie/slide on the boundary step clamps at the loop
            // point; 1 = let it extend/slide across the loop while playing.
            _clampLoopEnd = (val.getIntValue() == 0);
            continue;
        }

        if (key == "debugDump")
        {
            // 1 = write the raw-state <stamp>.seq.json sidecar next to each
            // Capture (diagnostics only; off by default).
            _debugDump = (val.getIntValue() != 0);
            continue;
        }

        if (auto* p = parameters.getParameter (key))
        {
            const int cc = val.getIntValue();
            if (cc > 0 && cc < 128)
                _ccMap[cc] = p;
        }
    }
}

/// @brief pthread entry for the Capture export worker.
void* JC303::exportRun (void* arg)
{
    static_cast<JC303*> (arg)->exportLoop();
    return nullptr;
}

/// @brief Off-audio-thread loop: performs a queued Capture .mid export.
void JC303::exportLoop()
{
    while (! _exportQuit.load())
    {
        if (_exportRequested.exchange (false))
            exportSequenceMidi();
        usleep (20000);
    }
}

/// @brief Writes the current pattern to a Standard MIDI File (Capture button).
///
/// Destination is the MM-303.conf capture-path (if set and present) else
/// "<plugin dir>/Midi Clips/". File: MM-303-<timestamp>.mid. Message thread.
void JC303::exportSequenceMidi()
{
    TrackData303 td;
    _sequencer.copyTrackData (td);

    const int len = juce::jlimit (1, (int) SEQ303_STEP_MAX, (int) td.stepLength);

    constexpr int kPpq        = 960;  // ticks per quarter note (Force resolution)
    constexpr int kStepTicks  = 240;  // 16th note = ppq / 4
    constexpr int kGateTicks  = 120;  // 50% gate (matches the engine)
    constexpr int kSlideExtra = 200;  // engine slide overhang

    juce::MidiMessageSequence seq;
    const double bpm = juce::jmax (20.0, (double) _sequencer.getTempo());
    seq.addEvent (juce::MidiMessage::tempoMetaEvent ((int) (60000000.0 / bpm)), 0.0);
    seq.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);

    for (int i = 0; i < len; ++i)
    {
        const StepData303& s = td.step[mm303StepIndex (i, _sequencer.getRotation(),
                                                       _sequencer.getRotationDir(),
                                                       SEQ303_STEP_MAX)];
        if (s.rest)
            continue;

        int gate = kGateTicks;
        for (int j = 1; j <= len; ++j)
        {
            // Lookahead must use the ROTATION-MAPPED step (same as the live
            // sequencer's stepAt), otherwise captured gate/slide/tie lengths
            // drift from what actually plays when Rotate != 0.
            const int next = mm303StepIndex ((i + j) % len, _sequencer.getRotation(),
                                             _sequencer.getRotationDir(), SEQ303_STEP_MAX);
            if (s.slide && j == 1 && ! td.step[next].rest)
            {
                gate = kGateTicks + kSlideExtra;
                break;
            }
            else if (td.step[next].tie && td.step[next].rest)
            {
                gate = kGateTicks + j * kStepTicks;
            }
            else if (! td.step[next].rest || ! td.step[next].tie)
            {
                break;
            }
        }

        const int startTick = i * kStepTicks;
        // Clamp at the loop end unless "lastNote = 1" (match what plays).
        const int endTick   = _clampLoopEnd
                                ? juce::jmin (startTick + gate, len * kStepTicks)
                                : startTick + gate;
        const int note      = (int) s.note;
        const int vel       = s.accent ? (int) SEQ303_ACCENT_VELOCITY
                                       : (int) SEQ303_NOTE_VELOCITY;
        seq.addEvent (juce::MidiMessage::noteOn  (1, note, (juce::uint8) vel), startTick);
        seq.addEvent (juce::MidiMessage::noteOff (1, note), endTick);
    }
    seq.updateMatchedPairs();

    // Destination: capture-path from MM-303.conf if set and it exists, else the
    // default <plugin dir>/Midi Clips/.  File: MM-303-<timestamp>.mid.
    juce::File outDir;
    if (_capturePath.isNotEmpty())
    {
        juce::File cp (_capturePath);
        if (cp.isDirectory())
            outDir = cp;
    }
    if (! outDir.isDirectory())
    {
        juce::File base;
        if (mm303ShimPluginDir)
        {
            const char* p = mm303ShimPluginDir();
            if (p != nullptr && *p != '\0')
                base = juce::File (juce::String (p));
        }
        if (! base.isDirectory())
            base = juce::File::getCurrentWorkingDirectory();
        outDir = base.getChildFile ("Midi Clips");
        outDir.createDirectory();
    }

    // Local time + milliseconds so same-second captures don't collide.
    const auto now = juce::Time::getCurrentTime();
    const juce::String stamp = now.formatted ("%Y%m%d-%H%M%S")
                               + juce::String::formatted ("-%03d", now.getMilliseconds());
    const juce::File out = outDir.getChildFile ("MM-303-" + stamp +
        (_captureFormat == 1 || _captureFormat == 2 ? ".mpcpattern" : ".mid"));

    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (kPpq);
    mf.addTrack (seq);
    if (out.existsAsFile())
        out.deleteFile();

    // Raw internal-state sidecar: the STORED 64 steps (index 0..63, no rotation
    // applied) + length/rotation/shift/transpose.  Diagnostics only — written
    // when MM-303.conf "debugDump = 1".  Compare it against the generated
    // MIDI/mpcpattern to see exactly what the Capture algorithm did.
    if (_debugDump)
    {
        juce::String j;
        j << "{\n  \"length\": " << len
          << ",\n  \"rotation\": " << _sequencer.getRotation()
          << ",\n  \"rotationDir\": " << _sequencer.getRotationDir()
          << ",\n  \"shift\": " << (int) td.shift
          << ",\n  \"transpose\": " << (int) td.transpose
          << ",\n  \"ppq\": " << kPpq
          << ",\n  \"stepTicks\": " << kStepTicks
          << ",\n  \"gateTicks\": " << kGateTicks
          << ",\n  \"slideExtra\": " << kSlideExtra
          << ",\n  \"steps\": [\n";
        for (int i = 0; i < SEQ303_STEP_MAX; ++i)
        {
            const StepData303& s = td.step[i];
            j << "    {\"i\": " << i << ", \"note\": " << (int) s.note
              << ", \"rest\": " << (s.rest ? "true" : "false")
              << ", \"accent\": " << (s.accent ? "true" : "false")
              << ", \"slide\": " << (s.slide ? "true" : "false")
              << ", \"tie\": " << (s.tie ? "true" : "false") << "}"
              << (i < SEQ303_STEP_MAX - 1 ? ",\n" : "\n");
        }
        j << "  ],\n  \"playback\": [\n";
        for (int i = 0; i < SEQ303_STEP_MAX; ++i)
        {
            const int si = mm303StepIndex (i, _sequencer.getRotation(),
                                           _sequencer.getRotationDir(), SEQ303_STEP_MAX);
            const StepData303& s = td.step[si];
            j << "    {\"pos\": " << i << ", \"stored\": " << si
              << ", \"note\": " << (int) s.note
              << ", \"rest\": " << (s.rest ? "true" : "false")
              << ", \"accent\": " << (s.accent ? "true" : "false")
              << ", \"slide\": " << (s.slide ? "true" : "false")
              << ", \"tie\": " << (s.tie ? "true" : "false") << "}"
              << (i < SEQ303_STEP_MAX - 1 ? ",\n" : "\n");
        }
        j << "  ]\n}\n";
        const juce::File seqOut = outDir.getChildFile ("MM-303-" + stamp + ".seq.json");
        seqOut.replaceWithText (j);
    }

    // captureFormat 1/2: serialize the SMF and convert to Akai .mpcpattern.
    if (_captureFormat == 1 || _captureFormat == 2)
    {
        juce::MemoryBlock smf;
        {
            juce::MemoryOutputStream mos (smf, false);
            mf.writeTo (mos);
        }
        std::string json;
        const bool ok = (_captureFormat == 2)
            ? mm303::midiToMpcPatternV2 (static_cast<const uint8_t*> (smf.getData()),
                                         smf.getSize(), json)
            : mm303::midiToMpcPattern   (static_cast<const uint8_t*> (smf.getData()),
                                         smf.getSize(), json);
        if (ok)
            out.replaceWithText (juce::String (json.data(), json.size()));
        return;
    }

    if (auto os = out.createOutputStream())
        mf.writeTo (*os);
}

/// @brief Serialises APVTS params + the sequencer pattern into a binary chunk.
///
/// Runtime-only controls (Play State / Generate / Clear / Capture) are stripped
/// so the chunk is idempotent; the pattern is stored as an "AcidSeq303" child.
void JC303::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    // The sequencer is persisted as a child element below; make sure the APVTS
    // state we serialize never already contains one (see setStateInformation).
    state.removeChild (state.getChildWithName ("AcidSeq303"), nullptr);

    // Runtime-only controls are declared non-automatable in
    // createParameterLayout (Play / Sync / Start / Generate / Clear / Capture /
    // Transpose / Octave): never written to presets or project state.
    for (int i = state.getNumChildren(); --i >= 0;)
    {
        const auto child = state.getChild (i);
        if (! child.hasType ("PARAM")) continue;
        if (auto* p = parameters.getParameter (child.getProperty ("id").toString()))
            if (! p->isAutomatable())
                state.removeChild (i, nullptr);
    }

    auto xml = state.createXml();

    // ── Persist sequencer state as a child element ────────────────────────────
    auto seqXml = std::make_unique<juce::XmlElement> ("AcidSeq303");
    seqXml->setAttribute ("stepLength",  _sequencer.getTrackLength());
    seqXml->setAttribute ("transpose",   _sequencer.getTranspose());
    //seqXml->setAttribute ("tune",        _sequencer.getTune());
    //seqXml->setAttribute ("temperament", _sequencer.getTemperamentId());

    const TrackData303& td = _sequencer.getTrackData();
    for (int i = 0; i < SEQ303_STEP_MAX; ++i)
    {
        auto stepXml = std::make_unique<juce::XmlElement> ("Step");
        stepXml->setAttribute ("i",      i);
        stepXml->setAttribute ("note",   td.step[i].note);
        stepXml->setAttribute ("rest",   td.step[i].rest   ? 1 : 0);
        stepXml->setAttribute ("accent", td.step[i].accent ? 1 : 0);
        stepXml->setAttribute ("slide",  td.step[i].slide  ? 1 : 0);
        stepXml->setAttribute ("tie",    td.step[i].tie    ? 1 : 0);
        seqXml->addChildElement (stepXml.release());
    }

    xml->addChildElement (seqXml.release());
    copyXmlToBinary (*xml, destData);
}

/// @brief Restores APVTS params + the sequencer pattern from a binary chunk.
///
/// The "AcidSeq303" child is stripped before replaceState (kept out of the APVTS
/// tree so chunks stay idempotent) and applied to the sequencer directly.
void JC303::setStateInformation (const void* data, int sizeInBytes)
{
    // Anti-click: ask the audio thread to fade out, wait (briefly) for it, apply
    // the state (params + pattern) silently, then release the mute.
    _stateMuteReady.store (false);
    _stateMuteRequest.store (true);
    for (int i = 0; i < 20 && ! _stateMuteReady.load(); ++i)
        juce::Thread::sleep (1);

    std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));
    if (xmlState.get() != nullptr)
    {
        if (xmlState->hasTagName (parameters.state.getType()))
        {
            // Strip the sequencer child before handing the tree to the APVTS:
            // it is stored/restored separately by getStateInformation, and if it
            // lives in the APVTS state it is duplicated on every save/load (the
            // chunk grows). Keeping the APVTS tree clean makes state idempotent.
            auto stateTree = juce::ValueTree::fromXml (*xmlState);
            stateTree.removeChild (stateTree.getChildWithName ("AcidSeq303"), nullptr);

            // The runtime-only controls are stripped from the chunk (they are
            // non-automatable).  replaceState() resets any parameter ABSENT from
            // the tree to its default AND NOTIFIES the host — verified on device:
            // loading a slot fired `automate idx=44/38/34/42 v=0`, which is what
            // made the Force's transpose/octave/play buttons jump.  Re-attach
            // those params to the incoming tree with their CURRENT values, so
            // replaceState sees them unchanged (no reset, no notify, no side
            // effect).
            {
                auto current = parameters.copyState();
                // Drop any runtime-only children the chunk may still carry (old
                // saves), then re-attach them with the current values.
                for (int i = stateTree.getNumChildren(); --i >= 0;)
                {
                    const auto child = stateTree.getChild (i);
                    if (! child.hasType ("PARAM")) continue;
                    if (auto* p = parameters.getParameter (child.getProperty ("id").toString()))
                        if (! p->isAutomatable())
                            stateTree.removeChild (i, nullptr);
                }
                for (int i = 0; i < current.getNumChildren(); ++i)
                {
                    const auto child = current.getChild (i);
                    if (! child.hasType ("PARAM")) continue;
                    if (auto* p = parameters.getParameter (child.getProperty ("id").toString()))
                        if (! p->isAutomatable())
                            stateTree.appendChild (child.createCopy(), nullptr);
                }
            }

            parameters.replaceState (stateTree);

            // ── Restore sequencer sync state ──────────────────────────────────
            // SyncMode/StartMode/Tempo live in the APVTS params only; replaceState
            // does not fire parameterChanged, so push them into the engine here.
            if (auto* sm = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter("seqSyncMode")))
                _sequencer.setSyncMode((AcidSequencer303::SyncMode) sm->getIndex());
            if (auto* sm = dynamic_cast<juce::AudioParameterChoice*>(parameters.getParameter("seqStartMode")))
                _sequencer.setStartMode((AcidSequencer303::StartMode) sm->getIndex());
            if (auto* tp = dynamic_cast<juce::AudioParameterInt*>(parameters.getParameter("seqTempo")))
                _sequencer.setTempo((float) tp->get());

            // ── Restore sequencer state ───────────────────────────────────────
            if (auto* seqXml = xmlState->getChildByName ("AcidSeq303"))
            {
                _sequencer.setTrackLength (static_cast<uint8_t>
                                            (seqXml->getIntAttribute ("stepLength", SEQ303_STEP_MAX)));

                // Mirror the AcidSeq303 child (source of truth for length) into the
                // APVTS params so the UI, LEN knob and the saved param state agree.
                if (auto* lp = dynamic_cast<juce::AudioParameterInt*>(parameters.getParameter("seqLength")))
                    *lp = _sequencer.getTrackLength();

                _sequencer.setTranspose (static_cast<int8_t>
                                            (seqXml->getIntAttribute ("transpose", 0)));
                //_sequencer.setTune      (static_cast<uint8_t>
                //                            (seqXml->getIntAttribute ("tune",      0)));
                //_sequencer.setTemperament (static_cast<uint8_t>
                //                            (seqXml->getIntAttribute ("temperament", 1)));

                for (auto* stepXml : seqXml->getChildIterator())
                {
                    const int i = stepXml->getIntAttribute ("i", -1);
                    if (i >= 0 && i < SEQ303_STEP_MAX)
                    {
                        _sequencer.setStepData (i, static_cast<uint8_t>
                                                    (stepXml->getIntAttribute ("note",   SEQ303_DEFAULT_NOTE)));
                        _sequencer.setRest   (i, stepXml->getIntAttribute ("rest",   1) != 0);
                        _sequencer.setAccent (i, stepXml->getIntAttribute ("accent", 0) != 0);
                        _sequencer.setSlide  (i, stepXml->getIntAttribute ("slide",  0) != 0);
                        _sequencer.setTie    (i, stepXml->getIntAttribute ("tie",    0) != 0);
                    }
                }
            }
        }
    }

    // Re-derive the positional transpose after a load. The pattern restore
    // overwrites the engine's shift with the slot's saved value, while the
    // runtime-only Transpose/Octave toggles were preserved — so an "on" toggle
    // must re-apply its shift (from the last note) here.
    if (transpose != nullptr && transpose->load() > 0.5f && _lastTransposeNote >= 0)
        _sequencer.setTranspose ((int8_t) transposeShiftFor (_lastTransposeNote));

    _stateMuteRequest.store (false);   // release the anti-click mute
}

//==============================================================================
/// @brief JUCE VST2/plugin entry point: constructs a new JC303 instance.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new JC303();
}
