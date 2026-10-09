/// @file aeffect.h
/// @brief Hand-written, clean-room VST2 plugin-side ABI declarations
///        (no Steinberg SDK is used, vendored, or referenced by the build).
//
// Layout proven on-device by:
//   - akai-airwindows/shim/audioeffectx.h   (our own; 523/523 plugins on the Force)
//   - mpc-vst-plugins/wrapper/vst2_wrap.c   (sd88me, MIT; built instruments on the Force)
// Field requirements cross-checked against the VST2 client sources in the
// bundled JUCE tree (juce_audio_plugin_client/juce_audio_plugin_client_VST2.cpp),
// which compiles against these declarations.
//
// This file is part of this project. No third-party or vendor headers are
// used, vendored, or referenced by the build.
#pragma once

#include <stdint.h>

typedef int16_t VstInt16;
typedef int32_t VstInt32;
typedef int64_t VstInt64;
#if defined(__LP64__) || defined(_WIN64)
typedef int64_t VstIntPtr;
#else
typedef int32_t VstIntPtr;
#endif
typedef float VstFloat32;
typedef double VstFloat64;

#ifndef VSTCALLBACK
#define VSTCALLBACK
#endif

#define CCONST(a, b, c, d) \
    ((((VstInt32) (a)) << 24) | ((((VstInt32) (b)) << 16)) | (((VstInt32) (c)) << 8) | ((VstInt32) (d)))

#define kEffectMagic CCONST('V', 's', 't', 'P') /* 0x56737450 */

struct AEffect;

/// @brief The host callback every plugin entry point receives.
typedef intptr_t (VSTCALLBACK *audioMasterCallback) (struct AEffect* effect, VstInt32 opcode, VstInt32 index,
                                                     VstIntPtr value, void* ptr, float opt);
typedef intptr_t (VSTCALLBACK *AEffectDispatcherProc) (struct AEffect* effect, VstInt32 opcode, VstInt32 index,
                                                       VstIntPtr value, void* ptr, float opt);
typedef void (VSTCALLBACK *AEffectProcessProc) (struct AEffect* effect, float** inputs, float** outputs,
                                                VstInt32 sampleFrames);
typedef void (VSTCALLBACK *AEffectProcessDoubleProc) (struct AEffect* effect, double** inputs, double** outputs,
                                                      VstInt32 sampleFrames);
typedef void (VSTCALLBACK *AEffectSetParameterProc) (struct AEffect* effect, VstInt32 index, float parameter);
typedef float (VSTCALLBACK *AEffectGetParameterProc) (struct AEffect* effect, VstInt32 index);

/// @brief The VST2 plugin instance: a dispatch table plus process/parameter
///        function pointers.
///
/// Field order is fixed by the VST2 ABI — do not reorder. Binary layout proven
/// on-device (sd88me's mpc-vst-plugins, MIT; the author's own akai-airwindows
/// shim); identical in both.
struct AEffect {
    VstInt32 magic;
    AEffectDispatcherProc dispatcher;
    AEffectProcessProc process;
    AEffectSetParameterProc setParameter;
    AEffectGetParameterProc getParameter;
    VstInt32 numPrograms;
    VstInt32 numParams;
    VstInt32 numInputs;
    VstInt32 numOutputs;
    VstInt32 flags;
    VstIntPtr resvd1;
    VstIntPtr resvd2;
    VstInt32 initialDelay;
    VstInt32 realQualities;
    VstInt32 offQualities;
    float ioRatio;
    void* object;
    void* user;
    VstInt32 uniqueID;
    VstInt32 version;
    AEffectProcessProc processReplacing;
    AEffectProcessDoubleProc processDoubleReplacing;
    char future[56];
};
typedef struct AEffect AEffect;

/// @brief AEffect::flags bit values.
enum AEffectFlags {
    effFlagsHasEditor     = 1 << 0,
    effFlagsCanReplacing  = 1 << 4,
    effFlagsProgramChunks = 1 << 5,
    effFlagsIsSynth       = 1 << 8,
    effFlagsNoSoundInStop = 1 << 9,
    effFlagsCanDoubleReplacing = 1 << 12,
};

/* Base dispatcher opcodes. Values device-proven via our wrappers. */
/// @brief Base AEffect dispatcher opcodes (device-proven).
enum AEffectOpcodes {
    effOpen = 0,
    effClose = 1,
    effSetProgram = 2,
    effGetProgram = 3,
    effSetProgramName = 4,
    effGetProgramName = 5,
    effGetParamLabel = 6,
    effGetParamDisplay = 7,
    effGetParamName = 8,
    effGetVu = 9,             /* obsolete */
    effSetSampleRate = 10,
    effSetBlockSize = 11,
    effMainsChanged = 12,
    effEditGetRect = 13,
    effEditOpen = 14,
    effEditClose = 15,
    effEditDraw = 16,         /* obsolete */
    effEditMouse = 17,        /* obsolete */
    effEditKey = 18,          /* obsolete */
    effEditIdle = 19,
    effEditTop = 20,          /* obsolete */
    effEditSleep = 21,        /* obsolete */
    effIdentify = 22,         /* returns FourCC 'NvEF' */
    effGetChunk = 23,
    effSetChunk = 24,
    effNumOpcodes
};

/* Editor window rectangle. Field names/order as consumed by JUCE's VST2
 * client (createViewRect/getEditorBounds); layout cross-checked against
 * xaymar-vst2/vst.h (BSD-2-Clause, (c) Michael Fabian Dirks). */
/// @brief Editor window rectangle (top/left/bottom/right).
typedef struct ERect {
    VstInt16 top;
    VstInt16 left;
    VstInt16 bottom;
    VstInt16 right;
} ERect;

/// @brief Buffer-length limits for VST2 string opcodes.
enum {
    kVstMaxProgNameLen = 24,
    kVstMaxParamStrLen = 8,
    kVstMaxVendorStrLen = 64,
    kVstMaxProductStrLen = 64,
    kVstMaxEffectNameLen = 32,
    kVstMaxNameLen = 64,
    kVstMaxLabelLen = 64,
    kVstMaxShortLabelLen = 8
};

#define kVstVersion 2400
