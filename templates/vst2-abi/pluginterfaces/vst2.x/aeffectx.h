/// @file aeffectx.h
/// @brief Hand-written, clean-room VST2 extension declarations: events, time
///        info, pin/speaker properties and extended opcodes.
//
// Numbers and struct shapes are the public VST2 binary ABI, cross-checked
// against sources we author or that are permissively licensed:
//   - mpc-vst-plugins/wrapper/vst2_wrap.c (sd88me, MIT: events, time info,
//     audioMaster automate/getTime/updateDisplay)
//   - akai-airwindows/shim/audioeffectx.h (our own: opcode values)
//   - xaymar-vst2/vst.h (BSD-2-Clause, (c) Michael Fabian Dirks: speaker
//     structs, rectangle, opcode numbering anchors 0x21-0x48)
//   - midi-path-research DPF fork usage (our own, BSD/GPL: audioMaster
//     wantMidi=6, processEvents=8, sizeWindow=0x0F, sampleRate=0x10,
//     blockSize=0x11, beginEdit=0x2B, endEdit=0x2C, version=1)
// Field names required by the VST2 client in the bundled JUCE tree
// (juce_audio_plugin_client_VST2.cpp) — our headers must expose exactly the
// members that code touches.
//
// No third-party or vendor headers are used, vendored, or referenced.
#pragma once

#include "aeffect.h"

/* ---- host (audioMaster) opcodes ----------------------------------------
 * 0/1/7 proven on-device via mpc-vst-plugins (automate, getTime); 6/8/15/16/
 * 17/43/44 via the DPF fork against real hosts; 42 proven on-device (display
 * refresh escape hatch, mpc-vst-plugins wrapper). 13/23/41/45 marked below as
 * canonical-knowledge — low risk (JUCE falls back gracefully on all four). */
/// @brief Host (audioMaster) opcode values passed to the plugin's callback.
enum AudioMasterOpcodesX {
    audioMasterAutomate = 0,        /* index, value, opt — device-proven */
    audioMasterVersion = 1,         /* device/host proven (DPF queries first) */
    audioMasterCurrentId = 2,
    audioMasterIdle = 3,
    audioMasterPinConnected = 4,
    audioMasterWantMidi = 6,        /* DPF: "tell host we want MIDI events" */
    audioMasterGetTime = 7,         /* device-proven (tempo/LFO on Force) */
    audioMasterProcessEvents = 8,   /* DPF sends VstEvents through this */
    audioMasterIOChanged = 13,      /* canonical-knowledge (TODO: verify) */
    audioMasterSizeWindow = 15,     /* DPF: width/height */
    audioMasterGetSampleRate = 16,  /* DPF */
    audioMasterGetBlockSize = 17,   /* DPF */
    audioMasterGetCurrentProcessLevel = 23, /* canonical-knowledge (TODO) */
    audioMasterVendorSpecific = 41, /* canonical-knowledge (TODO) */
    audioMasterUpdateDisplay = 42,  /* device-proven (MPC param display) */
    audioMasterBeginEdit = 43,      /* DPF editParameter(started=true) */
    audioMasterEndEdit = 44,        /* DPF editParameter(started=false) */
    audioMasterCanDo = 45           /* canonical-knowledge (TODO: verify;
                                       only gates JUCE's sizeWindow probe) */
};

/* ---- extended dispatcher opcodes -------------------------------------- */
/// @brief Extended AEffect dispatcher opcodes (VST2.x).
enum AEffectXOpcodes {
    effProcessEvents = 25,          /* device-proven */
    effCanBeAutomated = 26,         /* device-proven */
    effString2Parameter = 27,
    effGetNumProgramCategories = 28, /* obsolete */
    effGetProgramNameIndexed = 29,
    effCopyProgram = 30,            /* obsolete */
    effConnectInput = 31,           /* obsolete */
    effConnectOutput = 32,          /* obsolete */
    effGetInputProperties = 33,     /* numbering anchor: xaymar 0x21 */
    effGetOutputProperties = 34,    /* numbering anchor: xaymar 0x22 */
    effGetPlugCategory = 35,        /* device-proven + xaymar 0x23 */
    effSetSpeakerArrangement = 42,  /* numbering anchor: xaymar 0x2A */
    effSetBypass = 44,              /* numbering anchor: xaymar 0x2C */
    effGetEffectName = 45,          /* device-proven + xaymar 0x2D */
    effGetVendorString = 47,        /* device-proven + xaymar 0x2F */
    effGetProductString = 48,       /* device-proven + xaymar 0x30 */
    effGetVendorVersion = 49,       /* device-proven + xaymar 0x31 */
    effVendorSpecific = 50,         /* xaymar 0x32 */
    effCanDo = 51,                  /* device-proven + xaymar 0x33 */
    effGetTailSize = 52,            /* xaymar 0x34 */
    effKeysRequired = 54,           /* obsolete; handler returns 0 either way */
    effGetVstVersion = 58,          /* device-proven + xaymar 0x3A */
    effEditKeyDown = 59,
    effEditKeyUp = 60,
    effSetEditKnobMode = 61,
    effGetMidiProgramName = 62,
    effGetCurrentMidiProgram = 63,
    effGetMidiProgramCategory = 64,
    effHasMidiProgramsChanged = 65,
    effGetMidiKeyName = 66,
    effBeginSetProgram = 67,
    effEndSetProgram = 68,
    effGetSpeakerArrangement = 69,  /* numbering anchor: xaymar 0x45 */
    effShellGetNextPlugin = 70,
    effStartProcess = 71,           /* numbering anchor: xaymar 0x47 */
    effStopProcess = 72,            /* numbering anchor: xaymar 0x48 */
    effSetTotalSampleToProcess = 73,
    effSetPanLaw = 74,
    effBeginLoadBank = 75,
    effBeginLoadProgram = 76,
    effSetProcessPrecision = 77,
    effGetNumMidiInputChannels = 78,
    effGetNumMidiOutputChannels = 79
};

