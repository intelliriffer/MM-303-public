/// @file preset_shim.cpp
/// @brief MpcPreset ABI shim for VST2 plugins (any framework): appends the +4
///        preset controls, the 128-slot preset bank, the Status LED and the
///        virtual ALSA MIDI output.
//
// preset_shim.cpp — MpcPreset ABI shim for VST2 plugins (any framework).
//
// Compile the plugin's entry point as VSTPluginMainReal (-DVSTPluginMain=
// VSTPluginMainReal on the plugin-client TU only) and link this TU: the real
// VSTPluginMain below wraps the plugin's AEffect with +4 control params
// (Prog#, Load, Save, Status — device-proven Kars semantics):
//   - controls are never automatable (effCanBeAutomated -> 0)
//   - controls are never part of VST chunk state (chunk delegates to the
//     plugin, whose state contains only its own parameters)
//   - Load/Save trigger on ANY setParameter value (Force toggles 0->1->0,
//     so both values mean "pressed"); spring back to 0 on read
//   - Status is a visual-only LED: any Load/Save sets it to 1, a
//     per-instance timer thread pushes 0 to the host after ~500ms even if
//     the host never calls getParameter/dispatch (device-proven: the
//     Force idles silent, so a lazy host-driven tick never fires and
//     the LED would stay on until the next host interaction);
//     host writes to Status are ignored, Prog# never touches it
//   - bank file <uid-hex>-bank.bin via MpcPreset (MPC.settings install dir,
//     falling back to the .so's own directory)
//   - effFlagsIsSynth plugins additionally map CC CC_PC (default 0) to
//     Prog# + auto-load; effect plugins get no MIDI path
//
// Part of this project (templates/preset/). Headers: templates/vst2-abi.

#include <dlfcn.h>
#include <memory>
#include <mutex>
#include <new>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <vector>

#include "aeffect.h"
#include "aeffectx.h"

#include "preset.h"

// Virtual ALSA MIDI output port.  MM-303 is always a MIDI generator, so the
// shim forwards every MIDI event the plugin sends to the host (via
// audioMasterProcessEvents) to one auto-named virtual port.  One port per
// load group: ctors within a short TTL JOIN the same handle (host builds 1-2
// objects per load; the live one may be the second), expired marker creates a
// fresh uniquely-named port.  Mirrors the device-proven lv2wrap design.
#include "RtMidi.h"
#define MM303_VMIDI_CLIENT "MockbaMod"
#define MM303_VMIDI_PORT   "MM-303"
#define VMIDI_MARK         "/dev/shm/MM-303.vst.last"
#define VMIDI_DUP_TTL_MS   500

// Host-sync diagnostics: OFF unless built with -DMM303_SHIM_LOG=1.  When off,
// tlog/glog are no-ops and no log file is written on the device.
#ifndef MM303_SHIM_LOG
#  define MM303_SHIM_LOG 0
#endif

// Runtime-only inner-plugin params are declared non-automatable in the plugin
// (createParameterLayout); the shim reads effCanBeAutomated at open to find
// them, so the plugin's parameter definitions are the single source of truth.

// Momentary trigger params (Generate/Clear/Capture): any write is a press and
// the shim springs them back to 0 so the host widget re-arms (like Load/Save).
#ifndef MM303_MOMENTARY_A
#  define MM303_MOMENTARY_A -1
#endif
#ifndef MM303_MOMENTARY_B
#  define MM303_MOMENTARY_B -1
#endif
#ifndef MM303_MOMENTARY_C
#  define MM303_MOMENTARY_C -1
#endif

// How long a momentary control reads back as "pressed" before the shim springs
// it to 0 (like the Status LED, but shorter) so a GUI can draw the push/release.
// The timer ticks every 50 ms, so the actual release lands in [this, this+50).
#ifndef MM303_MOMENTARY_HOLD_MS
#  define MM303_MOMENTARY_HOLD_MS 200
#endif

extern "C" AEffect* VSTPluginMainReal(audioMasterCallback host);

// Must be default-visibility: DPF builds with -fvisibility=hidden and exports
// only via its version script, so a plain extern "C" symbol would stay hidden.
#if defined(__GNUC__)
#  define PRESET_SHIM_EXPORT __attribute__((visibility("default")))
#else
#  define PRESET_SHIM_EXPORT
#endif

extern "C" PRESET_SHIM_EXPORT AEffect* VSTPluginMain(audioMasterCallback host);

namespace {

constexpr int kNumControls = 5;
enum Control { kCtlProg = 0, kCtlLoad = 1, kCtlSave = 2, kCtlStatus = 3, kCtlClip = 4 };

constexpr const char* kCtlNames[kNumControls] = {
    "Prog#", "Load", "Save", "Status", "Clip"
};

// ── Virtual MIDI output port ─────────────────────────────────────────────
// Port of the current load group: ctors within the TTL window JOIN this handle
// instead of skipping, so whichever object of the pair is live can send. A new
// load (expired marker) replaces it with a fresh uniquely-named port.
std::mutex gVmidiMutex;
std::shared_ptr<rt::midi::RtMidiOut> gVmidiPort;

// RtMidi ALSA getPortName returns "Client:Port C:P" — strip the client prefix
// and the trailing " C:P" so we can compare against the bare name we requested.
std::string vmidiPortOf(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() && s.back() == ' ')
        s.erase(s.size() - 1);
    const std::string::size_type sp = s.rfind(' ');
    if (sp != std::string::npos)
        s = s.substr(0, sp);
    const std::string::size_type c = s.find(':');
    if (c != std::string::npos)
        s = s.substr(c + 1);
    return s;
}

