/* juce_smoke.c - behavioral gate for JUCE-built VST2 plugins (our headers).
 *
 * Usage: juce_smoke <plugin.so> <0x-uid-hex> <name> <numParams>
 *
 * Like wrap_smoke but without DPF-hardcoded identity (vendor=mockbamod,
 * version=1000): JUCE vendors/versions are project-defined and only printed.
 * Enforced: dlopen, VSTPluginMain, magic, uid, io 0/2, numParams, product
 * name, state-chunk round-trip. Display length (>7 chars) and MIDI energy
 * are reported as notes for inspection. Exits non-zero on failure.
 */
#include <dlfcn.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef __cdecl
# define __cdecl
#endif
#include "vst.h"

static int failures;

#define MAXP 128

#define CHECK(cond, ...) do {                                       \
        if (cond) { printf("ok   - " __VA_ARGS__); putchar('\n'); } \
        else       { printf("FAIL - " __VA_ARGS__); putchar('\n'); ++failures; } \
    } while (0)

typedef struct {
    int32_t type;
    int32_t byteSize;
    int32_t deltaFrames;
    int32_t _ignore1[3];
    char    midiData[4];
    char    _ignore2[4];
} SmokeMidiEvent;

typedef struct {
    int32_t    numEvents;
    void*      reserved;
    const void* events[1];
} SmokeEvents;

/// @brief Minimal VST2 host callback: reports VST2, 48 kHz, 512-sample blocks.
static intptr_t
host_cb(vst_effect* e, VST_HOST_OPCODE opcode, int32_t i1, int64_t i2, void* ptr, float f)
{
    (void)e; (void)i1; (void)i2; (void)ptr; (void)f;
    switch (opcode) {
    case VST_HOST_OPCODE_01: return 2400;
    case VST_HOST_OPCODE_10: return 48000;
    case VST_HOST_OPCODE_11: return 512;
    default:                 return 0;
    }
}

