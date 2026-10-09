/// @file JC303.h
/// @brief MM-303 AudioProcessor declaration: the Open303 303 engine, the MXR
///        overdrive, the acid step sequencer and the APVTS parameter surface.
#pragma once

#include <map>
#include <pthread.h>

#include <JuceHeader.h>

// Open303
#include "dsp/open303/rosic_Open303.h"
using namespace rosic;

#include "dsp/mxr/MxrDistortion.h"

// Aciduino Sequencer
#include "sequencer/AcidSequencer303.h"

enum Open303Parameters
{
  WAVEFORM = 0,
  TUNING,
  CUTOFF,
  RESONANCE,
  ENVMOD,
  DECAY,
  ACCENT,
  VOLUME,
  // MODs
  SWITCH_MOD,
  NORMAL_DECAY,
  ACCENT_DECAY,
  FEEDBACK_HPF,
  SOFT_ATTACK,
  SLIDE_TIME,
  TANH_SHAPER_DRIVE,
  FILTER_DRIVE,
  BASS_COMP,
  // LFO
  LFO_WAVEFORM,
  LFO_RATE,
  LFO_DEPTH,
  LFO_DESTINATION,
  // Overdrive (MXR Distortion+)
  OVERDRIVE_SWITCH,
  OVERDRIVE_LEVEL,
  OVERDRIVE_OUTPUT,
  OVERDRIVE_BASS_BOOST,
  OVERDRIVE_DRY_WET,

  OPEN303_NUM_PARAMETERS
};