std::string vmidiUniqueName(const char* base) {
    std::string name(base);
    for (int suffix = 2; ; ++suffix) {
        bool taken = false;
        try {
            rt::midi::RtMidiIn probeIn;
            for (unsigned int i = 0, n = probeIn.getPortCount(); !taken && i < n; ++i)
                taken = vmidiPortOf(probeIn.getPortName(i)) == name;
            rt::midi::RtMidiOut probeOut;
            for (unsigned int i = 0, n = probeOut.getPortCount(); !taken && i < n; ++i)
                taken = vmidiPortOf(probeOut.getPortName(i)) == name;
        } catch (...) { return name; }
        if (!taken)
            return name;
        name = std::string(base) + "-" + std::to_string(suffix);
    }
}

// JUCE's VST2 client requests only the position flags from audioMasterGetTime
// (kVstPpqPosValid|kVstTempoValid|...), never the transport bits.  A host that
// fills just the requested flags — the Force does — therefore reports
// isPlaying=false and a constant ppq, so any host-sync code sees nothing.
// Intercept getTime and ask for the transport state explicitly, the way the
// DPF/plain-C wrappers do.  This is also where we learn what the host actually
// sends: mm303-host.log records getTime results + all opcode counts.
audioMasterCallback g_realHost = nullptr;
AEffect* g_shimEff = nullptr;   // the AEffect the host was handed (shim, not orig)

uint32_t g_outCounts[256];      // host-callback opcodes (plugin -> host)
uint32_t g_inCounts[256];       // dispatcher opcodes (host -> plugin)
uint8_t  g_inSeen[256];         // first-seen marker for host -> plugin opcodes
uint8_t  g_midiSeen[256];       // first-seen marker for MIDI input status bytes
int      g_eventsSeen = 0;
uint32_t g_blockCount = 0;
uint32_t g_lastGetFlags = 0;
int      g_haveGetFlags = 0;
int      g_loggedOpens  = 0;

void glog(const char* fmt, ...);   // defined after Shim (needs its dir)
void vmidiForwardEvents(const void* events);   // defined after Shim (needs g_shim)

/// @brief Host callback the shim passes to the plugin: forwards to the real host,
///        intercepting audioMasterGetTime (transport probe) and
///        audioMasterProcessEvents (mirror MIDI to the virtual port).
intptr_t VSTCALLBACK shimHostCb(AEffect* e, VstInt32 opcode, VstInt32 index,
                                intptr_t value, void* ptr, float opt) {
    if (g_realHost == nullptr)
        return 0;
    // The host only knows the AEffect our VSTPluginMain returned (the shim).
    // JUCE calls host opcodes with its own inner AEffect; map it back so the
    // host recognises the instance (mirrors notify()'s &s->eff usage).
    AEffect* he = g_shimEff ? g_shimEff : e;
    if ((uint32_t)opcode < 256) g_outCounts[(uint32_t)opcode]++;
    if (opcode == audioMasterAutomate) {
        // Plugin -> host param change (fires for setParameter AND for
        // replaceState during a state load).  Shows who moves which param.
        glog("automate idx=%d v=%.4f", (int)index, (double)opt);
    }
    if (opcode == audioMasterGetTime) {
        const intptr_t transportFlags = kVstTransportPlaying | kVstTransportRecording
                             | kVstTransportCycleActive
                             | kVstPpqPosValid | kVstTempoValid | kVstBarsValid
                             | kVstCyclePosValid | kVstTimeSigValid
                             | kVstNanosValid;
        // Ask twice: with the flags JUCE passed, and with the transport flags.
        // Tells us whether the host answers at all / only specific flag sets.
        const intptr_t rOrig = g_realHost(he, opcode, index, value, ptr, opt);
        const intptr_t rTr   = g_realHost(he, opcode, index, transportFlags, ptr, opt);
        const VstTimeInfo* a = reinterpret_cast<const VstTimeInfo*>(rOrig);
        const VstTimeInfo* b = reinterpret_cast<const VstTimeInfo*>(rTr);
        const uint32_t fa = a ? (uint32_t)a->flags : 0xFFFFFFFFu;
        const uint32_t fb = b ? (uint32_t)b->flags : 0xFFFFFFFFu;
        const uint32_t key = fa ^ (fb << 1);
        if (!g_haveGetFlags || key != g_lastGetFlags) {
            g_haveGetFlags = 1;
            g_lastGetFlags = key;
            glog("getTime juceFlags=0x%lx -> %s f=0x%04x tempo=%.2f ppq=%.4f playing=%d",
                 (long)value, a ? "ok" : "NULL", fa, a ? a->tempo : 0.0,
                 a ? a->ppqPos : 0.0,
                 (a && (a->flags & kVstTransportPlaying)) ? 1 : 0);
            glog("getTime trFlags=0x%lx -> %s f=0x%04x tempo=%.2f ppq=%.4f playing=%d",
                 (long)transportFlags, b ? "ok" : "NULL", fb, b ? b->tempo : 0.0,
                 b ? b->ppqPos : 0.0,
                 (b && (b->flags & kVstTransportPlaying)) ? 1 : 0);
        }
        return rTr ? rTr : rOrig;
    }
    if (opcode == audioMasterProcessEvents) {
        // Mirror the plugin's MIDI out (sequencer notes) to the virtual port.
        vmidiForwardEvents(ptr);
        return g_realHost(he, opcode, index, value, ptr, opt);
    }
    return g_realHost(he, opcode, index, value, ptr, opt);
}