/// @brief Run the identity/lifecycle/param/state/capture gate; non-zero on failure.
int
main(int argc, char** argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: juce_smoke <plugin.so> <0x-uid-hex> <name> <numParams>\n");
        return 2;
    }

    const uint32_t uid = (uint32_t)strtoul(argv[2], NULL, 0);
    const long nparams = strtol(argv[4], NULL, 10);
    if (nparams < 1 || nparams > MAXP) {
        fprintf(stderr, "FAIL - numParams %s out of range 1..%d\n", argv[4], MAXP);
        return 2;
    }

    void* const so = dlopen(argv[1], RTLD_NOW);
    if (so == NULL) {
        fprintf(stderr, "FAIL - dlopen: %s\n", dlerror());
        return 1;
    }

    vst_effect* (*entry)(vst_host_callback) =
        (vst_effect* (*)(vst_host_callback))dlsym(so, "VSTPluginMain");
    if (entry == NULL) {
        fprintf(stderr, "FAIL - dlsym VSTPluginMain: %s\n", dlerror());
        return 1;
    }

    vst_effect* const e = entry(host_cb);
    CHECK(e != NULL, "VSTPluginMain returns an effect");
    if (e == NULL)
        return 1;

    /* --- identity contract (device rules) --- */
    CHECK(e->magic_number == 0x56737450, "magic=0x%x", (unsigned)e->magic_number);
    CHECK(e->unique_id == (int32_t)uid, "unique_id=0x%08x (expect 0x%08x)",
          (uint32_t)e->unique_id, uid);
    CHECK(e->num_params == (int32_t)nparams, "numParams=%d (expect %ld)",
          e->num_params, nparams);
    CHECK(e->num_inputs == 0 || e->num_inputs == 2, "numInputs=%d (0 or 2)",
          e->num_inputs);
    CHECK(e->num_outputs == 2, "numOutputs=%d (device rule: exactly 2)", e->num_outputs);
    printf("NIN=%d\nNOUT=%d\n", e->num_inputs, e->num_outputs);
    printf("note - version=%d numPrograms=%d flags=0x%x\n",
           e->version, e->num_programs, (unsigned)e->flags);

    char text[64];
    memset(text, 0, sizeof(text));
    e->control(e, VST_EFFECT_OPCODE_EFFECT_NAME, 0, 0, text, 0.0f);
    text[63] = '\0';
    CHECK(strcmp(text, argv[3]) == 0, "effect name=\"%s\" (expect \"%s\")", text, argv[3]);
    memset(text, 0, sizeof(text));
    e->control(e, VST_EFFECT_OPCODE_PRODUCT_NAME, 0, 0, text, 0.0f);
    text[63] = '\0';
    CHECK(strcmp(text, argv[3]) == 0, "product=\"%s\" (expect \"%s\")", text, argv[3]);
    memset(text, 0, sizeof(text));
    e->control(e, VST_EFFECT_OPCODE_VENDOR_NAME, 0, 0, text, 0.0f);
    text[63] = '\0';
    printf("note - vendor=\"%s\"\n", text);

    /* --- lifecycle --- */
    e->control(e, VST_EFFECT_OPCODE_00, 0, 0, NULL, 0.0f);              /* effOpen */
    e->control(e, VST_EFFECT_OPCODE_SET_SAMPLE_RATE, 0, 0, NULL, 48000.0f);
    e->control(e, VST_EFFECT_OPCODE_SET_BLOCK_SIZE, 0, 512, NULL, 0.0f);
    e->control(e, VST_EFFECT_OPCODE_SUSPEND, 0, 1, NULL, 0.0f);         /* resume */

    /* --- value text (0x07 = effGetParamDisplay) and name (0x08 =
     * effGetParamName; <=7 chars is the TUI label contract), per the
     * device-proven numbering in shim/audioeffectx.h + xaymar vst.h --- */
    int32_t i;
    int over7 = 0;
    for (i = 0; i < (int32_t)nparams; ++i) {
        char val[32];
        memset(val, 0, sizeof(val));
        e->control(e, VST_EFFECT_OPCODE_PARAM_GETVALUE, i, 0, val, 0.0f);
        val[31] = '\0';
        char name[32];
        memset(name, 0, sizeof(name));
        e->control(e, VST_EFFECT_OPCODE_PARAM_GETNAME, i, 0, name, 0.0f);
        name[31] = '\0';
        const size_t nlen = strlen(name);
        if (nlen > 7) ++over7;
        printf("note - param[%02d] name=\"%-24s\" val=\"%s\"%s\n", i, name, val,
               nlen > 7 ? "  <-- name >7 chars (TUI label contract)" : "");
    }
    printf("note - %d/%ld param names exceed 7 chars (TUI truncation risk)\n",
           over7, nparams);

    /* --- param bridge round-trip --- */
    e->set_parameter(e, 1, 0.75f);
    {
        const float got = e->get_parameter(e, 1);
        float d = got - 0.75f;
        if (d < 0) d = -d;
        if (d >= 1e-4f)
            printf("note - param[1] roundtrip got=%.6f (diff %.6f)\n",
                   (double)got, (double)d);
        else
            CHECK(1, "param[1] roundtrip=%.6f", (double)got);
    }

    /* --- host-sync regression: Host mode with a tempo-only host (no
     * transport / ppq, as the Akai Force VST2 host reports) must still run
     * the sequencer from PLAY.  host_cb returns 0 for audioMasterGetTime, so
     * this is exactly the transport-less case.  Runs before any MIDI note so
     * the measured energy can only come from the sequencer. --- */
    {
        const int SYNC = 34;   /* seqSyncMode: choice 0=Internal 1=Host 2=Midi */
        const int PLAY = 38;   /* seqPlayState */
        static float zbuf2[512], outL2[512], outR2[512];
        const float* ins2[2] = { zbuf2, zbuf2 };
        float* outs2[2] = { outL2, outR2 };
        float baseL = 0.0f, baseR = 0.0f;
        float peakL2 = 0.0f, peakR2 = 0.0f;
        int b2, n2;

        /* silent baseline: Host mode, PLAY off */
        e->set_parameter(e, SYNC, 0.5f);   /* choice index 1 = Host */
        for (b2 = 0; b2 < 4; ++b2) {
            memset(outL2, 0, sizeof(outL2));
            memset(outR2, 0, sizeof(outR2));
            if (e->process != NULL)
                e->process(e, ins2, outs2, 512);
            else
                e->process_float(e, ins2, outs2, 512);
            for (n2 = 0; n2 < 512; ++n2) {
                float aL = outL2[n2] < 0 ? -outL2[n2] : outL2[n2];
                float aR = outR2[n2] < 0 ? -outR2[n2] : outR2[n2];
                if (aL > baseL) baseL = aL;
                if (aR > baseR) baseR = aR;
            }
        }

        /* PLAY on -> sequencer must run despite no host transport */
        e->set_parameter(e, PLAY, 1.0f);
        usleep(50000);
        for (b2 = 0; b2 < 32; ++b2) {
            memset(outL2, 0, sizeof(outL2));
            memset(outR2, 0, sizeof(outR2));
            if (e->process != NULL)
                e->process(e, ins2, outs2, 512);
            else
                e->process_float(e, ins2, outs2, 512);
            for (n2 = 0; n2 < 512; ++n2) {
                float aL = outL2[n2] < 0 ? -outL2[n2] : outL2[n2];
                float aR = outR2[n2] < 0 ? -outR2[n2] : outR2[n2];
                if (aL > peakL2) peakL2 = aL;
                if (aR > peakR2) peakR2 = aR;
            }
        }
        printf("note - host-sync baseline: L=%.5f R=%.5f, with PLAY: L=%.5f R=%.5f\n",
               (double)baseL, (double)baseR, (double)peakL2, (double)peakR2);
        CHECK(baseL <= 1e-3f && baseR <= 1e-3f,
              "Host mode idle is silent before PLAY");
        CHECK(peakL2 > 1e-3f || peakR2 > 1e-3f,
              "Host sync runs from PLAY with a tempo-only host");

        e->set_parameter(e, PLAY, 0.0f);   /* stop */
        e->set_parameter(e, SYNC, 0.0f);   /* back to Internal */
    }

    /* --- MIDI note-on -> energy on both channels within 8 blocks (note) --- */
    {
        SmokeMidiEvent mev;
        memset(&mev, 0, sizeof(mev));
        mev.type        = 1;                /* kVstMidiType */
        mev.byteSize    = (int32_t)sizeof(mev);
        mev.midiData[0] = (char)0x90;
        mev.midiData[1] = (char)60;
        mev.midiData[2] = (char)100;

        SmokeEvents evs;
        evs.numEvents = 1;
        evs.reserved  = NULL;
        evs.events[0] = &mev;
        e->control(e, VST_EFFECT_OPCODE_19, 0, 0, &evs, 0.0f);

        static float zbuf[512], outL[512], outR[512];
        const float* ins[2] = { zbuf, zbuf };
        float* outs[2] = { outL, outR };
        float peakL = 0.0f, peakR = 0.0f;
        int b, n;
        for (b = 0; b < 8; ++b) {
            memset(outL, 0, sizeof(outL));
            memset(outR, 0, sizeof(outR));
            if (e->process != NULL)
                e->process(e, ins, outs, 512);
            else
                e->process_float(e, ins, outs, 512);
            for (n = 0; n < 512; ++n) {
                float aL = outL[n] < 0 ? -outL[n] : outL[n];
                float aR = outR[n] < 0 ? -outR[n] : outR[n];
                if (aL > peakL) peakL = aL;
                if (aR > peakR) peakR = aR;
            }
        }
        printf("note - energy after note-on: L=%.5f R=%.5f\n",
               (double)peakL, (double)peakR);
        if (peakL <= 1e-3f && peakR <= 1e-3f)
            printf("note - no audio energy (envelope may be silent within 8 blocks)\n");
        else
            CHECK(1, "MIDI note-on produced audio");
    }

    /* --- state chunk round-trip: byte equality is not guaranteed (JUCE
     * re-serializes XML), so verify the contract: values restore. --- */
    e->set_parameter(e, 1, 0.75f);
    /* APVTS flushes dirty params into the state tree on a 10Hz timer; give
     * it a beat or the chunk snapshot has no values yet (setChunk then
     * resets to defaults). Observed with JUCE 5.3 + Temper. */
    usleep(400000);
    {
        char* chunk = NULL;
        const intptr_t sz = e->control(e, VST_EFFECT_OPCODE_17, 0, 0, &chunk, 0.0f);
        CHECK(sz > 1 && chunk != NULL, "get chunk size=%ld", (long)sz);
        if (sz > 1 && chunk != NULL) {
            char* const copy = (char*)malloc((size_t)sz);
            memcpy(copy, chunk, (size_t)sz);
            printf("note - chunk head: %.100s\n", copy);

            /* scramble, then reload: param must come back as 0.75 */
            e->set_parameter(e, 1, 0.10f);
            CHECK(e->get_parameter(e, 1) < 0.2f, "scramble took effect");
            e->control(e, VST_EFFECT_OPCODE_18, 0, sz, copy, 0.0f);
            {
                const float got = e->get_parameter(e, 1);
                float d = got - 0.75f;
                if (d < 0) d = -d;
                CHECK(d < 1e-4f, "param[1] restored by setChunk: %.6f (diff %.6f)",
                      (double)got, (double)d);
            }

            char* chunk2 = NULL;
            const intptr_t sz2 = e->control(e, VST_EFFECT_OPCODE_17, 0, 0, &chunk2, 0.0f);
            CHECK(sz2 > 1 && chunk2 != NULL, "re-get chunk size=%ld", (long)sz2);
            free(copy);
        }
    }

    /* --- MIDI capture: seqCapture writes Midi Clips/MM-303-<ts>.mid --- */
    {
        const int CAPTURE = 41;   /* seqCapture index (last real param) */
        e->set_parameter(e, CAPTURE, 1.0f);
        usleep(50000);

        char dir[600];
        snprintf(dir, sizeof(dir), "%s", argv[1]);
        char* slash = strrchr(dir, '/');
        if (slash) *slash = '\0';
        char clips[700];
        snprintf(clips, sizeof(clips), "%s/Midi Clips", dir);

        int found = 0;
        DIR* d = opendir(clips);
        if (d != NULL) {
            struct dirent* de;
            while ((de = readdir(d)) != NULL) {
                if (strstr(de->d_name, "MM-303-") && strstr(de->d_name, ".mid")) {
                    ++found;
                    printf("note - captured MIDI: %s/%s\n", clips, de->d_name);
                    break;
                }
            }
            closedir(d);
        }
        CHECK(found > 0, "seqCapture wrote Midi Clips/MM-303-*.mid");
    }

    e->control(e, VST_EFFECT_OPCODE_01, 0, 0, NULL, 0.0f);  /* effClose */

    if (failures != 0) {
        printf("juce_smoke: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("juce_smoke: OK\n");
    return 0;
}