//==============================================================================
/// @brief The 303 synth voice plus host-synced acid step sequencer.
///
/// Owns the Open303 engine, the MXR Distortion+ overdrive, the sequencer and
/// the APVTS parameter tree; exposes the sequencer to the editor and turns
/// host/GUI parameter changes into engine updates.
class JC303  :  public juce::AudioProcessor,
                public juce::AudioProcessorValueTreeState::Listener
{
public:
    //==============================================================================
    JC303();
    ~JC303() override;

    //==============================================================================
    /// @brief Prepares the sequencer + engine and loads MM-303.conf (first call).
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    /// @brief Real-time audio callback: renders the sequencer + 303 engine.
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    /// @brief Enables/disables the Devil-Fish-style mod section.
    void setDevilMod(bool mode);

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    /// @brief Serialises the APVTS state into a MemoryBlock (idempotent chunk).
    void getStateInformation (juce::MemoryBlock& destData) override;
    /// @brief Restores APVTS state from a chunk produced by getStateInformation.
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    /// @brief APVTS listener: applies a changed parameter to the engine/sequencer.
    void parameterChanged(const juce::String& parameterID, float newValue) override;

    // ── Sequencer public API (called from Editor / host automation) ───────────

    /** Direct access for the Editor — pattern editing, randomize, display. */
    AcidSequencer303& getSequencer() { return _sequencer; }

    /** Sync source: Internal (own BPM), Host (DAW transport), MidiClock. */
    void setSequencerSyncMode  (AcidSequencer303::SyncMode  m) { _sequencer.setSyncMode  (m); }
    /** Start trigger: TransportStart (play/MIDI Start) or NoteTriggered. */
    void setSequencerStartMode (AcidSequencer303::StartMode m) { _sequencer.setStartMode (m); }

    /** Internal BPM — only active when SyncMode == Internal. */
    void  setSequencerTempo (float bpm) { _sequencer.setTempo (bpm); }
    float getSequencerTempo()     const { return _sequencer.getTempo(); }

    /** Hard start / stop from the UI. */
    void sequencerStart() { _sequencer.start(); }
    void sequencerStop()  { _sequencer.stop();  }

    // ── Sequencer UI→audio command channel ────────────────────────────────────
    enum class SeqCommand : int { None = 0, Play, Stop, Generate, Clear };
    void seqCommand (SeqCommand command) { _seqCommand.store (static_cast<int>(command), std::memory_order_release); }

private:
    /// @brief Clocks the sequencer and dispatches its MIDI/host events.
    void renderMidi  (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages);
    /// @brief Renders the 303 engine into samples [beginSample, endSample).
    void render303   (juce::AudioBuffer<float>& buffer, int beginSample, int endSample);
    /// @brief Applies one engine parameter (see Open303Parameters) by index.
    void setParameter (Open303Parameters index, float value);

    /// @brief Sequence transpose shift for an incoming note (see seqTranspose.h).
    int transposeShiftFor (int note) const;

    // Sequencer UI→audio command channel, consumed at the top of every audio
    // buffer so play/stop/generate/clear (and the accompanying Open303 note
    // flushes) always run on the audio thread.  Setting it from the message
    // thread keeps sequencer + Open303 state single-threaded while running.
    void applySeqCommands();

    // Writes the current sequencer pattern to a Standard MIDI File in
    // "<plugin dir>/Midi Clips/MM-303-<timestamp>.mid".  Message thread.
    void exportSequenceMidi();

    // ── MM-303.conf: capture path + MIDI CC → parameter map ───────────────────
    // Loaded once (lazily) from "<plugin dir>/MM-303.conf"; created with the
    // default template if missing.  Keys: "capture-path" + "<param-id> = <cc>".
    void loadMidiConfig();
    juce::String _capturePath;                              // empty => default
    std::map<int, juce::RangedAudioParameter*> _ccMap;      // CC# -> parameter
    bool _configLoaded = false;
    int _baseOctave { 5 };   // MM-303.conf "base-octave" (3..6); base note = *12
    int _octaveMax  { 2 };   // MM-303.conf "octave-max" (1..2)
    int _captureFormat { 0 };// MM-303.conf "captureFormat": 0 midi, 1 v1, 2 v2
    bool _clampLoopEnd { true };  // MM-303.conf "lastNote": 0 clamp, 1 extend
    bool _debugDump { false };    // MM-303.conf "debugDump": write <stamp>.seq.json

    // embedded core dsp objects
    // Open303
    Open303 open303Core;
    // MXR Distortion+ (vendored GxDistortionPlus DSP)
    MxrDistortion overdrive;
    juce::dsp::DryWetMixer<float> overdriveMix;

    // ── Acid Sequencer ────────────────────────────────────────────────────────
    AcidSequencer303 _sequencer;

    // Pending note events populated by the sequencer callback during
    // _sequencer.processBlock(), then flushed into Open303 inside the
    // existing MIDI render loop. 64 slots >> anything a mono 16-step
    // sequencer can produce per audio buffer.
    struct PendingNote
    {
        Acid303EventType type;
        uint8_t          note;
        uint8_t          velocity;
        int              sampleOffset;
    };
    static constexpr int kPendingMax = 64;
    PendingNote  _pendingNotes[kPendingMax];
    int          _pendingCount { 0 };

    // Tracks host play state across buffers to detect start/stop edges
    bool _wasHostPlaying { false };
    bool _hostTransportSeen { false };
    bool _haveLastPpq { false };
    double _lastHostPpq { 0.0 };

    // Currently held note from the sequencer (-1 = none).
    // Prevents stale NoteOffs from closing a new note after a slide,
    // and detects wrap-around ties (same pitch re-firing at pattern start).
    int  _heldNote         { -1    };

    // Slide flag from the last dispatched step, carried across buffer
    // boundaries so the *receiving* step's NoteOn gets slide=1 correctly.
    bool _lastStepHadSlide { false };

    // Last incoming note used for transposition, so toggling Transpose/Octave
    // can re-derive the shift immediately without waiting for another note.
    int  _lastTransposeNote { -1 };

    // ── Bypass: audio off, MIDI-only ──────────────────────────────────────────
    // _bypassGain fades the output (click-free); once fully faded, _bypassDsp
    // stops the synth/overdrive DSP so the plugin is a pure MIDI generator.
    juce::SmoothedValue<float> _bypassGain { 1.0f };
    bool _bypassDsp { false };
    bool _mutedLast { false };
    bool _clipWas { false };   // rising-edge latch for the shim Clip LED
    double _sr { 44100.0 };
    // Transient mute around state loads so applying a preset/pattern while the
    // sequence is playing does not click (handshake with setStateInformation).
    std::atomic<bool> _stateMuteRequest { false };
    std::atomic<bool> _stateMuteReady { false };

    // ── Capture export worker ─────────────────────────────────────────────────
    // The Capture button writes a .mid; doing that on the audio thread glitches,
    // so parameterChanged only sets _exportRequested and this worker does I/O.
    // pthread (not std::thread): the plugin statically links libstdc++, where
    // std::thread aborts under the armhf build ("Enable multithreading").
    std::atomic<bool> _exportRequested { false };
    std::atomic<bool> _exportQuit { false };
    pthread_t         _exportTid {};
    bool              _exportLive { false };
    void exportLoop();
    static void* exportRun (void* arg);

    // Mute flag for sequencer (used during acidRandomize to prevent note triggering)
    std::atomic<bool> _sequencerMuted { false };

    // Rec-mode live state (audio thread only):
    // - _recHeldNote: last MIDI note recorded while still held, for legato/slide
    //   detection (a new noteOn while one is held records the step with slide).
    // - _sustainArmed: CC64 (sustain pedal) re-arm latch — a 127 tap records one
    //   rest, then waits for 0 before the next 127 can record another.
    // - _recWasOn: edge detector to reset the two above when rec is re-enabled.
    int  _recHeldNote   { -1    };
    bool _sustainArmed  { true  };
    bool _recWasOn      { false };

    //==============================================================================
    juce::AudioProcessorValueTreeState parameters;
    std::atomic<float>* waveForm = nullptr;
    std::atomic<float>* tuning = nullptr;
    std::atomic<float>* cutoffFreq = nullptr;
    std::atomic<float>* resonance = nullptr;
    std::atomic<float>* envelopMod = nullptr;
    std::atomic<float>* decay = nullptr;
    std::atomic<float>* accent = nullptr;
    std::atomic<float>* volume = nullptr;
    // MODs
    std::atomic<float>* switchModState = nullptr;
    std::atomic<float>* normalDecay = nullptr;
    std::atomic<float>* accentDecay = nullptr;
    std::atomic<float>* feedbackFilter = nullptr;
    std::atomic<float>* softAttack = nullptr;
    std::atomic<float>* slideTime = nullptr;
    std::atomic<float>* sqrDriver = nullptr;
    // LFO
    std::atomic<float>* lfoWaveform = nullptr;
    std::atomic<float>* lfoRate = nullptr;
    std::atomic<float>* lfoDepth = nullptr;
    std::atomic<float>* lfoDestination = nullptr;
    // overdrive
    std::atomic<float>* switchOverdriveState = nullptr;
    std::atomic<float>* overdriveLevel = nullptr;
    std::atomic<float>* overdriveOutput = nullptr;
    std::atomic<float>* overdriveBassBoost = nullptr;
    std::atomic<float>* overdriveDryWet = nullptr;
    std::atomic<float>* filterType = nullptr;
    std::atomic<float>* filterDrive = nullptr;
    std::atomic<float>* bassComp = nullptr;
    // generative sequencer
    std::atomic<float>* seqGenerativeFill = nullptr;
    std::atomic<float>* seqGenerativeAccentProbability = nullptr;
    std::atomic<float>* seqGenerativeSlideProbability = nullptr;
    std::atomic<float>* seqGenerativeTieProbability = nullptr;
    std::atomic<float>* numberOfTones = nullptr;
    std::atomic<float>* lowerNote = nullptr;
    std::atomic<float>* rangeNote = nullptr;
    std::atomic<float>* seqPlayState = nullptr;
    std::atomic<float>* seqGenerate = nullptr;
    std::atomic<float>* seqClear = nullptr;
    std::atomic<float>* seqCapture = nullptr;
    std::atomic<float>* transpose = nullptr;
    std::atomic<float>* rotation = nullptr;
    std::atomic<float>* octaveTranspose = nullptr;
    std::atomic<float>* bypass = nullptr;
    //std::atomic<float>* seqHarmonizer = nullptr;
    std::atomic<float>* seqLength = nullptr;
    std::atomic<float>* seqSyncMode = nullptr;
    std::atomic<float>* seqStartMode = nullptr;
    std::atomic<float>* seqTempo = nullptr;

    double decayMin = 200;
    double decayMax = 2000;

    // Flag to track if any parameter has changed
    std::atomic<bool> parametersNeedUpdate { false };

    // Pending sequencer command set by parameterChanged, applied by the audio
    // thread in renderMidi (see applySeqCommands).
    std::atomic<int> _seqCommand { static_cast<int>(SeqCommand::None) };

    //==============================================================================
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (JC303)
};