struct Shim {
    AEffect eff;
    AEffect* orig;
    audioMasterCallback host;
    int numReal;
    float prog;
    float status;
    uint64_t blinkUntil;    // monotonic ms; Status=1 until then, then 0
    float clip;             // visual-only clip LED: 1 while clipping
    uint64_t clipUntil;     // monotonic ms; Clip=1 until then, then 0
    char dir[512];
    bool opened;
    FILE* logf;
    float lastGet[kNumControls];   // change-suppression for control getParameter logging
    pthread_t blinkTid; // per-instance timer: pushes LED-off at the deadline
    bool blinkLive;     // timer thread created
    bool closing;       // effClose asks the timer to exit
    pthread_mutex_t mu; // guards status/blinkUntil/closing across threads
    bool progExp;       // bank exposed as 128 VST programs (#000-#127)
    // Momentary trigger params awaiting an ASYNC reset: the Force's latch must
    // be re-armed by pushing 0 back from the timer thread (like the Status LED),
    // not synchronously inside the host's setParameter call.
    int  momPending[16];
    uint64_t momPendingAt[16];   // monotonic ms when each entry springs back
    int  momPendingCount;
    // Plugin state captured at open — applied when an empty bank slot is loaded
    // (so an unset slot gives a default sound + default sequence, not whatever
    // was last playing).
    std::vector<char> defaultChunk;
    // Virtual ALSA MIDI out for this load group (shared between the load's
    // ctors; distinct handles across loads).  Non-trivial member => the Shim
    // is allocated with new/delete, not calloc/free.
    std::shared_ptr<rt::midi::RtMidiOut> midiOut;
};

inline Shim* shimOf(AEffect* e) { return reinterpret_cast<Shim*>(e); }

inline bool isMomentaryIndex(int i) {
    return i == MM303_MOMENTARY_A || i == MM303_MOMENTARY_B
        || i == MM303_MOMENTARY_C;
}

// All host-facing param values are VST2-normalized 0..1; slots are 0..127.
inline int slotOf(float norm) {
    int slot = static_cast<int>(norm * 127.0f + 0.5f);
    if (slot < 0) slot = 0;
    if (slot > 127) slot = 127;
    return slot;
}
inline float normOfSlot(int slot) { return static_cast<float>(slot) / 127.0f; }

// Global instance handle so the free-function host callback can log to the
// same directory as the plugin.  One plugin instance per process on the Force.
Shim* g_shim = nullptr;

/// @brief Forwards the plugin's outgoing MIDI to this load group's virtual port.
///
/// Called from the audio thread with the audioMasterProcessEvents payload; each
/// load group owns its own RtMidiOut and exceptions are swallowed so RtMidi
/// cannot kill audio.
void vmidiForwardEvents(const void* events) {
    if (g_shim == nullptr || g_shim->midiOut == nullptr || events == nullptr)
        return;
    const VstEvents* evs = static_cast<const VstEvents*>(events);
    for (VstInt32 i = 0; i < evs->numEvents; ++i) {
        const VstEvent* ev = evs->events[i];
        if (ev == nullptr || ev->type != kVstMidiType)
            continue;
        const VstMidiEvent* me = reinterpret_cast<const VstMidiEvent*>(ev);
        const unsigned char st = static_cast<unsigned char>(me->midiData[0]);
        if (st == 0xF0)   // sysex has no fixed length here -> skip
            continue;
        int len = 3;
        if ((st & 0xF0) == 0xC0 || (st & 0xF0) == 0xD0)  len = 2;
        else if (st >= 0xF0)                              len = 1;   // realtime/common
        try {
            std::vector<unsigned char> msg(me->midiData, me->midiData + len);
            g_shim->midiOut->sendMessage(&msg);
        } catch (...) {}
    }
}