/// @brief effSetProcessPrecision values (32- vs 64-bit processing).
enum VstProcessPrecision {
    kVstProcessPrecision32 = 0,
    kVstProcessPrecision64 = 1
};

/* ---- events (layouts from mpc-vst-plugins/wrapper/vst2_wrap.c — sd88me,
 * MIT; member names required by JUCE's VSTMidiEventList) ------------------ */
/// @brief Event type tags used in VstEvent::type.
enum VstEventTypes {
    kVstMidiType = 1,
    kVstSysExType = 6  /* canonical event-type numbering (TODO: confirm) */
};

/// @brief Generic VST2 event header (see VstMidiEvent for the MIDI variant).
typedef struct VstEvent {
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
    char data[16];
} VstEvent;

/// @brief A MIDI event delivered via effProcessEvents / audioMasterProcessEvents.
typedef struct VstMidiEvent {
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
    VstInt32 noteLength;
    VstInt32 noteOffset;
    unsigned char midiData[4];
    char detune;
    char noteOffVelocity;
    char reserved1;
    char reserved2;
} VstMidiEvent;

/// @brief A SysEx event (unused by MM-303; declared for ABI completeness).
typedef struct VstMidiSysexEvent {
    VstInt32 type;
    VstInt32 byteSize;
    VstInt32 deltaFrames;
    VstInt32 flags;
    VstInt32 dumpBytes;
    VstIntPtr resvd1;
    char* sysexDump;
    VstIntPtr resvd2;
} VstMidiSysexEvent;

/// @brief A batch of VstEvent pointers (variable length, two-slot inline array).
typedef struct VstEvents {
    VstInt32 numEvents;
    VstIntPtr reserved;
    VstEvent* events[2]; /* variable size */
} VstEvents;

/// @brief Transport/time snapshot returned by audioMasterGetTime.
///
/// Layout cross-checked against sd88me's mpc-vst-plugins (MIT).
typedef struct VstTimeInfo {
    double samplePos;
    double sampleRate;
    double nanoSeconds;
    double ppqPos;
    double tempo;
    double barStartPos;
    double cycleStartPos;
    double cycleEndPos;
    VstInt32 timeSigNumerator;
    VstInt32 timeSigDenominator;
    VstInt32 smpteOffset;
    VstInt32 smpteFrameRate;
    VstInt32 samplesToNextClock;
    VstInt32 flags;
} VstTimeInfo;

/// @brief VstTimeInfo::flags bits (kVstTempoValid, kVstTransportPlaying, …).
enum VstTimeInfoFlags {
    kVstTransportChanged     = 1,
    kVstTransportPlaying     = 1 << 1,
    kVstTransportCycleActive = 1 << 2,
    kVstTransportRecording   = 1 << 3,
    kVstAutomationWriting    = 1 << 6,
    kVstAutomationReading    = 1 << 7,
    kVstNanosValid           = 1 << 8,
    kVstPpqPosValid          = 1 << 9,
    kVstTempoValid           = 1 << 10, /* device-proven (our wrapper) */
    kVstBarsValid            = 1 << 11,
    kVstCyclePosValid        = 1 << 12,
    kVstTimeSigValid         = 1 << 13,
    kVstSmpteValid           = 1 << 14,
    kVstClockValid           = 1 << 15
};

/// @brief SMPTE frame-rate codes (VstTimeInfo::smpteFrameRate).
enum VstSmpteFrameRate {
    kVstSmpte24fps    = 0,
    kVstSmpte25fps    = 1,
    kVstSmpte2997fps  = 2,
    kVstSmpte30fps    = 3,
    kVstSmpte2997dfps = 4,
    kVstSmpte30dfps   = 5,
    kVstSmpteFilm16mm = 6,
    kVstSmpteFilm35mm = 7,
    kVstSmpte239fps   = 10,
    kVstSmpte249fps   = 11,
    kVstSmpte599fps   = 12,
    kVstSmpte60fps    = 13
};

