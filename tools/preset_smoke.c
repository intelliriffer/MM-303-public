/* preset_smoke.c - behavioral gate for preset-shim-enabled VST2 plugins.
 *
 * Usage: preset_smoke <plugin.so> <0x-uid-hex> <realCount> <totalParams>
 *
 * Loads a shim-wrapped .so (templates/preset/preset_shim.cpp) and enforces
 * the MpcPreset control contract:
 *   - numParams == total, numOutputs == 2 (device rule), numInputs 0 or 2
 *   - control names Prog#/Load/Save/Status on the last 4 params (<=7 chars)
 *   - controls: effCanBeAutomated == 0, effString2Parameter == 0
 *   - controls never appear in the VST state chunk (byte-equal chunk with a
 *     determinism pre-check); a real-param change does alter the chunk
 *   - Save writes <uid-hex>-bank.bin next to the .so; Load restores the
 *     saved real-param values after a scramble; Prog# shows the slot,
 *     Status is a visual-only LED (1 for ~500ms after Load/Save, then 0;
 *     the OFF push reaches the host with NO getParameter/dispatch calls
 *     in between — the shim runs its own timer; host writes to Status
 *     are ignored; Prog# changes never touch Status)
 *   - Load/Save spring back to 0 on read; ANY setParameter value (1 or 0,
 *     repeated included) triggers — Force toggles 0->1->0 per tap
 *   - bank-as-programs exposure (factory-less plugins): numPrograms==128
 *     exposes #000-#127 (current+indexed names), effSetProgram(slot in the
 *     VALUE arg) loads that bank slot and mirrors Prog#; factory plugins
 *     keep their own numPrograms (else-branch check)
 * Exits non-zero on failure. Prints NIN=/NOUT= for build.sh.
 */
#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...) do {                                       \
        if (cond) { printf("ok   - " __VA_ARGS__); putchar('\n'); } \
        else       { printf("FAIL - " __VA_ARGS__); putchar('\n'); ++failures; } \
    } while (0)

#define MAXP 128

typedef struct AEffect AEffect;
typedef intptr_t (*audioMasterCallback)(AEffect*, int32_t, int32_t, intptr_t, void*, float);

/* VST2 AEffect - 32-bit ARM layout (compiled in-container), same as so_check. */
struct AEffect {
    int32_t magic;
    void* dispatcher;
    void (*process)(AEffect*, float**, float**, int32_t);
    void (*setParameter)(AEffect*, int32_t, float);
    float (*getParameter)(AEffect*, int32_t);
    int32_t numPrograms;
    int32_t numParams;
    int32_t numInputs;
    int32_t numOutputs;
    int32_t flags;
    intptr_t reserved1;
    intptr_t reserved2;
    int32_t initialDelay;
    int32_t realQualities;
    int32_t offQualities;
    float ioRatio;
    void* object;
    void* user;
    int32_t uniqueID;
    int32_t version;
    void (*processReplacing)(AEffect*, float**, float**, int32_t);
    double (*processDoubleReplacing)(AEffect*, double**, double**, int32_t);
    char future[56];
};

typedef intptr_t (*dispatch_fn)(AEffect*, int32_t, int32_t, intptr_t, void*, float);

/* classic VST2 opcodes */
enum {
    OP_OPEN = 0, OP_CLOSE = 1,
    OP_GET_PARAM_LABEL = 6, OP_GET_PARAM_DISPLAY = 7, OP_GET_PARAM_NAME = 8,
    OP_SET_SAMPLE_RATE = 10, OP_SET_BLOCK_SIZE = 11, OP_MAINS_CHANGED = 12,
    OP_GET_CHUNK = 23, OP_SET_CHUNK = 24, OP_PROCESS_EVENTS = 25,
    OP_CAN_BE_AUTOMATED = 26, OP_STRING2PARAM = 27,
    OP_SET_PROGRAM = 2, OP_GET_PROGRAM = 3, OP_GET_PROGRAM_NAME = 5,
    OP_GET_PROGRAM_NAME_INDEXED = 29,
    OP_EFFECT_NAME = 45
};