/// @brief Global debug log (only when built with MM303_SHIM_LOG=1).
void glog(const char* fmt, ...) {
#if ! MM303_SHIM_LOG
    (void) fmt;
#else
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    static FILE* f = nullptr;
    if (!f) {
        char path[600];
        if (g_shim && g_shim->dir[0])
            snprintf(path, sizeof(path), "%s/preset-shim.log", g_shim->dir);
        else
            snprintf(path, sizeof(path), "/tmp/preset-shim.log");
        f = fopen(path, "a");
        if (!f) { pthread_mutex_unlock(&mu); return; }
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(f, "%ld.%03ld ", (long)ts.tv_sec, ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
    pthread_mutex_unlock(&mu);
#endif
}

/// @brief Emits a periodic (every 2000 blocks) opcode-count summary trace.
void maybeSummary() {
    if ((++g_blockCount % 2000) != 0) return;
    glog("SUMMARY blk=%u | out automate=%u idle=%u getTime=%u procEvents=%u "
         "| in open=%u close=%u setRate=%u setBlock=%u mains=%u "
         "events=%u start=%u stop=%u | lastGetFlags=0x%04x",
         g_blockCount,
         g_outCounts[audioMasterAutomate], g_outCounts[audioMasterIdle],
         g_outCounts[audioMasterGetTime], g_outCounts[audioMasterProcessEvents],
         g_inCounts[effOpen], g_inCounts[effClose], g_inCounts[effSetSampleRate],
         g_inCounts[effSetBlockSize], g_inCounts[effMainsChanged],
         g_inCounts[effProcessEvents], g_inCounts[effStartProcess],
         g_inCounts[effStopProcess], (unsigned)g_lastGetFlags);
}

/// @brief Asks the host for MIDI (audioMasterWantMidi) on resume.
///
/// DPF (the working container) does this; the Force may gate MIDI/transport
/// delivery on that request.
void requestMidi(Shim* s) {
    if (g_realHost) {
        g_realHost(&s->eff, audioMasterWantMidi, 0, 0, nullptr, 0.0f);
        glog("out audioMasterWantMidi");
    }
}

/// @brief Per-instance debug trace (only when built with MM303_SHIM_LOG=1).
///
/// Writes <dir>/preset-shim.log (fallback /tmp/), fopen once, fflush per line so
/// a crash leaves evidence. No-op otherwise.
void tlog(Shim* s, const char* fmt, ...) {
#if ! MM303_SHIM_LOG
    (void) s; (void) fmt;
#else
    static pthread_mutex_t logMu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&logMu);
    if (!s->logf) {
        char path[560];
        if (s->dir[0])
            snprintf(path, sizeof(path), "%s/preset-shim.log", s->dir);
        else
            snprintf(path, sizeof(path), "/tmp/preset-shim.log");
        s->logf = fopen(path, "a");
        if (!s->logf) { pthread_mutex_unlock(&logMu); return; }
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(s->logf, "%ld.%03ld ", (long)ts.tv_sec, ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(s->logf, fmt, ap);
    va_end(ap);
    fputc('\n', s->logf);
    fflush(s->logf);
    pthread_mutex_unlock(&logMu);
#endif
}

/// @brief Pushes a parameter value back to the host (audioMasterAutomate).
void notify(AEffect* e, int index, float v) {
    Shim* s = shimOf(e);
    if (s->host)
        s->host(e, audioMasterAutomate, index, 0, nullptr, v);
}

void realSet(Shim* s, int i, float v);   // defined below (used by blinkTick)
uint64_t nowMs();                         // defined below (used by queueReset)

/// @brief Queues an async spring-back (push 0) for a momentary control index.
///
/// The 0 is pushed from the timer thread (like the Status LED), after a short
/// hold so the button reads as pressed then released. A synchronous push inside
/// the host's setParameter does not re-arm the Force widget.
void queueReset(Shim* s, int idx) {
    pthread_mutex_lock(&s->mu);
    if (s->momPendingCount < 16) {
        s->momPending[s->momPendingCount]   = idx;
        s->momPendingAt[s->momPendingCount] = nowMs() + MM303_MOMENTARY_HOLD_MS;
        ++s->momPendingCount;
    }
    pthread_mutex_unlock(&s->mu);
}

/// @brief Pokes the host (audioMasterIdle) so it re-reads params and repaints.
///
/// The Force redraws plugin knob widgets only on audioMasterIdle: per-param
/// automate pushes update controls (LED) but a silent state restore (setChunk /
/// doLoad) leaves the GUI stale, so this runs after any state application.
void pushRefresh(AEffect* e) {
    Shim* s = shimOf(e);
    if (s->host)
        s->host(e, audioMasterIdle, 0, 0, nullptr, 0.0f);
}

/// @brief Monotonic milliseconds (CLOCK_MONOTONIC).
uint64_t nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/// @brief Arms the visual-only Status LED (1 for ~500 ms, then back to 0).
///
/// Any Load/Save calls this. Expiry is pushed by a per-instance timer thread
/// (the Force makes no getParameter/dispatch calls while idle, so a host-driven
/// lazy tick never fires — the LED would stick on until the next interaction);
/// blinkTick() also runs lazily from getParameter/dispatch as a fallback. Host
/// writes to Status are ignored.
void blinkOn(Shim* s) {
    pthread_mutex_lock(&s->mu);
    s->status = 1.0f;
    s->blinkUntil = nowMs() + 500;
    pthread_mutex_unlock(&s->mu);
    tlog(s, "STATUS blink on (1 for ~500ms)");
    notify(&s->eff, s->numReal + kCtlStatus, 1.0f);
}

/// @brief Arms the visual-only Clip LED (1 for ~150 ms, then back to 0).
///
/// The plugin calls the mm303ShimClipStatus() hook on the clipped edge.
void clipOn(Shim* s) {
    pthread_mutex_lock(&s->mu);
    s->clip = 1.0f;
    s->clipUntil = nowMs() + 150;
    pthread_mutex_unlock(&s->mu);
    tlog(s, "CLIP on (1 for ~150ms)");
    notify(&s->eff, s->numReal + kCtlClip, 1.0f);
}

/// @brief Timer-thread tick: expires the Status + Clip LEDs and flushes the
///        momentary spring-back queue (pushes 0 to the plugin + host).
void blinkTick(Shim* s) {
    pthread_mutex_lock(&s->mu);
    const uint64_t now = nowMs();
    bool expired = s->blinkUntil && now >= s->blinkUntil;
    if (expired) {
        s->blinkUntil = 0;
        s->status = 0.0f;
    }
    bool clipExpired = s->clipUntil && now >= s->clipUntil;
    if (clipExpired) {
        s->clipUntil = 0;
        s->clip = 0.0f;
    }
    int pend[16];
    int pc = 0;
    int still = 0;
    for (int i = 0; i < s->momPendingCount; ++i) {
        if (now >= s->momPendingAt[i]) {
            if (pc < 16) pend[pc++] = s->momPending[i];   // hold elapsed -> release
        } else {
            s->momPending[still]   = s->momPending[i];    // keep until its deadline
            s->momPendingAt[still] = s->momPendingAt[i];
            ++still;
        }
    }
    s->momPendingCount = still;
    pthread_mutex_unlock(&s->mu);
    if (expired) {
        tlog(s, "STATUS blink off (0)");
        notify(&s->eff, s->numReal + kCtlStatus, 0.0f);
    }
    if (clipExpired) {
        tlog(s, "CLIP off (0)");
        notify(&s->eff, s->numReal + kCtlClip, 0.0f);
    }
    // Momentary triggers + Load/Save: push the 0 back once the hold elapses, off
    // the host's setParameter call, so the Force re-arms its latch and the
    // button can show pressed-then-released.
    for (int i = 0; i < pc; ++i) {
        if (pend[i] < s->numReal)
            realSet(s, pend[i], 0.0f);
        notify(&s->eff, pend[i], 0.0f);
        tlog(s, "MOM idx=%d -> reset0+notify", pend[i]);
    }
}

/// @brief Per-instance timer thread: 50 ms ticks driving blinkTick() until close.
void* blinkTimer(void* arg) {
    Shim* s = static_cast<Shim*>(arg);
    for (;;) {
        pthread_mutex_lock(&s->mu);
        bool stop = s->closing;
        pthread_mutex_unlock(&s->mu);
        if (stop) break;
        usleep(50000);
        blinkTick(s);
    }
    return nullptr;
}

/// @brief Reads a parameter straight from the wrapped plugin.
float realGet(Shim* s, int i) {
    return s->orig->getParameter(s->orig, i);
}

/// @brief Writes a parameter straight to the wrapped plugin.
void realSet(Shim* s, int i, float v) {
    s->orig->setParameter(s->orig, i, v);
}

/// @brief Loads the bank slot for the current Prog# into the plugin.
///
/// Restores every non-transient parameter and pushes the values to the host so
/// the widgets repaint.
// ── Bank: ONE fixed 128-slot file ───────────────────────────────────────────
// <uid>-bank.bin holds 128 fixed-size records; each record is the plugin's full
// VST chunk (params + the sequencer pattern).  This is the single bank — the old
// separate float bank (<uid>-chunks.bin sidecar) is gone.  A record is read only
// when its slot is loaded.
#define MM303_BANK_SLOTS        128
#define MM303_BANK_RECORD_BYTES 8192

static std::string bankPath(Shim* s) {
    char uidHex[9];
    snprintf(uidHex, sizeof(uidHex), "%08x", (unsigned)s->orig->uniqueID);
    return std::string(s->dir) + "/" + uidHex + "-bank.bin";
}

/// @brief Ensures the bank exists at the fixed size, with every slot initialised
///        to the state the plugin loaded with.  Recreates it if the size doesn't
///        match (e.g. the old float-bank layout).
static void ensureBank(Shim* s) {
    if (!s->dir[0]) return;
    const off_t want = (off_t) MM303_BANK_SLOTS * MM303_BANK_RECORD_BYTES;
    struct stat st;
    if (stat(bankPath(s).c_str(), &st) == 0 && st.st_size == want)
        return;
    // Build one record from the plugin's load state (len + chunk, zero-padded).
    std::vector<char> rec(MM303_BANK_RECORD_BYTES, 0);
    if (!s->defaultChunk.empty()) {
        uint32_t len = (uint32_t) s->defaultChunk.size();
        if (len > MM303_BANK_RECORD_BYTES - 4) len = MM303_BANK_RECORD_BYTES - 4;
        memcpy(&rec[0], &len, sizeof len);
        memcpy(&rec[sizeof len], s->defaultChunk.data(), len);
    }
    FILE* f = fopen(bankPath(s).c_str(), "wb");
    if (!f) return;
    for (int i = 0; i < MM303_BANK_SLOTS; ++i)
        fwrite(&rec[0], 1, rec.size(), f);
    fclose(f);
}

/// @brief Store the plugin's current chunk (whole preset) into slot `slot`.
static void saveBankSlot(Shim* s, int slot) {
    if (!s->dir[0] || slot < 0 || slot >= MM303_BANK_SLOTS) return;
    char* p = nullptr;
    const intptr_t n = s->orig->dispatcher(s->orig, effGetChunk, 0, 0, &p, 0.0f);
    if (n <= 0 || p == nullptr) return;
    uint32_t len = (uint32_t)n;
    if (len > MM303_BANK_RECORD_BYTES - 4) len = MM303_BANK_RECORD_BYTES - 4;
    std::vector<char> rec(MM303_BANK_RECORD_BYTES, 0);
    memcpy(&rec[0], &len, sizeof len);
    memcpy(&rec[sizeof len], p, len);
    FILE* f = fopen(bankPath(s).c_str(), "r+b");
    if (!f) return;
    fseeko(f, (off_t)slot * MM303_BANK_RECORD_BYTES, SEEK_SET);
    fwrite(&rec[0], 1, rec.size(), f);
    fclose(f);
}

/// @brief Captures the plugin's current (init) state as the default chunk.
static void captureDefaultChunk(Shim* s) {
    char* p = nullptr;
    const intptr_t n = s->orig->dispatcher(s->orig, effGetChunk, 0, 0, &p, 0.0f);
    if (n > 0 && p != nullptr)
        s->defaultChunk.assign(p, p + n);
}

/// @brief Restore slot `slot`'s record (effSetChunk) into the plugin.  If the
///        slot is empty, apply the captured default state instead (default
///        sound + default sequence, not the previously loaded one).
static void loadBankSlot(Shim* s, int slot) {
    if (!s->dir[0] || slot < 0 || slot >= MM303_BANK_SLOTS) return;
    std::vector<char> buf;
    if (FILE* f = fopen(bankPath(s).c_str(), "rb")) {
        if (fseeko(f, (off_t)slot * MM303_BANK_RECORD_BYTES, SEEK_SET) == 0) {
            uint32_t len = 0;
            if (fread(&len, sizeof len, 1, f) == 1
                && len > 0 && len <= MM303_BANK_RECORD_BYTES - 4) {
                buf.resize(len);
                if (fread(&buf[0], 1, len, f) != len) buf.clear();
            }
        }
        fclose(f);
    }
    if (buf.empty()) {
        if (s->defaultChunk.empty()) return;   // nothing captured yet
        tlog(s, "LOAD slot=%d empty -> default", slot);
        s->orig->dispatcher(s->orig, effSetChunk, 0,
                            (VstIntPtr)s->defaultChunk.size(), &s->defaultChunk[0], 0.0f);
        return;
    }
    s->orig->dispatcher(s->orig, effSetChunk, 0, (VstIntPtr)buf.size(), &buf[0], 0.0f);
}

void doLoad(Shim* s) {
    int slot = slotOf(s->prog);
    blinkOn(s);   // LED = press reached the shim (even if the load then fails)
    if (!s->dir[0]) { tlog(s, "LOAD slot=%d BLOCKED (no dir)", slot); return; }
    tlog(s, "LOAD slot=%d", slot);
    // The single bank record IS the whole preset (params + pattern); it applies
    // through the plugin's anti-click fade (setStateInformation).
    loadBankSlot(s, slot);
    pushRefresh(&s->eff);
}

/// @brief Saves the plugin's whole state (chunk) into the bank slot for Prog#.
void doSave(Shim* s) {
    int slot = slotOf(s->prog);
    blinkOn(s);
    if (!s->dir[0]) { tlog(s, "SAVE slot=%d BLOCKED (no dir)", slot); return; }
    ensureBank(s);
    tlog(s, "SAVE slot=%d", slot);
    saveBankSlot(s, slot);
}

/// @brief Resolves the install dir and ensures the single bank exists (called
///        once, on the plugin's effOpen).
void captureDefaultsAndInit(Shim* s) {
    Dl_info info;
    const char* soPath = nullptr;
    if (dladdr(reinterpret_cast<void*>(&VSTPluginMain), &info) && info.dli_fname)
        soPath = info.dli_fname;

    char uidHex[9];
    snprintf(uidHex, sizeof(uidHex), "%08x",
             static_cast<unsigned int>(s->orig->uniqueID));

    const char* base = "plugin.so";
    if (soPath) {
        const char* slash = strrchr(soPath, '/');
        base = slash ? slash + 1 : soPath;
    }

    char* dir = MpcPreset::findInstallDir(uidHex, base,
                                          "/data/Settings/MPC/MPC.settings");
    if (dir) {
        snprintf(s->dir, sizeof(s->dir), "%s", dir);
        free(dir);
    } else if (soPath) {
        snprintf(s->dir, sizeof(s->dir), "%s", soPath);
        char* slash = strrchr(s->dir, '/');
        if (slash) *slash = '\0';
    } else {
        s->dir[0] = '\0';
    }
    if (!s->dir[0]) return;
    captureDefaultChunk(s);   // capture the load state BEFORE filling the bank
    ensureBank(s);
}

/// @brief True for dispatcher opcodes whose `idx` targets the param/control slot.
bool paramIndexedOpcode(int32_t op) {
    switch (op) {
        case effGetParamLabel:
        case effGetParamDisplay:
        case effGetParamName:
        case effCanBeAutomated:
        case effString2Parameter:
        case 66: /* effGetParamProperties (opcode 66, not in our header) */
            return true;
        default:
            return false;
    }
}

/// @brief Writes the name of a control slot (Prog#/Load/Save/Status) to ptr.
void controlName(Shim* s, int idx, void* ptr) {
    int c = idx - s->numReal;
    if (ptr && c >= 0 && c < kNumControls)
        strcpy(static_cast<char*>(ptr), kCtlNames[c]);
}

/// @brief Writes the display string of a control slot (numeric).
void controlDisplay(Shim* s, int idx, void* ptr) {
    if (!ptr) return;
    int c = idx - s->numReal;
    int v = 0;
    if (c == kCtlProg || c == kCtlStatus || c == kCtlClip)
        v = slotOf(c == kCtlProg ? s->prog
                                 : (c == kCtlStatus ? s->status : s->clip));
    snprintf(static_cast<char*>(ptr), 8, "%d", v);
}

/// @brief Inner dispatcher: handles the control slots, the program bank and the
///        chunk/refresh special cases, forwarding everything else to the plugin.
intptr_t shimDispatch(AEffect* e, int32_t op, int32_t idx, VstIntPtr value,
                      void* ptr, float opt) {
    Shim* s = shimOf(e);
    AEffect* o = s->orig;
    if ((uint32_t)op < 256) {
        g_inCounts[(uint32_t)op]++;
        if (!g_inSeen[(uint32_t)op]) {
            g_inSeen[(uint32_t)op] = 1;
            glog("host op=%d first idx=%d val=%ld", (int)op, (int)idx, (long)value);
        }
    }
    switch (op) {
        case effSetSampleRate: glog("host effSetSampleRate %.0f", (double)opt); break;
        case effSetBlockSize:  glog("host effSetBlockSize %d", (int)value); break;
        case effMainsChanged:
            glog("host effMainsChanged %d", (int)value);
            if (value != 0) requestMidi(s);
            break;
        case effStartProcess:
            glog("host effStartProcess");
            requestMidi(s);
            break;
        case effStopProcess:   glog("host effStopProcess"); break;
        case effGetChunk:      glog("host effGetChunk"); break;
        case effSetChunk:      glog("host effSetChunk %d bytes", (int)value); break;
        default: break;
    }
    blinkTick(s);

    if (s->progExp) {
        switch (op) {
            case effSetProgram: {
                // Force/JUCE convention: program number travels in `value`.
                // Route through our bank loader (spec: never call the
                // plugin's setCurrentProgram).
                int slot = static_cast<int>(value);
                if (slot < 0) slot = 0;
                if (slot > 127) slot = 127;
                s->prog = normOfSlot(slot);
                notify(e, s->numReal + kCtlProg, s->prog);
                tlog(s, "PROGRAM set value=%d slot=%d",
                     static_cast<int>(value), slot);
                doLoad(s);
                return 0;
            }
            case effGetProgram:
                return slotOf(s->prog);
            case effGetProgramName:  // opcode 5: CURRENT program (JUCE)
                if (ptr)
                    snprintf(static_cast<char*>(ptr), 24 + 1, "#%03d",
                             slotOf(s->prog));
                return 0;
            case effGetProgramNameIndexed:  // opcode 29: by index (JUCE)
                if (ptr && idx >= 0 && idx < 128) {
                    snprintf(static_cast<char*>(ptr), 24 + 1, "#%03d", idx);
                    return 1;
                }
                return 0;
            default:
                break;   // other opcodes fall through to the normal path
        }
    }

    if (paramIndexedOpcode(op) && idx >= s->numReal) {
        switch (op) {
            case effGetParamName:
                controlName(s, idx, ptr);
                return 1;
            case effGetParamLabel:
                if (ptr) static_cast<char*>(ptr)[0] = '\0';
                return 1;
            case effGetParamDisplay:
                controlDisplay(s, idx, ptr);
                return 1;
            case effCanBeAutomated:
                return 0;
            case effString2Parameter:
                return 0;
            default:
                return 0;
        }
    }

    switch (op) {
        case effOpen: {
            intptr_t r = o->dispatcher(o, op, idx, value, ptr, opt);
            if (!s->opened) {
                s->opened = true;
                captureDefaultsAndInit(s);
                tlog(s, "OPEN numReal=%d uid=%08x dir=%s",
                     s->numReal, (unsigned)s->orig->uniqueID,
                     s->dir[0] ? s->dir : "(none)");
                glog("=== host-sync-dbg v2 open ===");
            }
            return r;
        }
        case effClose: {
            tlog(s, "CLOSE");
            if (s->blinkLive) {
                pthread_mutex_lock(&s->mu);
                s->closing = true;
                pthread_mutex_unlock(&s->mu);
                pthread_join(s->blinkTid, nullptr);
            }
            intptr_t r = o->dispatcher(o, op, idx, value, ptr, opt);
            if (s->logf) { fclose(s->logf); s->logf = nullptr; }
            pthread_mutex_destroy(&s->mu);
            // Release the virtual MIDI port: the last ref of this load frees
            // the ALSA client and unlinks the marker.
            {
                std::lock_guard<std::mutex> lock(gVmidiMutex);
                if (s->midiOut) {
                    const bool ours = gVmidiPort && s->midiOut.get() == gVmidiPort.get();
                    s->midiOut.reset();
                    if (ours && gVmidiPort.use_count() == 1) {
                        gVmidiPort.reset();
                        remove(VMIDI_MARK);
                    }
                }
            }
            delete s;
            return r;
        }
        case effProcessEvents: {
            if (s->eff.flags & effFlagsIsSynth) {
                VstEvents* evs = static_cast<VstEvents*>(ptr);
                if (evs) {
                    if (evs->numEvents > 0 && !g_eventsSeen) {
                        g_eventsSeen = 1;
                        glog("effProcessEvents first non-empty numEvents=%d",
                             (int)evs->numEvents);
                    }
                    for (VstInt32 i = 0; i < evs->numEvents; ++i) {
                        VstEvent* ev = evs->events[i];
                        if (!ev || ev->type != kVstMidiType) continue;
                        VstMidiEvent* me = reinterpret_cast<VstMidiEvent*>(ev);
                        const uint8_t st = me->midiData[0];
                        if (!g_midiSeen[st]) {
                            g_midiSeen[st] = 1;
                            glog("midi in status=0x%02x d1=%d d2=%d first",
                                 st, me->midiData[1], me->midiData[2]);
                        }
                        if      (st == 0xFA) glog("midi START");
                        else if (st == 0xFB) glog("midi CONTINUE");
                        else if (st == 0xFC) glog("midi STOP");
                        else if (st == 0xF2) glog("midi SONG-POSITION");
                        else if (st == 0xF8) { /* clock: counted, not logged */ }
                        if ((me->midiData[0] & 0xF0) == 0xB0 &&
                            me->midiData[1] == CC_PC) {
                            s->prog = normOfSlot(me->midiData[2]);
                            notify(e, s->numReal + kCtlProg, s->prog);
                            tlog(s, "CC slot=%d -> auto-load",
                                 slotOf(s->prog));
                            doLoad(s);
                        }
                    }
                }
            }
            return o->dispatcher(o, op, idx, value, ptr, opt);
        }
        case effSetChunk: {
            intptr_t r = o->dispatcher(o, op, idx, value, ptr, opt);
            for (int i = 0; i < s->numReal; ++i)
                notify(&s->eff, i, realGet(s, i));
            pushRefresh(&s->eff);
            tlog(s, "SETCHUNK forwarded, replayed %d params + idle", s->numReal);
            return r;
        }
        case effSetProgram: {
            // Factory programs (Temper's 5 in-code presets; progExp=0 so
            // they are NOT handled above): JUCE applies the preset via a
            // bare state swap whose redirect pushes audioMasterAutomate,
            // but nothing sends audioMasterIdle and the Force repaints
            // knob widgets only on idle — GUI stays stale (ISSUE-002).
            // Same replay as effSetChunk, after the plugin has applied it.
            intptr_t r = o->dispatcher(o, op, idx, value, ptr, opt);
            for (int i = 0; i < s->numReal; ++i)
                notify(&s->eff, i, realGet(s, i));
            pushRefresh(&s->eff);
            tlog(s, "SETPROGRAM value=%d replayed %d params + idle",
                 (int)value, s->numReal);
            return r;
        }
        default:
            return o->dispatcher(o, op, idx, value, ptr, opt);
    }
}

/// @brief Inner setParameter: momentary triggers + control slots, else the plugin.
void shimSetParam(AEffect* e, int32_t idx, float v) {
    Shim* s = shimOf(e);
    if (idx < s->numReal) {
        tlog(s, "PARAM idx=%d v=%.4f", idx, (double) v);
        if (isMomentaryIndex(idx)) {
            // The Force button is a LATCH that sends 1 / 0 / 1 / ... on each
            // press.  Treat ANY received value as a press: force a clean 0->1
            // edge so the plugin always fires, then queue an ASYNC reset
            // (blinkTick, timer thread) that pushes 0 to plugin + host so the
            // latch re-arms and the next press fires again.
            realSet(s, idx, 0.0f);
            realSet(s, idx, 1.0f);
            queueReset(s, idx);
            tlog(s, "MOM idx=%d recv=%.3f -> trigger + queued reset", idx, (double)v);
            return;
        }
        realSet(s, idx, v);
        return;
    }
    int c = idx - s->numReal;
    if (c >= 0 && c < kNumControls)
        tlog(s, "SET %s idx=%d v=%.3f", kCtlNames[c], idx, v);
    switch (c) {
        case kCtlProg:
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            s->prog = v;
            break;
        case kCtlLoad:
            // Any write triggers: Force toggles 0->1->0->1, so both values
            // are a press. No edge detection, no prev state.
            doLoad(s);
            // Spring the widget back to 0 asynchronously (timer thread) so the
            // Force re-arms -- a synchronous push here does not stick.
            queueReset(s, idx);
            break;
        case kCtlSave:
            doSave(s);
            queueReset(s, idx);
            break;
        case kCtlStatus:
            // Visual-only LED: host/user writes ignored (logged above).
            break;
        default:
            break;
    }
}

/// @brief Inner getParameter: plugin value, or the control slot's value.
float shimGetParam(AEffect* e, int32_t idx) {
    Shim* s = shimOf(e);
    blinkTick(s);
    if (idx < s->numReal)
        return realGet(s, idx);
    int c = idx - s->numReal;
    float r;
    switch (c) {
        case kCtlProg:   r = s->prog; break;
        case kCtlLoad:   r = 0.0f; break;
        case kCtlSave:   r = 0.0f; break;
        case kCtlStatus: r = s->status; break;
        case kCtlClip:   r = s->clip; break;
        default:         r = 0.0f; break;
    }
    if (c >= 0 && c < kNumControls && r != s->lastGet[c]) {
        tlog(s, "GET %s -> %.3f", kCtlNames[c], r);
        s->lastGet[c] = r;
    }
    return r;
}

/// @brief Inner process(): trace summary, then render via the plugin.
void shimProcess(AEffect* e, float** in, float** out, VstInt32 n) {
    Shim* s = shimOf(e);
    maybeSummary();
    s->orig->process(s->orig, in, out, n);
}

/// @brief Inner processReplacing(): trace summary, then render via the plugin.
void shimProcessReplacing(AEffect* e, float** in, float** out, VstInt32 n) {
    Shim* s = shimOf(e);
    maybeSummary();
    s->orig->processReplacing(s->orig, in, out, n);
}

/// @brief Inner processDoubleReplacing(): trace summary, then render via the plugin.
void shimProcessDoubleReplacing(AEffect* e, double** in, double** out,
                                VstInt32 n) {
    Shim* s = shimOf(e);
    maybeSummary();
    s->orig->processDoubleReplacing(s->orig, in, out, n);
}

} // namespace

/// @brief Hook for the inner plugin: blink the Status LED on a momentary trigger.
///
/// Declared weak on the plugin side so a build without the shim still links.
extern "C" PRESET_SHIM_EXPORT void mm303ShimBlinkStatus(void) {
    if (g_shim) blinkOn(g_shim);
}

/// @brief Hook for the inner plugin: light the Clip LED when the output clips.
///
/// Declared weak on the plugin side so a build without the shim still links.
extern "C" PRESET_SHIM_EXPORT void mm303ShimClipStatus(void) {
    if (g_shim) clipOn(g_shim);
}

/// @brief Hook for the inner plugin: the resolved install dir (for "Midi Clips").
///
/// Null if unresolved.
extern "C" PRESET_SHIM_EXPORT const char* mm303ShimPluginDir(void) {
    return (g_shim && g_shim->dir[0]) ? g_shim->dir : nullptr;
}

/// @brief Plugin entry point: wraps the real plugin's AEffect with the shim.
///
/// Builds the +4-control AEffect, starts the Status-LED timer thread and opens
/// the virtual MIDI port for this load group.
extern "C" PRESET_SHIM_EXPORT AEffect* VSTPluginMain(audioMasterCallback host) {
    g_realHost = host;
    AEffect* orig = VSTPluginMainReal(shimHostCb);
    if (!orig) return nullptr;

    Shim* s = new (std::nothrow) Shim();
    if (!s) return orig;
    g_shim = s;
    g_shimEff = &s->eff;
    {
        // Resolve the .so's own directory now so logging works even if the
        // host queries/sets things before effOpen (captureDefaultsAndInit
        // may later refine it to the MPC.settings install dir).
        Dl_info info;
        if (dladdr(reinterpret_cast<void*>(&VSTPluginMain), &info) && info.dli_fname) {
            snprintf(s->dir, sizeof(s->dir), "%s", info.dli_fname);
            char* slash = strrchr(s->dir, '/');
            if (slash) *slash = '\0';
        }
    }
    for (int i = 0; i < kNumControls; ++i) s->lastGet[i] = -1.0f; // log first read

    memcpy(&s->eff, orig, sizeof(AEffect));
    s->orig = orig;
    s->host = host;
    s->numReal = orig->numParams;
    s->eff.numParams = orig->numParams + kNumControls;
#ifndef NOPROGRAMS
    if (orig->numPrograms <= 1) {   // factory-less -> expose the bank
        s->eff.numPrograms = 128;
        s->progExp = true;
    }
#endif
    s->eff.dispatcher = shimDispatch;
    s->eff.setParameter = shimSetParam;
    s->eff.getParameter = shimGetParam;
    pthread_mutex_init(&s->mu, nullptr);
    s->blinkLive = (pthread_create(&s->blinkTid, nullptr, blinkTimer, s) == 0);

    // Virtual MIDI out port: create a uniquely-named port, or JOIN the current
    // load group's port when the TTL marker is still fresh (the host builds 1-2
    // objects per load and the live one may be the second).
    {
        std::lock_guard<std::mutex> lock(gVmidiMutex);
        bool fresh = false;
        struct stat st;
        if (stat(VMIDI_MARK, &st) == 0) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            const long ageMs = (long)(now.tv_sec - st.st_mtim.tv_sec) * 1000L
                             + (long)(now.tv_nsec - st.st_mtim.tv_nsec) / 1000000L;
            fresh = ageMs >= 0 && ageMs <= VMIDI_DUP_TTL_MS;
        }
        if (FILE* mf = fopen(VMIDI_MARK, "w")) fclose(mf);  // refresh for next ctor
        if (fresh && gVmidiPort) {
            s->midiOut = gVmidiPort;
        } else {
            try {
                s->midiOut = std::make_shared<rt::midi::RtMidiOut>();
                s->midiOut->setClientName(MM303_VMIDI_CLIENT);
                s->midiOut->openVirtualPort(vmidiUniqueName(MM303_VMIDI_PORT));
                gVmidiPort = s->midiOut;
            } catch (...) { s->midiOut.reset(); }
        }
    }

    if (orig->process)
        s->eff.process = shimProcess;
    if (orig->processReplacing)
        s->eff.processReplacing = shimProcessReplacing;
    if (orig->processDoubleReplacing)
        s->eff.processDoubleReplacing = shimProcessDoubleReplacing;
    return &s->eff;
}