/// @brief Pin (channel) properties for effGetInput/OutputProperties.
///
/// Field set/order required by JUCE's getPinProperties (label[64], flags,
/// arrangementType, shortLabel[8]) — canonical public ABI layout.
typedef struct VstPinProperties {
    char label[kVstMaxLabelLen];
    VstInt32 flags;
    VstInt32 arrangementType;
    char shortLabel[kVstMaxShortLabelLen];
    char future[48];
} VstPinProperties;

/// @brief VstPinProperties::flags bits.
enum VstPinPropertiesFlags {
    kVstPinIsActive = 1 << 0,
    kVstPinIsStereo = 1 << 1,
    kVstPinUseSpeaker = 1 << 2
};

/// @brief One speaker's properties inside a VstSpeakerArrangement.
///
/// Struct shape from xaymar-vst2/vst.h (BSD-2-Clause, © Michael Fabian Dirks).
typedef struct VstSpeakerProperties {
    float azimuth;
    float elevation;
    float radius;
    float reserved;
    char name[kVstMaxNameLen];
    VstInt32 type;
    char future[28];
} VstSpeakerProperties;

/// @brief A speaker arrangement (type + channel count + speaker array).
typedef struct VstSpeakerArrangement {
    VstInt32 type;
    VstInt32 numChannels;
    VstSpeakerProperties speakers[8];
} VstSpeakerArrangement;

/// @brief Speaker position codes (VstSpeakerProperties::type).
enum VstSpeakerType {
    kSpeakerM   = 0,
    kSpeakerL   = 1,  /* xaymar VST_SPEAKER_TYPE_LEFT */
    kSpeakerR   = 2,
    kSpeakerC   = 3,
    kSpeakerLfe = 4,
    kSpeakerLs  = 5,
    kSpeakerRs  = 6,
    kSpeakerSl  = 7,  /* canonical sequence beyond xaymar anchors (TODO:
                         verify; multichannel-only under the 2-out rule) */
    kSpeakerSr  = 8,
    kSpeakerLc  = 9,
    kSpeakerRc  = 10,
    kSpeakerS   = 11,
    kSpeakerLfe2 = 12,
    kSpeakerTfl = 13,
    kSpeakerTfr = 14,
    kSpeakerTfc = 15,
    kSpeakerTm  = 16,
    kSpeakerTrl = 17,
    kSpeakerTrr = 18,
    kSpeakerTrc = 19
};

/* Arrangement numbering: anchors UserDefined/Empty/Mono/Stereo/5.1 = -2/-1/
 * 0/1/0x0F from xaymar-vst2/vst.h; intervening values follow the canonical
 * sequence (only Mono/Stereo/Empty are ever exchanged under the 2-out rule). */
/// @brief Speaker arrangement codes (mono/stereo/5.1…).
enum VstSpeakerArrangementType {
    kSpeakerArrUserDefined = -2,
    kSpeakerArrEmpty = -1,
    kSpeakerArrMono = 0,
    kSpeakerArrStereo = 1,
    kSpeakerArrStereoSurround = 2,
    kSpeakerArrStereoCenter = 3,
    kSpeakerArrStereoSide = 4,
    kSpeakerArrStereoCLfe = 5,
    kSpeakerArr30Cine = 6,
    kSpeakerArr30Music = 7,
    kSpeakerArr31Cine = 8,
    kSpeakerArr31Music = 9,
    kSpeakerArr40Cine = 10,
    kSpeakerArr40Music = 11,
    kSpeakerArr41Cine = 12,
    kSpeakerArr41Music = 13,
    kSpeakerArr50 = 14,
    kSpeakerArr51 = 15,   /* xaymar anchor: VST_ARRANGEMENT_TYPE_5_1 = 0x0F */
    kSpeakerArr60Cine = 16,
    kSpeakerArr60Music = 17,
    kSpeakerArr61Cine = 18,
    kSpeakerArr61Music = 19,
    kSpeakerArr70Cine = 20,
    kSpeakerArr70Music = 21,
    kSpeakerArr71Cine = 22,
    kSpeakerArr71Music = 23,
    kSpeakerArr80Cine = 24,
    kSpeakerArr80Music = 25,
    kSpeakerArr81Cine = 26,
    kSpeakerArr81Music = 27,
    kSpeakerArr102 = 28
};

/// @brief effGetPlugCategory values (cross-checked against xaymar VST_CATEGORY).
enum VstPlugCategory {
    kPlugCategUnknown = 0,
    kPlugCategEffect = 1,
    kPlugCategSynth = 2,
    kPlugCategAnalysis = 3,
    kPlugCategMastering = 4,
    kPlugCategSpacializer = 5,
    kPlugCategRoomFx = 6,
    kPlugSurroundFx = 7,
    kPlugCategRestoration = 8,
    kPlugCategOfflineProcess = 9,
    kPlugCategShell = 10
};

/// @brief MIDI note-name info for effGetMidiKeyName (JUCE's handleGetMidiKeyName).
typedef struct MidiKeyName {
    VstInt32 thisKeyNumber;
    char keyName[kVstMaxNameLen];
} MidiKeyName;