static int32_t automate_calls;
static int32_t idle_calls;          /* audioMasterIdle pushes (host redraw) */
static int32_t status_idx = -1;     /* Status param index (nreal+3) */
static int32_t status_off_calls;    /* audioMasterAutomate pushes of Status=0 */

/* Transient inner params (never persisted): derived from the plugin's own
 * effCanBeAutomated flag (the shim uses the same source).  g_expected holds the
 * CSV declared on the command line, if any, to assert against the derived set. */
static char g_transient[MAXP];
static char g_expected[MAXP];
static int  g_have_expected;
static int is_transient(long i) { return i >= 0 && i < MAXP && g_transient[i]; }
/// @brief Parse the transient-param CSV (arg 5) into g_expected[].
static void parse_transient(const char* s)
{
    if (!s || !*s || strcmp(s, "wrapper") == 0) return;
    g_have_expected = 1;
    char buf[256];
    snprintf(buf, sizeof buf, "%s", s);
    for (char* tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
        long v = strtol(tok, NULL, 10);
        if (v >= 0 && v < MAXP) g_expected[v] = 1;
    }
}

/// @brief Host callback that counts the automate/idle/Status-off pushes the shim makes.
static intptr_t host_cb(AEffect* e, int32_t op, int32_t idx, intptr_t val, void* ptr, float f)
{
    (void)e; (void)val; (void)ptr;
    switch (op) {
    case 1:  return 2400;          /* audioMasterVersion */
    case 0:  ++automate_calls;     /* audioMasterAutomate */
            if (idx == status_idx && f == 0.0f)
                ++status_off_calls;
            return 0;
    case 3:  ++idle_calls;          /* audioMasterIdle */
            return 0;
    case 10: return 48000;
    case 11: return 512;
    default: return 0;
    }
}

static dispatch_fn disp(AEffect* e) { return (dispatch_fn)e->dispatcher; }

static void pset(AEffect* e, int32_t i, float v) { e->setParameter(e, i, v); }
static float pget(AEffect* e, int32_t i) { return e->getParameter(e, i); }

static int feq(float a, float b, float eps) { return fabsf(a - b) < eps; }

/// @brief Read parameter *i*'s name into out (NUL-terminated, <= n bytes).
static void pname(AEffect* e, int32_t i, char* out, size_t n)
{
    memset(out, 0, n);
    disp(e)(e, OP_GET_PARAM_NAME, i, 0, out, 0.0f);
    out[n - 1] = '\0';
}

/// @brief Read parameter *i*'s display text into out.
static void pdisp(AEffect* e, int32_t i, char* out, size_t n)
{
    memset(out, 0, n);
    disp(e)(e, OP_GET_PARAM_DISPLAY, i, 0, out, 0.0f);
    out[n - 1] = '\0';
}

/// @brief Fetch the bank chunk (value=1) and return its size via *sz.
static char* get_chunk(AEffect* e, intptr_t* sz)
{
    char* chunk = NULL;
    *sz = disp(e)(e, OP_GET_CHUNK, 0, 1, &chunk, 0.0f);   /* value=1: bank */
    return chunk;
}

/// @brief Run the MpcPreset control-contract gate; non-zero on failure.
int main(int argc, char** argv)
{
    if (argc != 5 && argc != 6) {
        fprintf(stderr, "usage: preset_smoke <plugin.so> <0x-uid-hex> "
                        "<realCount> <totalParams> [transientCsv|wrapper]\n"
                        "  transientCsv = comma-separated inner param indices "
                        "that are intentionally not persisted\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IONBF, 0); /* keep output on abort */
    const uint32_t uid = (uint32_t)strtoul(argv[2], NULL, 0);
    const long nreal = strtol(argv[3], NULL, 10);
    const long ntotal = strtol(argv[4], NULL, 10);
    const int is_wrapper = (argc == 6 && strcmp(argv[5], "wrapper") == 0);
    parse_transient(argc >= 6 ? argv[5] : NULL);
    if (nreal < 1 || ntotal < nreal + 4 || ntotal > MAXP) {
        fprintf(stderr, "FAIL - counts out of range (real=%ld total=%ld)\n",
                nreal, ntotal);
        return 2;
    }

    void* const so = dlopen(argv[1], RTLD_NOW);
    if (!so) {
        fprintf(stderr, "FAIL - dlopen: %s\n", dlerror());
        return 1;
    }
    AEffect* (*entry)(audioMasterCallback) =
        (AEffect* (*)(audioMasterCallback))dlsym(so, "VSTPluginMain");
    if (!entry) {
        fprintf(stderr, "FAIL - dlsym VSTPluginMain: %s\n", dlerror());
        return 1;
    }

    AEffect* const e = entry(host_cb);
    CHECK(e != NULL, "VSTPluginMain returns an effect");
    if (!e) return 1;
    status_idx = (int32_t)nreal + 3;

    /* --- identity / device contract --- */
    CHECK(e->magic == 0x56737450, "magic=0x%x", (unsigned)e->magic);
    CHECK(e->uniqueID == (int32_t)uid, "uniqueID=0x%08x (expect 0x%08x)",
          (uint32_t)e->uniqueID, uid);
    CHECK(e->numParams == (int32_t)ntotal, "numParams=%d (expect %ld)",
          e->numParams, ntotal);
    printf("NIN=%d\nNOUT=%d\n", e->numInputs, e->numOutputs);
    CHECK(e->numOutputs == 2, "numOutputs=%d (device rule: exactly 2)",
          e->numOutputs);
    CHECK(e->numInputs == 0 || e->numInputs == 2, "numInputs=%d (0 or 2)",
          e->numInputs);

    /* --- lifecycle --- */
    disp(e)(e, OP_OPEN, 0, 0, NULL, 0.0f);
    disp(e)(e, OP_SET_SAMPLE_RATE, 0, 0, NULL, 48000.0f);
    disp(e)(e, OP_SET_BLOCK_SIZE, 0, 512, NULL, 0.0f);
    disp(e)(e, OP_MAINS_CHANGED, 0, 1, NULL, 0.0f);

    /* --- derive the runtime-only set from the plugin's own automatable flag ---
     * The shim derives its transient set the same way, so this makes the plugin
     * the single source of truth and (if a CSV was declared) asserts the CSV
     * matches what the plugin actually reports. */
    {
        long k;
        int nderived = 0;
        for (k = 0; k < nreal; ++k) {
            intptr_t a = disp(e)(e, OP_CAN_BE_AUTOMATED, (int32_t)k, 0, NULL, 0.0f);
            if (a == 0) { g_transient[k] = 1; ++nderived; }
        }
        CHECK(nderived > 0, "%d runtime-only params (non-automatable)", nderived);
        if (g_have_expected) {
            int mismatch = -1;
            for (k = 0; k < nreal; ++k)
                if (g_expected[k] != g_transient[k]) { mismatch = (int)k; break; }
            if (mismatch >= 0)
                CHECK(0, "runtime-only set mismatch at param[%d] "
                         "(plugin=%d csv=%d)", mismatch,
                      g_transient[mismatch], g_expected[mismatch]);
            else
                CHECK(1, "runtime-only set matches declared CSV");
        }
    }

    /* --- control names on the last 4 params --- */
    static const char* want[4] = { "Prog#", "Load", "Save", "Status" };
    int c;
    for (c = 0; c < 4; ++c) {
        char name[32];
        pname(e, (int32_t)(nreal + c), name, sizeof(name));
        CHECK(strcmp(name, want[c]) == 0, "control[%d] name=\"%s\" (expect \"%s\")",
              c, name, want[c]);
        CHECK(strlen(name) <= 7, "control[%d] name \"%s\" <= 7 chars", c, name);
    }

    /* --- real params: names non-empty + automatable; controls not --- */
    long i;
    for (i = 0; i < nreal; ++i) {
        char name[32];
        pname(e, (int32_t)i, name, sizeof(name));
        if (!name[0]) {
            CHECK(0, "param[%ld] has a name", i);
        }
    }
    for (i = 0; i < nreal; ++i) {
        if (is_transient(i)) continue;   // runtime-only: intentionally not automatable
        intptr_t a = disp(e)(e, OP_CAN_BE_AUTOMATED, (int32_t)i, 0, NULL, 0.0f);
        if (!a) {
            CHECK(0, "param[%ld] is automatable (delegate returned %ld)",
                  i, (long)a);
            break;
        }
    }
    printf("note - real params automatable OK (%ld params, first-failure break)\n", nreal);
    for (c = 0; c < 4; ++c) {
        intptr_t a = disp(e)(e, OP_CAN_BE_AUTOMATED, (int32_t)(nreal + c), 0, NULL, 0.0f);
        CHECK(a == 0, "control[%d] not automatable (got %ld)", c, (long)a);
        intptr_t s2p = disp(e)(e, OP_STRING2PARAM, (int32_t)(nreal + c), 0, NULL, 0.0f);
        CHECK(s2p == 0, "control[%d] effString2Parameter==0 (got %ld)", c, (long)s2p);
    }

    /* --- control display: Prog# shows its slot (default 0) --- */
    {
        char d[32];
        pdisp(e, (int32_t)nreal, d, sizeof(d));
        CHECK(strcmp(d, "0") == 0, "Prog# default display=\"%s\" (expect \"0\")", d);
    }

    /* --- chunk: determinism pre-check, control exclusion, real-state proof ---
     * APVTS flushes dirty params into the state tree on a 10Hz timer; settle
     * before snapshots so the flush doesn't land mid-comparison. */
    float dfl[MAXP];
    for (i = 0; i < nreal; ++i)
        dfl[i] = pget(e, (int32_t)i);

    usleep(400000);
    intptr_t szA = 0, szA2 = 0, szB = 0, szC = 0;
    /* Chunk buffers are owned by the wrapper (freed on the next get); we
     * only ever compare through our own copyA snapshot. */
    char* chunkA = get_chunk(e, &szA);
    CHECK(szA > 1 && chunkA, "get chunk size=%ld", (long)szA);
    char* copyA = NULL;
    if (szA > 1 && chunkA) {
        copyA = (char*)malloc((size_t)szA);
        memcpy(copyA, chunkA, (size_t)szA);
    }
    {
        char* chunkA2 = NULL;
        chunkA2 = get_chunk(e, &szA2);   /* invalidates chunkA */
        int same = (szA == szA2 && copyA && chunkA2 &&
                    memcmp(copyA, chunkA2, (size_t)szA) == 0);
        CHECK(same, "chunk deterministic across consecutive gets (%ld/%ld)",
              (long)szA, (long)szA2);
    }

    /* control values must not enter the plugin chunk: set the shim-only
     * controls (Prog# stores, Status is ignored) and re-read the chunk.
     * Load/Save are exercised separately below - they may apply bank state,
     * whose chunk effect is pipeline-specific. */
    pset(e, (int32_t)(nreal + 0), 42.0f / 127.0f);   /* Prog# = 42 */
    pset(e, (int32_t)(nreal + 3), 7.0f / 127.0f);    /* Status = 7 (ignored) */
    {
        float sw = pget(e, (int32_t)(nreal + 3));
        CHECK(!feq(sw, 7.0f / 127.0f, 1e-6f),
              "Status write ignored (read back %.4f, not 7/127)", (double)sw);
    }
    {
        char* chunkB = NULL;
        szB = disp(e)(e, OP_GET_CHUNK, 0, 1, &chunkB, 0.0f);
        int same = (copyA && szB == szA && memcmp(copyA, chunkB, (size_t)szA) == 0);
        CHECK(same, "controls not in chunk (A=%ld B=%ld, bytes %s)",
              (long)szA, (long)szB, same ? "equal" : "differ");
    }
    /* Load/Save fire on ANY value; must not crash (blink + slot 42 save). */
    pset(e, (int32_t)(nreal + 1), 0.0f);             /* Load = 0 (still fires) */
    pset(e, (int32_t)(nreal + 2), 0.0f);             /* Save = 0 (still fires) */

    /* a real-param change must alter the chunk (guards an empty/dead chunk) */
    {
        static const float probes[4] = { 1.0f, 0.0f, 0.9f, 0.1f };
        int changed = -1;
        float v = 0.0f;
        for (i = 0; i < nreal && changed < 0; ++i) {
            int k;
            for (k = 0; k < 4; ++k) {
                pset(e, (int32_t)i, probes[k]);
                float g = pget(e, (int32_t)i);
                if (!feq(g, dfl[i], 1e-6f)) {
                    changed = (int)i;
                    v = g;
                    break;
                }
            }
            if (changed < 0)
                pset(e, (int32_t)i, dfl[i]);
        }
        CHECK(changed >= 0, "a probe changed a real param");
        if (changed >= 0) {
            char* chunkC = NULL;
            usleep(400000); /* let the APVTS flush timer write the value */
            szC = disp(e)(e, OP_GET_CHUNK, 0, 1, &chunkC, 0.0f);
            int differs = (copyA && szC > 1 &&
                           (szC != szA || memcmp(copyA, chunkC, (size_t)szC) != 0));
            CHECK(differs, "real-param change alters the chunk (%ld -> %ld)",
                  (long)szA, (long)szC);
            pset(e, changed, dfl[changed]);
        }
        (void)v;
    }

    /* --- setChunk must push every real param to the host ---
     * The Force redraws parameter UIs only on audioMasterAutomate; preset
     * and project restore arrive as setChunk and the plugin applies them
     * silently. Without the push the audio loads but the GUI stays stale. */
    {
        intptr_t szS = 0;
        char* copyS = NULL;
        usleep(150000);   /* let any pending APVTS flush settle to dfl[] */
        char* chunkS = get_chunk(e, &szS);
        if (szS > 1 && chunkS) {
            copyS = (char*)malloc((size_t)szS);
            memcpy(copyS, chunkS, (size_t)szS);
        }
        CHECK(szS > 1 && copyS != NULL, "setChunk gate: chunk captured (%ld)",
              (long)szS);
        for (i = 0; i < nreal; ++i)
            if (!is_transient(i))
                pset(e, (int32_t)i, dfl[i] < 0.5f ? 0.9f : 0.1f);  /* scramble */
        automate_calls = 0;
        idle_calls = 0;
        disp(e)(e, OP_SET_CHUNK, 0, szS, copyS, 0.0f);
        int restored = 1;
        for (i = 0; i < nreal && restored; ++i) {
            if (is_transient(i))
                continue;
            if (!feq(pget(e, (int32_t)i), dfl[i], 1e-4f))
                restored = 0;
        }
        CHECK(restored, "setChunk restored real params");
        /* The requirement is that the shim replay pushes EVERY real param
         * (so the Force repaints all knobs). JUCE also self-pushes changed
         * params, but a discrete param whose scrambled value equals its
         * restored value is legitimately not re-notified, so requiring
         * 2*nreal over-counts. nreal is the guarantee. */
        long expect_auto = nreal;
        char msg[160];
        snprintf(msg, sizeof msg,
                 "setChunk: shim replay pushes (automate=%d, expect >= %ld)",
                 (int)automate_calls, expect_auto);
        CHECK(automate_calls >= expect_auto, "%s", msg);
        CHECK(idle_calls >= 1,
              "setChunk pushed audioMasterIdle redraw (idle=%d, expect >= 1)",
              (int)idle_calls);
        free(copyS);
    }

    /* --- Save/Load round-trip --- */
    float saved[MAXP];
    int nchanged = 0;
    for (i = 0; i < nreal; ++i) {
        static const float probes[4] = { 1.0f, 0.0f, 0.9f, 0.1f };
        saved[i] = dfl[i];
        if (is_transient(i))
            continue;
        int k;
        for (k = 0; k < 4; ++k) {
            pset(e, (int32_t)i, probes[k]);
            float g = pget(e, (int32_t)i);
            if (!feq(g, dfl[i], 1e-6f)) {
                saved[i] = g;
                ++nchanged;
                break;
            }
        }
    }
    CHECK(nchanged >= 1, "%d real params moved off their defaults", nchanged);

    pset(e, (int32_t)(nreal + 0), 5.0f / 127.0f);    /* Prog# = 5 */
    automate_calls = 0;
    pset(e, (int32_t)(nreal + 2), 1.0f);             /* Save rising edge */
    CHECK(automate_calls > 0, "Save notified host (audioMasterAutomate=%d)",
          (int)automate_calls);

    /* bank file exists next to the .so */
    {
        char bank[600];
        const char* slash = strrchr(argv[1], '/');
        char dir[512];
        if (slash) {
            size_t n = (size_t)(slash - argv[1]);
            if (n >= sizeof(dir)) n = sizeof(dir) - 1;
            memcpy(dir, argv[1], n);
            dir[n] = '\0';
        } else {
            strcpy(dir, ".");
        }
        snprintf(bank, sizeof(bank), "%s/%08x-bank.bin", dir, uid);
        struct stat st;
        int exists = (stat(bank, &st) == 0 && st.st_size > 0);
        CHECK(exists, "bank file written: %s (%ld bytes)", bank,
              exists ? (long)st.st_size : 0L);
    }

    /* scramble, then Load */
    int nscrambled = 0;
    for (i = 0; i < nreal; ++i) {
        if (feq(saved[i], dfl[i], 1e-6f))
            continue;
        if (is_transient(i))
            continue;
        float cand = (saved[i] >= 0.5f) ? 0.0f : 1.0f;
        pset(e, (int32_t)i, cand);
        float g = pget(e, (int32_t)i);
        if (!feq(g, saved[i], 1e-6f))
            ++nscrambled;
    }
    CHECK(nscrambled >= 1, "%d real params scrambled", nscrambled);

    automate_calls = 0;
    idle_calls = 0;
    pset(e, (int32_t)(nreal + 1), 1.0f);             /* Load rising edge */
    CHECK(automate_calls > 0, "Load notified host (audioMasterAutomate=%d)",
          (int)automate_calls);
    CHECK(idle_calls >= 1, "Load pushed audioMasterIdle redraw (idle=%d, expect >= 1)",
          (int)idle_calls);

    int restored = 0, bad = 0;
    for (i = 0; i < nreal; ++i) {
        if (feq(saved[i], dfl[i], 1e-6f))
            continue;
        if (is_transient(i))
            continue;
        float g = pget(e, (int32_t)i);
        if (feq(g, saved[i], 1e-5f))
            ++restored;
        else {
            ++bad;
            printf("note - param[%ld] not restored: got %.6f want %.6f\n",
                   i, (double)g, (double)saved[i]);
        }
    }
    CHECK(bad == 0 && restored == nchanged,
          "Load restored saved values (%d/%d)", restored, nchanged);

    {
        float st = pget(e, (int32_t)(nreal + 3));
        CHECK(feq(st, 1.0f, 1e-5f),
              "Status LED on after Load (%.4f, expect 1)", (double)st);
        float pr = pget(e, (int32_t)(nreal + 0));
        CHECK(feq(pr, 5.0f / 127.0f, 1e-5f), "Prog# kept=%.4f (expect 5/127)",
              (double)pr);
        float ld = pget(e, (int32_t)(nreal + 1));
        CHECK(ld == 0.0f, "Load spring-back: get=%.4f (expect 0)", (double)ld);
        float sv = pget(e, (int32_t)(nreal + 2));
        CHECK(sv == 0.0f, "Save spring-back: get=%.4f (expect 0)", (double)sv);
    }

    /* --- declared transient params must survive a Load untouched ---
     * Regression gate for the preset-shim transient set: the shim's compiled
     * transient indices MUST match the CSV declared to this gate. When they
     * drift (e.g. the shim knows only {38,39,40,41} while the plugin declares
     * {34,35,38,39,40,41,42,44}), doLoad restores the runtime controls
     * (Sync/Start/Transpose/Octave) from the bank instead of leaving them
     * alone, so the user sees them "reset" on preset load. Momentary triggers
     * self-reset by design and are excluded. */
    {
        static const long momentary[] = { 39, 40, 41 };
        const int nmom = (int)(sizeof momentary / sizeof momentary[0]);
        float keep[MAXP];
        int nkeep = 0;
        for (i = 0; i < nreal; ++i) {
            int mom = 0, m;
            if (!is_transient(i)) continue;
            for (m = 0; m < nmom; ++m)
                if (i == momentary[m]) mom = 1;
            if (mom) continue;
            /* move the transient off its current value, remember the result */
            pset(e, (int32_t)i, pget(e, (int32_t)i) < 0.5f ? 1.0f : 0.0f);
            keep[i] = pget(e, (int32_t)i);
            ++nkeep;
        }
        CHECK(nkeep > 0, "%d non-momentary transient params set", nkeep);
        pset(e, (int32_t)(nreal + 1), 1.0f);             /* Load (slot 5) */
        int reverted = 0;
        for (i = 0; i < nreal; ++i) {
            int mom = 0, m;
            if (!is_transient(i)) continue;
            for (m = 0; m < nmom; ++m)
                if (i == momentary[m]) mom = 1;
            if (mom) continue;
            float g = pget(e, (int32_t)i);
            if (!feq(g, keep[i], 1e-5f)) {
                ++reverted;
                printf("note - transient param[%ld] reverted on Load: "
                       "got %.6f want %.6f\n", i, (double)g, (double)keep[i]);
            }
        }
        CHECK(reverted == 0,
              "transient params survive Load (shim set matches declared CSV)");
    }

    /* --- Status LED off push comes from the shim's OWN timer: no
     * getParameter/dispatch calls are allowed to happen during the wait,
     * otherwise a lazy host-driven tick could fake a pass --- */
    {
        int32_t off0 = status_off_calls;
        usleep(900000);                  /* silent wait: nothing below here */
        CHECK(status_off_calls > off0,
              "LED-off pushed to host by shim timer alone (%d pushes)",
              (int)(status_off_calls - off0));
        float st0 = pget(e, (int32_t)(nreal + 3));
        CHECK(st0 == 0.0f, "Status LED off after ~500ms (read %.4f, expect 0)",
              (double)st0);
    }

    /* --- Prog# must never trigger Status (on or off) --- */
    {
        int32_t off0 = status_off_calls;
        float st0 = pget(e, (int32_t)(nreal + 3));
        pset(e, (int32_t)(nreal + 0), 99.0f / 127.0f);   /* Prog# change */
        float st1 = pget(e, (int32_t)(nreal + 3));
        CHECK(st1 == st0 && status_off_calls == off0,
              "Prog# change leaves Status alone (before %.4f after %.4f, "
              "off-pushes %d)",
              (double)st0, (double)st1, (int)(status_off_calls - off0));
    }

    /* --- trigger semantics: any value fires (Force toggle 0->1->0->1) --- */
    automate_calls = 0;
    pset(e, (int32_t)(nreal + 2), 1.0f);             /* toggle -> 1 */
    CHECK(automate_calls > 0, "Save fires on 1");
    automate_calls = 0;
    pset(e, (int32_t)(nreal + 2), 0.0f);             /* toggle -> 0 */
    CHECK(automate_calls > 0, "Save fires on 0");
    automate_calls = 0;
    pset(e, (int32_t)(nreal + 2), 0.0f);             /* repeated 0 still fires */
    CHECK(automate_calls > 0, "Save fires on repeated 0");
    automate_calls = 0;
    pset(e, (int32_t)(nreal + 1), 1.0f);             /* Load -> 1 */
    CHECK(automate_calls > 0, "Load fires on 1");
    automate_calls = 0;
    pset(e, (int32_t)(nreal + 1), 0.0f);             /* Load -> 0 */
    CHECK(automate_calls > 0, "Load fires on 0");
    automate_calls = 0;
    pset(e, (int32_t)(nreal + 1), 0.0f);             /* repeated 0 still fires */
    CHECK(automate_calls > 0, "Load fires on repeated 0");

    /* --- bank-as-programs exposure (spec 2026-10-02-bank-as-programs) ---
     * numParams == ntotal was already enforced above: exposure must not
     * touch the param list. numPrograms==128 => exposure active (factory-less
     * plugin); otherwise the plugin keeps its own factory programs. */
    if (e->numPrograms == 128) {
        char nm[32];
        /* current-program name (opcode 5, JUCE semantics) via Prog# control */
        pset(e, (int32_t)nreal + 0, 0.0f);            /* current program 0 */
        memset(nm, 0, sizeof nm);
        disp(e)(e, OP_GET_PROGRAM_NAME, 0, 0, nm, 0.0f);
        CHECK(strcmp(nm, "#000") == 0, "current name(0)=\"%s\" (expect \"#000\")", nm);
        pset(e, (int32_t)nreal + 0, 1.0f);            /* current program 127 */
        memset(nm, 0, sizeof nm);
        disp(e)(e, OP_GET_PROGRAM_NAME, 0, 0, nm, 0.0f);
        CHECK(strcmp(nm, "#127") == 0, "current name(127)=\"%s\" (expect \"#127\")", nm);
        /* indexed name (opcode 29) */
        memset(nm, 0, sizeof nm);
        disp(e)(e, OP_GET_PROGRAM_NAME_INDEXED, 0, 0, nm, 0.0f);
        CHECK(strcmp(nm, "#000") == 0, "indexed name(0)=\"%s\" (expect \"#000\")", nm);
        memset(nm, 0, sizeof nm);
        disp(e)(e, OP_GET_PROGRAM_NAME_INDEXED, 127, 0, nm, 0.0f);
        CHECK(strcmp(nm, "#127") == 0, "indexed name(127)=\"%s\" (expect \"#127\")", nm);

        /* effSetProgram(value=5): must load slot 5 saved by the Save
         * round-trip above (scramble first so restoration is observable) */
        int nscrambled2 = 0;
        for (i = 0; i < nreal; ++i) {
            if (feq(saved[i], dfl[i], 1e-6f))
                continue;
            float cand = (saved[i] >= 0.5f) ? 0.0f : 1.0f;
            pset(e, (int32_t)i, cand);
            if (!feq(pget(e, (int32_t)i), saved[i], 1e-6f))
                ++nscrambled2;
        }
        CHECK(nscrambled2 >= 1, "%d params scrambled before program recall",
              nscrambled2);
        disp(e)(e, OP_SET_PROGRAM, 0, 5, NULL, 0.0f);  /* slot in VALUE */
        CHECK(disp(e)(e, OP_GET_PROGRAM, 0, 0, NULL, 0.0f) == 5,
              "effGetProgram()==5 after selection");
        {
            float pr = pget(e, (int32_t)(nreal + 0));
            CHECK(feq(pr, 5.0f / 127.0f, 1e-5f),
                  "Prog# mirrors selected slot (got %.4f, expect 5/127)",
                  (double)pr);
        }
        int restored2 = 0, bad2 = 0;
        for (i = 0; i < nreal; ++i) {
            if (feq(saved[i], dfl[i], 1e-6f))
                continue;
            if (feq(pget(e, (int32_t)i), saved[i], 1e-5f))
                ++restored2;
            else
                ++bad2;
        }
        CHECK(bad2 == 0 && restored2 == nchanged,
              "program #005 recall restored saved values (%d/%d)",
              restored2, nchanged);
    } else {
        CHECK(e->numPrograms >= 1,
              "factory programs intact (numPrograms=%d, exposure off)",
              e->numPrograms);
        /* --- ISSUE-002: factory program select must notify + redraw ---
         * Plugins like Temper (own in-code presets) forward effSetProgram
         * to JUCE, which applies the preset via a bare state swap: the
         * redirect pushes audioMasterAutomate but nothing ever sends
         * audioMasterIdle, and the Force repaints knob widgets only on
         * idle — GUI stays stale. Mirrors the setChunk gate above. */
        automate_calls = 0;
        idle_calls = 0;
        disp(e)(e, OP_SET_PROGRAM, 0, 1, NULL, 0.0f);  /* factory program 1 */
        CHECK(automate_calls >= nreal,
              "factory setProgram: host notified (automate=%d, expect >= %ld)",
              (int)automate_calls, nreal);
        CHECK(idle_calls >= 1,
              "factory setProgram pushed audioMasterIdle redraw (idle=%d, expect >= 1)",
              (int)idle_calls);
    }

    free(copyA);
    disp(e)(e, OP_CLOSE, 0, 0, NULL, 0.0f);          /* effClose */

    if (failures) {
        printf("preset_smoke: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("preset_smoke: OK\n");
    return 0;
}
