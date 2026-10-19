/* SPDX-License-Identifier: GPL-3.0-only */
/* FUNCTION ACID 0.1 test: an FM-1 firmware where every track is a function y(s, phi) of the step s and the
 * phase phi in it, drawn on one pair of axes (time across, pitch up) and played as it is drawn. After
 * AA (@measure_plan) "trying to turn math into acid". Built on Dinghy (zvenson), which is built on Felucca
 * (Leo Kuroshita, Hügelton Instruments) and SLOOP (isod89): the platform below is theirs, unchanged.
 * The browser prototype this is ported from: visual-interaction-lab/04-function-acid (engine.js, ui.js).
 *
 *   T1 ACID  16 steps you write (note, accent, slide, tie: the 303's way), or the AA formula (N-step cycle
 *            against 16, octave jumps, slides, chord / scale walks) -> saw/square -> SVF -> a ping-pong delay
 *   T2 BIO   animal calls fitted from real recordings (fly, bee, frog, humpback)
 *   T3 KICK  a pitch spike falling exponentially; DUCK pushes T1/T2 down while it sounds
 *   T4 PERC  noise hat / cricket (4.45 kHz, 3 pulses at 23 Hz) / cicada (40.5 Hz clicks into a resonance)
 *
 * Integers only (no FPU): pitch is Q8 semitones (MIDI x 256), levels Q15, tables in acid_tables.h. */
#define APP_NAME "FN ACID"
#ifndef APP_VER
#define APP_VER "0.31"
#endif
#define CV_MAX (240u * 110u)
#include "platform.c"
#include "acid_tables.h"

#define LANE 32u
#define NONE 255u
enum { T_KICK = 1, T_FOUR = 2, T_UP = 4, T_SLIDE = 8, T_ARP = 16, T_BLIP = 32, T_PERC = 64, T_FILL = 128, T_SCALE = 256, T_BIO = 512 };

/* ---------------------------------------------------------------- parameters (all kept in flash: < 256 bytes) */
typedef struct {
    uint16_t terms;
    uint8_t N, off, dens, swing, scaleM, scaleC, walk, arpSet, arpSpd, blipAmt, blipN;
    int8_t root;
    uint8_t bpm, autoSong, bars, chapter, mutes;
    uint8_t pre[4];
    uint8_t cut, res, env, aWave, aSub; int8_t aTune; uint8_t aLvl, aDrive, aDec, aAcc, aGate, aGlide;
    uint8_t bAnimal, bPer, bPhase, bQuant; int8_t bTune; uint8_t bDrift, bNoise, bBright, bLvl, bArp;
    uint8_t kTop, kTone, kClick, kDrive, kPdec, kBody; int8_t kBase; uint8_t kDuck, kLvl;
    uint8_t hType, hTone, hDec, hPat, hLvl;
    uint8_t lRate, lDep, lShape, lDest, dTime, dFb, dMix, mDrive, dTone, rMix;
    uint8_t laneMode[4], keyLock, magic;
    uint8_t aSrc, sLen; int8_t sTrans;           /* ACID: 0 = its steps (lane 0), 1 = the formula; steps 1..16; transpose */
    uint16_t accM, sldM;                         /* ACID steps: accent, slide into the next step */
    uint8_t lane[4][LANE];                       /* NONE = empty, 0..127 a note, | 128 a tie */
} params_t;
_Static_assert(sizeof(params_t) <= 256u, "the settings page holds 256 bytes");
static params_t P;
#define MAGIC 0xA7
#define STEPS (P.aSrc == 0u)

/* what is not saved: the live keys, the riser, the clock */
static volatile int32_t riseStart = -1;
static volatile uint8_t run;
static uint8_t laneN[4];
static volatile int16_t live[4] = {-1, -1, -1, -1}, liveN[4] = {-1, -1, -1, -1};
static volatile uint8_t recNow;                 /* REC is on: the keys write notes (they don't transpose the steps) */
/* FX held + a white key: punch-in effects while the key is held. These act on ACID's curve itself (y + 12, a mirror,
 * x / 2, 2x, a squelch), the others on the sound (loops, reverse, tape stop, sweeps, crush: see the audio ISR) */
enum { FN_OCT = 1, FN_MIRROR = 2, FN_HALF = 4, FN_DOUBLE = 8, FN_SQUELCH = 16 };
static volatile uint8_t fxFn;
static volatile uint32_t liveS[4], liveE[4];

static const uint8_t ARP_SETS[6][4] = {{3, 7, 10, 12}, {4, 7, 11, 12}, {5, 7, 10, 12}, {12, 12, 24, 12}, {7, 12, 7, 19}, {3, 6, 9, 12}};
static const uint8_t LFO_STEPS[8] = {1, 2, 4, 8, 16, 32, 64, 128};
static const uint8_t BIO_PER[6] = {2, 4, 8, 16, 32, 64};

/* ---------------------------------------------------------------- chapters and presets */
typedef struct { const char *name; uint16_t terms; uint8_t N, off, cut, scaleM, scaleC, walk, arpSet, blipAmt, blipN; } chapter_t;
static const chapter_t CHAPTERS[6] = {
    {"ONE", T_KICK | T_UP | T_SLIDE, 5, 2, 34, 0, 0, 0, 255, 0, 0},
    {"TWO", T_KICK | T_FOUR | T_UP | T_SLIDE | T_ARP, 5, 2, 40, 0, 0, 0, 0, 0, 0},
    {"THREE", T_KICK | T_FOUR | T_UP | T_SLIDE | T_BLIP | T_FILL | T_PERC | T_SCALE | T_BIO, 5, 2, 46, 7, 3, 3, 255, 3, 3},
    {"FOUR", T_KICK | T_FOUR | T_UP | T_ARP | T_PERC | T_SCALE, 7, 2, 52, 7, 3, 3, 0, 0, 0},
    {"FIVE", T_KICK | T_FOUR | T_SLIDE | T_BLIP | T_PERC | T_SCALE | T_BIO, 5, 2, 58, 5, 3, 3, 255, 5, 3},
    {"SIX", T_KICK | T_FOUR | T_UP | T_SLIDE | T_ARP | T_BLIP | T_FILL | T_PERC | T_SCALE | T_BIO, 5, 2, 66, 5, 3, 3, 0, 3, 3},
};
static void apply_chapter(uint32_t k)
{
    const chapter_t *c = &CHAPTERS[k % 6u];
    P.aSrc = 1; P.chapter = (uint8_t)(k % 6u); P.terms = c->terms; P.N = c->N; P.off = c->off; P.cut = c->cut;
    if (c->scaleM) { P.scaleM = c->scaleM; P.scaleC = c->scaleC; P.walk = c->walk; }
    if (c->arpSet != 255) P.arpSet = c->arpSet;
    if (c->blipAmt) { P.blipAmt = c->blipAmt; P.blipN = c->blipN; }
}
typedef struct { const char *name; uint8_t v[12]; } preset_t;
/* ACID: aWave aSub aTune aLvl res env aDrive aDec aAcc aGate aGlide */
static const preset_t PRE_ACID[8] = {
    {"TB CLASSIC", {0, 0, 0, 100, 88, 72, 30, 60, 70, 70, 0}},   {"SQUARE 303", {1, 0, 0, 92, 80, 64, 40, 50, 60, 65, 20}},
    {"SQUELCH AA", {0, 0, 0, 96, 104, 110, 50, 30, 80, 75, 0}},  {"RUBBER", {0, 90, 0, 100, 50, 46, 20, 90, 40, 85, 50}},
    {"SCREAM", {0, 0, 0, 82, 112, 100, 110, 40, 100, 60, 10}},   {"DEEP SUB", {1, 110, 0, 108, 40, 30, 10, 70, 30, 80, 30}},
    {"GLASS", {2, 0, 1, 86, 96, 90, 20, 20, 60, 40, 0}},         {"DIRTY S+Q", {3, 40, 0, 90, 70, 80, 90, 55, 70, 70, 15}}};
/* BIO: bAnimal bPer bPhase bQuant bTune bDrift bNoise bBright bLvl */
static const preset_t PRE_BIO[6] = {
    {"HOUSEFLY", {0, 3, 0, 0, 0, 32, 30, 90, 60}},  {"BEE", {1, 1, 0, 0, 0, 32, 20, 100, 54}},
    {"FROG", {2, 1, 2, 0, 0, 32, 5, 50, 127}},      {"WHALE", {3, 4, 0, 0, 0, 32, 6, 30, 120}},
    {"FROG SINGS", {2, 2, 0, 127, 0, 110, 5, 60, 120}}, {"WHALE SONG", {3, 4, 0, 127, 12, 127, 6, 40, 110}}};
/* KICK: kTop kTone kClick kDrive kPdec kBody kBase kDuck */
static const preset_t PRE_KICK[6] = {
    {"CLEAN", {36, 0, 20, 20, 38, 60, 0, 80}},     {"909", {30, 0, 70, 70, 50, 55, 2, 70}},
    {"808 LONG", {20, 0, 0, 10, 20, 110, 254, 100}}, {"ZAP", {48, 20, 40, 40, 90, 40, 0, 60}},
    {"AA RAW", {25, 127, 0, 30, 38, 60, 12, 127}},  {"THUD", {12, 0, 10, 50, 30, 70, 251, 90}}};
/* PERC: hType hTone hDec hPat hLvl */
static const preset_t PRE_PERC[6] = {
    {"CLOSED HAT", {0, 90, 30, 0, 64}}, {"OPEN HAT", {0, 80, 92, 0, 56}}, {"SHAKER", {0, 70, 26, 1, 52}},
    {"CRICKET", {1, 64, 40, 4, 60}},    {"CICADA", {2, 64, 64, 5, 54}},   {"TICK 3", {0, 112, 12, 2, 66}}};
static const uint8_t NPRE[4] = {8, 6, 6, 6};
static const char *pre_name(uint32_t t, uint32_t k)
{
    return t == 0 ? PRE_ACID[k].name : t == 1 ? PRE_BIO[k].name : t == 2 ? PRE_KICK[k].name : PRE_PERC[k].name;
}
static void apply_preset(uint32_t t, uint32_t k)
{
    const uint8_t *v;
    k %= NPRE[t]; P.pre[t] = (uint8_t)k;
    if (t == 0) { v = PRE_ACID[k].v; P.aWave = v[0]; P.aSub = v[1]; P.aTune = (int8_t)v[2]; P.aLvl = v[3]; P.res = v[4]; P.env = v[5]; P.aDrive = v[6]; P.aDec = v[7]; P.aAcc = v[8]; P.aGate = v[9]; P.aGlide = v[10]; }
    else if (t == 1) { v = PRE_BIO[k].v; P.bAnimal = v[0]; P.bPer = v[1]; P.bPhase = v[2]; P.bQuant = v[3]; P.bTune = (int8_t)v[4]; P.bDrift = v[5]; P.bNoise = v[6]; P.bBright = v[7]; P.bLvl = v[8]; P.terms |= T_BIO; }
    else if (t == 2) { v = PRE_KICK[k].v; P.kTop = v[0]; P.kTone = v[1]; P.kClick = v[2]; P.kDrive = v[3]; P.kPdec = v[4]; P.kBody = v[5]; P.kBase = (int8_t)v[6]; P.kDuck = v[7]; P.terms |= T_KICK; }
    else { v = PRE_PERC[k].v; P.hType = v[0]; P.hTone = v[1]; P.hDec = v[2]; P.hPat = v[3]; P.hLvl = v[4]; P.terms |= T_PERC; }
}
static void defaults(void)
{
    uint32_t t, i;
    memset(&P, 0, sizeof P);
    P.N = 5; P.off = 2; P.dens = 127; P.scaleM = 7; P.scaleC = 3; P.walk = 3; P.blipAmt = 3; P.blipN = 3;
    P.root = 33; P.bpm = 120; P.autoSong = 0; P.bars = 8;
    P.kLvl = 110; P.bArp = 1; P.lRate = 4; P.mDrive = 20;
    P.dTime = 2; P.dFb = 54; P.dMix = 60; P.dTone = 64;       /* the film's echo: dotted eighth, 42 %, darker each time */
    P.rMix = 40;                                               /* and its small room */
    P.laneMode[2] = P.laneMode[3] = 1; P.keyLock = 1; P.magic = MAGIC;
    for (t = 0; t < 4u; t++) for (i = 0; i < LANE; i++) P.lane[t][i] = NONE;
    for (t = 0; t < 4u; t++) apply_preset(t, 0);
    apply_chapter(0);
    /* the start: a blank page that still pulses. ACID plays its root on all 16 steps (yours to write over),
     * a four-to-the-floor kick and an off-beat hat; no animal, no song mode */
    P.aSrc = 0; P.sLen = 16; P.sTrans = 0; P.accM = P.sldM = 0;
    for (i = 0; i < 16u; i++) P.lane[0][i] = (uint8_t)P.root;
    P.terms = (uint16_t)((P.terms | T_FOUR | T_PERC) & ~T_BIO);
    P.cut = 40;
}
/* starting points for the steps (HOME held: white keys 9, 10): [semitones above the root, accent, slide, rest] */
static const int8_t FILM_PAT[16][4] = {     /* the acid film's line (function-sounds/video/acid-film.html) */
    {0, 1, 0, 0}, {0, 0, 0, 0}, {12, 0, 1, 0}, {0, 0, 0, 0}, {3, 0, 0, 0}, {0, 0, 0, 1}, {12, 1, 0, 0}, {10, 0, 1, 0},
    {0, 0, 0, 0}, {0, 1, 0, 0}, {7, 0, 0, 0}, {12, 0, 1, 0}, {12, 0, 0, 0}, {3, 0, 0, 1}, {15, 1, 0, 0}, {0, 0, 0, 0}};
static void load_steps(int film)
{
    uint32_t i;
    P.aSrc = 0; P.sLen = 16; P.sTrans = 0; P.accM = P.sldM = 0; P.autoSong = 0;
    for (i = 0; i < LANE; i++) P.lane[0][i] = NONE;
    for (i = 0; i < 16u; i++) {
        if (!film) { P.lane[0][i] = (uint8_t)P.root; continue; }
        if (!FILM_PAT[i][3]) P.lane[0][i] = (uint8_t)(P.root + FILM_PAT[i][0]);
        if (FILM_PAT[i][1]) P.accM |= (uint16_t)(1u << i);
        if (FILM_PAT[i][2]) P.sldM |= (uint16_t)(1u << i);
    }
    if (film) {   /* the film's drop (acid-film.html, 0:48-0:56): 120 BPM, saw, cutoff ~400 Hz, reso high, env ~2 octaves,
                   * decay 280 ms, gate half a step, accent strong; echo 3/16 at 42 %; kick ducks the acid 30 %; 16th hats */
        P.bpm = 120; P.aWave = 0; P.aSub = 0; P.aTune = 0; P.aLvl = 100; P.cut = 80; P.res = 100; P.env = 68; P.aDec = 18;
        P.aAcc = 110; P.aGate = 50; P.aGlide = 0; P.aDrive = 30;
        P.dTime = 2; P.dFb = 54; P.dMix = 52; P.dTone = 64; P.rMix = 40;
        P.kDuck = 40; P.hPat = 1; P.terms |= T_KICK | T_FOUR | T_PERC; P.terms &= (uint16_t)~T_BIO; P.mutes = 0;
    }
}
static void count_lanes(void)
{
    uint32_t t, i;
    for (t = 0; t < 4u; t++) {
        laneN[t] = 0;
        for (i = 0; i < LANE; i++) if (P.lane[t][i] != NONE && !(P.lane[t][i] & 128u)) laneN[t]++;
    }
}

/* ---------------------------------------------------------------- small maths */
static int32_t expneg(int32_t xq8)               /* e^-x, x Q8 -> Q15 */
{
    int32_t i, fr;
    if (xq8 <= 0) return 32767;
    i = xq8 >> 3;
    if (i >= 256) return 0;
    fr = xq8 & 7;
    return (EXPT[i] * (8 - fr) + EXPT[i + 1] * fr) >> 3;
}
static inline int32_t sinu(uint32_t u16) { return SINT[(u16 >> 6) & 1023u]; }
static uint32_t hash32(uint32_t x)
{
    x *= 0x9E3779B1u; x ^= x >> 15; x *= 0x85EBCA6Bu; x ^= x >> 13;
    return x;
}
static int32_t lfo_at(uint32_t s, uint32_t p16)
{
    uint32_t per = LFO_STEPS[P.lRate & 7u], ph = (((s & (per - 1u)) << 16) + p16) / per;
    switch (P.lShape) {
    case 0: return sinu(ph);
    case 1: return ph < 32768u ? (int32_t)ph * 2 - 32768 : 98303 - (int32_t)ph * 2;
    case 2: return ph < 32768u ? 32767 : -32767;
    default: return (int32_t)(hash32(s / per + 7777u) & 65535u) - 32768;
    }
}
static int32_t snap_q8(int32_t y)                 /* the nearest note of the scale (natural minor when SCALE is off) */
{
    int32_t m = (P.terms & T_SCALE) ? P.scaleM : 7, C = (P.terms & T_SCALE) ? P.scaleC : 2;
    int32_t d = y - (P.root << 8), o = d >= 0 ? d / 3072 : -((-d + 3071) / 3072), oo, k, best = 0, be = 1 << 30;
    for (oo = o - 1; oo <= o + 1; oo++)
        for (k = 0; k < m; k++) {
            int32_t c = (oo * 12 + (12 * k + C) / m) << 8, e = c > d ? c - d : d - c;
            if (e < be) { be = e; best = c; }
        }
    return (P.root << 8) + best;
}

/* ---------------------------------------------------------------- the curves: y(s, phi) */
typedef struct { int32_t y; uint32_t age; uint8_t acc, slide, rest, gate; } acid_t;   /* age: since the attack, Q16 steps */
typedef struct { int32_t y, amp; uint32_t kt; uint8_t on; } voice_t;
typedef struct { uint8_t ok, live; uint32_t age, len; int32_t note; } trig_t;

static int trig(uint32_t t, uint32_t s, uint32_t p16, uint32_t back, trig_t *o)   /* the latest note: lane or key */
{
    uint32_t k, pos = (s << 16) + p16;
    o->ok = 0; o->live = 0;
    if (laneN[t])
        for (k = 0; k <= back; k++) {
            uint32_t v = P.lane[t][(s - k) & (LANE - 1u)];
            if (v != NONE && !(v & 128u)) {
                uint32_t len = 1;
                while (len < LANE) { uint32_t w = P.lane[t][(s - k + len) & (LANE - 1u)]; if (w != NONE && (w & 128u)) len++; else break; }
                o->ok = 1; o->age = (k << 16) + p16; o->note = (int32_t)v; o->len = len << 16;
                break;
            }
        }
    if (liveN[t] >= 0) {
        uint32_t a = pos - liveS[t];
        if (a < ((back + 1u) << 16) && (!o->ok || a < o->age)) {
            o->ok = 1; o->live = 1; o->age = a; o->note = liveN[t];
            o->len = live[t] >= 0 ? 0x7FFFFFFFu : (liveE[t] - liveS[t] > 16384u ? liveE[t] - liveS[t] : 16384u);
        }
    }
    return o->ok;
}

/* ---- ACID's steps, the 303's way: a note, a tie (holds the note before), a rest; an accent; a slide into the next
 * step (no new attack, the pitch glides there in about 60 ms, the gate stays open) */
static uint32_t step_len(void) { return P.sLen >= 1u && P.sLen <= 16u ? P.sLen : 16u; }
static int legato_into(uint32_t i, uint32_t L)     /* step i goes on from the step before it: no new attack */
{
    uint32_t v = P.lane[0][i], pi = (i + L - 1u) % L, pv = P.lane[0][pi];
    if (v == NONE || pv == NONE || L < 2u) return 0;
    return (v & 128u) || (P.sldM >> pi & 1u);
}
static int32_t step_note(uint32_t i, uint32_t L)   /* the pitch step i sounds (a tie or a rest: the last note) */
{
    uint32_t k;
    for (k = 0; k < L; k++) { uint32_t v = P.lane[0][(i + L - k) % L]; if (v != NONE && !(v & 128u)) return (int32_t)v; }
    return P.root;
}
static int32_t acid_mods(uint32_t s, uint32_t p16)  /* the LFO on pitch and the riser */
{
    int32_t y = 0;
    if (P.lDep && P.lDest == 1u) y += ((lfo_at(s, p16) * P.lDep) >> 15) * 3072 / 127;
    if (riseStart >= 0 && (int32_t)(s - (uint32_t)riseStart) >= 0) {
        uint32_t d = s - (uint32_t)riseStart; if (d > 16u) d = 16u;
        int32_t r = (int32_t)((((d << 16) + p16) * 384u) >> 16);
        y += r < (24 << 8) ? r : (24 << 8);
    }
    return y;
}
static void eval_steps(uint32_t s, uint32_t p16, acid_t *o)
{
    uint32_t L = step_len(), i = s % L, k, v = P.lane[0][i], nv = P.lane[0][(i + 1u) % L];
    int32_t tr = P.sTrans + P.aTune * 12, n;
    o->acc = o->slide = o->rest = 0; o->gate = 0; o->age = 0x7FFFFFFFu;
    if (!run) {                                      /* stopped: only a key sounds (trying notes, or a step being set) */
        if (live[0] >= 0) { o->y = (live[0] + tr) << 8; o->gate = 1; o->age = (s << 16) + p16 - liveS[0]; }
        else { o->y = (P.root + tr) << 8; o->rest = 1; }
        o->y += acid_mods(s, p16);
        return;
    }
    if (live[0] >= 0 && !recNow) tr += live[0] - P.root;     /* a key held while it plays: transpose, as on a 303 */
    n = step_note(i, L);
    o->y = (n + tr) << 8;
    if (v == NONE) { o->rest = 1; o->y += acid_mods(s, p16); return; }
    for (k = 0; k + 1u < L && legato_into((i + L - k) % L, L); k++) { }
    o->age = (k << 16) + p16;
    o->acc = (uint8_t)(P.accM >> ((i + L - k) % L) & 1u);
    if (!(v & 128u) && legato_into(i, L)) {          /* slid into: glide from the note before (RC, tau 22 ms + GLIDE) */
        int32_t pn = step_note((i + L - 1u) % L, L), tau = 22 + (P.aGlide >> 1);
        uint32_t x = p16 * (15000u / P.bpm) / (256u * (uint32_t)tau);
        o->y += (((pn - n) << 8) * expneg((int32_t)(x > 2047u ? 2047u : x))) >> 15;
    }
    o->slide = (uint8_t)((P.sldM >> i & 1u) && nv != NONE);
    o->gate = o->slide || (nv != NONE && (nv & 128u) && L > 1u) || p16 < P.aGate * 655u;
    o->y += acid_mods(s, p16);
}

static void eval_acid0(uint32_t s, uint32_t p16, acid_t *o, int noglide)
{
    uint32_t t = P.terms, N = P.N, i = (s + P.off) % N, c = ((s + 32u) / N) >> P.arpSpd;
    int32_t base = P.root, y, laneRest = 0, legato = 0;
    trig_t tg;
    if (STEPS) { eval_steps(s, p16, o); return; }
    if (live[0] >= 0 && (P.laneMode[0] == 1 || !run)) { base = live[0]; legato = 1; }
    else if (liveN[0] >= 0 && P.laneMode[0] == 0 && run && !laneN[0]) base = liveN[0];   /* FOLLOW: a key = transpose */
    else if (!run) laneRest = 1;
    else if (P.laneMode[0] == 0 && laneN[0]) { if (trig(0, s, p16, 31, &tg)) base = tg.note & 127; }
    else if (laneN[0]) {
        uint32_t v = P.lane[0][s & (LANE - 1u)], nx = P.lane[0][(s + 1u) & (LANE - 1u)];
        if (v == NONE) laneRest = 1; else { base = (int32_t)(v & 127u); legato = nx != NONE && (nx & 128u); }
    }
    y = (base + P.aTune * 12) << 8;
    o->acc = o->slide = o->rest = 0;
    if ((t & T_SCALE) && (i & 1u)) {
        uint32_t m = P.scaleM, k = (P.walk * c + i + m - 1u) % m;
        y += (int32_t)((12u * k + P.scaleC) / m) << 8;
    }
    if ((t & T_UP) && (i == 2u || (N >= 8u && i == N - 2u))) { y += ((t & T_ARP) ? ARP_SETS[P.arpSet % 6u][c & 3u] : 12) << 8; o->acc = 1; }
    if ((t & T_SLIDE) && ((N > 4u && i == 4u) || (N >= 10u && i == N - 1u))) { y -= (2 * (32767 - expneg((int32_t)(p16 >> 7))) * 256) >> 15; o->slide = 1; }
    if ((t & T_BLIP) && P.blipAmt && P.blipN && s % P.blipN == 0u) y += (P.blipAmt * expneg((int32_t)((p16 * 2304u) >> 16)) * 256) >> 15;
    if (P.lDep && P.lDest == 1u) y += ((lfo_at(s, p16) * P.lDep) >> 15) * 3072 / 127;
    if (laneRest || (P.dens < 127u && !o->acc && !legato && (hash32(s) & 127u) >= P.dens)) o->rest = 1;
    if (P.aGlide && !noglide && !o->slide) {
        acid_t pr;
        eval_acid0(s - 1u, 65535u, &pr, 1);
        if (!pr.rest) { int32_t k = 2 + ((127 - P.aGlide) >> 3); y = pr.y + (((y - pr.y) * (32767 - expneg((int32_t)((p16 * (uint32_t)k) >> 8)))) >> 15); }
    }
    if (riseStart >= 0 && (int32_t)(s - (uint32_t)riseStart) >= 0) {
        uint32_t d = s - (uint32_t)riseStart; if (d > 16u) d = 16u;
        int32_t r = (int32_t)((((d << 16) + p16) * 384u) >> 16);
        y += r < (24 << 8) ? r : (24 << 8);
    }
    o->y = y; o->age = p16;
    o->gate = o->rest ? 0 : (o->slide || legato || p16 < P.aGate * 655u);
}
static void eval_acid(uint32_t s, uint32_t p16, acid_t *o, int noglide)   /* with the punch-ins that change the curve */
{
    uint32_t f = fxFn;
    if (f & FN_HALF) { p16 = (((s & 1u) << 16) | p16) >> 1; s >>= 1; }                     /* x / 2 */
    else if (f & FN_DOUBLE) { uint32_t q = p16 << 1; s = (s << 1) + (q >> 16); p16 = q & 65535u; }   /* 2x */
    eval_acid0(s, p16, o, noglide);
    if (f & FN_MIRROR) {                                                                    /* y -> 2c - y, around the root's octave */
        o->y = ((P.root + 12) << 9) - o->y;
        if (P.keyLock) o->y = snap_q8(o->y);
    }
    if (f & FN_OCT) o->y += 12 << 8;                                                        /* y + 12 */
}
static uint32_t tau_ms(uint32_t dec)             /* the steps' DECAY: 200 ms .. 2 s, as on a 303 (log) */
{
    static const uint16_t TAU[17] = {200, 233, 272, 316, 369, 430, 501, 584, 681, 794, 925, 1078, 1257, 1465, 1707, 1990, 2000};
    uint32_t i = dec >> 3, f = dec & 7u;
    return (TAU[i] * (8u - f) + TAU[i + 1u] * f) >> 3;
}
static int32_t cutoff_at(const acid_t *o, uint32_t s, uint32_t p16)
{
    int32_t base = (24 + ((P.cut * 72) >> 7)) << 8, e, amt;
    if (P.lDep && P.lDest == 0) base += ((lfo_at(s, p16) * P.lDep) >> 15) * 6144 / 127;
    {   /* the filter envelope runs from the attack (over ties and slides); on the steps an accent closes fast, as on a 303 */
        uint32_t a = o->age > (24u << 16) ? (24u << 16) : o->age, x;
        if (STEPS) {                                     /* e^(-t / tau): kk = 64 / tau in steps; an accent: 200 ms */
            uint32_t kk = 960000u / ((o->acc ? 200u : tau_ms(P.aDec)) * P.bpm);
            x = (a * (kk ? kk : 1u)) >> 14;
        } else x = (a * (2u + ((127u - P.aDec) >> 4))) >> 8;
        e = expneg((int32_t)(x > 4095u ? 4095u : x));
    }
    amt = ((P.env * 48) >> 7) + (o->acc ? (P.aAcc * 16) >> 7 : 0);
    if (fxFn & FN_SQUELCH) { base += 10 << 8; amt += 14; }
    return base + ((amt * e * 256) >> 15);
}

static void eval_kick(uint32_t s, uint32_t p16, voice_t *o)
{
    uint32_t t = P.terms, bar = s & 15u, kt = 0xFFFFFFFFu;
    int32_t note = 57, drop;
    trig_t tg;
    o->on = 0; o->amp = 0; o->kt = 0; o->y = P.root << 8;
    if (!(t & T_KICK)) return;
    if (run && !laneN[2]) {
        if ((t & T_FILL) && bar >= 14u) kt = p16;
        else if (t & T_FOUR) kt = ((s & 3u) << 16) | p16;
        else kt = (bar << 16) | p16;
    }
    if ((laneN[2] || liveN[2] >= 0) && trig(2, s, p16, 15, &tg) && (run || tg.live))
        if (kt == 0xFFFFFFFFu || tg.age < kt) { kt = tg.age; note = tg.note & 127; }
    if (kt == 0xFFFFFFFFu) return;
    drop = expneg((int32_t)(((kt >> 8) * (384u + P.kPdec * 20u)) >> 8));
    o->y = ((P.root + P.kBase + note - 57) << 8) + ((P.kTop * 256 * drop) >> 15);
    o->amp = expneg((int32_t)(((kt >> 8) * (40u + (127u - P.kBody) * 3u)) >> 8));
    o->kt = kt; o->on = o->amp > 600;
}

static void eval_bio(uint32_t s, uint32_t p16, voice_t *o)
{
    uint32_t a = P.bAnimal & 3u, per = BIO_PER[P.bPer % 6u], drone = a < 2u;
    int32_t u = -1, center = 0, gateEnd = 32767, follow = -1, off = 0, amp = 0, y;
    trig_t tg;
    o->on = 0; o->amp = 0; o->y = 0; o->kt = 0;
    if (!(P.terms & T_BIO)) return;
    if (P.laneMode[1] == 0 && run && (laneN[1] || liveN[1] >= 0)) {          /* FOLLOW: only the root */
        if (trig(1, s, p16, 31, &tg)) follow = tg.note & 127;
        else if (laneN[1]) return;
    } else if (laneN[1] || liveN[1] >= 0 || !run) {
        if (trig(1, s, p16, 31, &tg)) {
            uint32_t el = tg.age;
            if (drone) {
                u = (int32_t)((el % (per << 16)) / per);
                gateEnd = el < tg.len ? 32767 : (int32_t)(el - tg.len) * 2 < 32767 ? 32767 - (int32_t)(el - tg.len) * 2 : 0;
                if (gateEnd <= 0) u = -1;
            } else if (el < (per << 16)) u = (int32_t)(el / per);
            center = ((tg.note & 127) + BIO_OCT[a] + P.bTune) << 8;
        }
        if ((laneN[1] || !run) && u < 0) return;
    }
    if (u < 0) {                                  /* calls on its own clock (with FOLLOW: at the key's pitch) */
        uint32_t sp = s + 1024u - P.bPhase;
        u = (int32_t)((((sp & (per - 1u)) << 16) + p16) / per);
        center = follow >= 0 ? (follow + BIO_OCT[a] + P.bTune) << 8 : snap_q8(BIO_CENTER_Q8[a] + (P.bTune << 8));
        if (P.bArp) {
            uint32_t ci = sp / per, k;
            static const uint8_t UD[6] = {0, 1, 2, 3, 2, 1};
            k = P.bArp == 1u ? (ci & 3u) : P.bArp == 2u ? UD[ci % 6u] : (hash32(ci + 99u) & 3u);
            if (k) center += ARP_SETS[P.arpSet % 6u][k - 1u] << 8;
        }
    }
    switch (a) {
    case 0: off = (sinu((uint32_t)u) * 128) >> 15; amp = 26214 + ((sinu(p16) * 6553) >> 15); break;          /* fly */
    case 1: off = (sinu((uint32_t)u) * 64) >> 15; amp = 30000; break;                                          /* bee */
    case 2: {                                                                                                   /* frog */
        int32_t lin = u * 5 / 2, att = u * 42, dec = expneg((u * 1920) >> 16);
        off = -(((lin < 65536 ? lin : 65536) * 474) >> 16);
        amp = u < 31457 ? ((att < 32767 ? att : 32767) * dec) >> 15 : 0;
        break;
    }
    default: {                                                                                                  /* whale */
        int32_t tq = (u - 7646) & 65535;
        if (tq < 26214) {
            int32_t bend = tq - 18568;
            amp = sinu((uint32_t)(tq * 32768 / 26214));
            off = bend > 0 ? -(bend * 461 / 7646) : 0;
        }
    }
    }
    y = center + ((off * P.bDrift) >> 5);
    if (P.bQuant) y += ((snap_q8(y) - y) * P.bQuant) >> 7;
    o->y = y; o->amp = (amp * gateEnd) >> 15; o->on = o->amp > 900;
}

static uint32_t perc_on(uint32_t s)
{
    if (!(P.terms & T_PERC) || !run) return 0;
    if (laneN[3]) { uint32_t v = P.lane[3][s & (LANE - 1u)]; return v != NONE && !(v & 128u) ? 2u : 0u; }
    switch (P.hPat) {
    case 0: return (s & 3u) == 2u ? 2u : 0u;
    case 1: return (s & 3u) == 2u ? 2u : 1u;
    case 2: return s % 3u == 0u ? 2u : 0u;
    case 3: return (s & 3u) == 2u ? 2u : ((s & 15u) >= 12u && (s & 1u)) ? 1u : 0u;
    case 4: return (s & 7u) == 4u ? 2u : 0u;
    default: return (s & 15u) == 0u ? 2u : 0u;
    }
}

/* ---------------------------------------------------------------- the sound (audio ISR) */
#define DMASK 32767u
static int16_t dbufL[DMASK + 1u] __attribute__((section(".pool"))), dbufR[DMASK + 1u] __attribute__((section(".pool")));
static struct {
    volatile uint32_t s, stepPh;
    volatile uint8_t playing, chBar, startReq, stopReq;
    uint32_t sinc, ctr;
    uint32_t aPh, aSub, aInc; int32_t aAmp, aAmpT, lp, bp, f, damp, aDrv; uint8_t acc, kick_;
    uint32_t bPh, bInc; int32_t bAmp, bAmpT, bLp, bK;
    uint32_t kPh, kInc; int32_t kAmp, kAmpT, kClk, kLp, kDrv, duck;
    uint32_t nz; int32_t hEnv, hDk, hA, hy, hx;
    int32_t cT, cBurst, cF, cLp, cBp; uint32_t cPh, cInc, cPulse, cCnt, cN;
    uint32_t didx, dlen; int32_t dlp, dk;
} E = {.nz = 22222u, .cT = -1};

enum { PB_NONE, PB_L4, PB_L8, PB_L16, PB_L32, PB_REV, PB_STOP };
enum { PF_LP = 1, PF_HP = 2, PF_CRUSH = 4 };
/* a small room after the delay (Freeverb's tunings: 4 combs with damping, 2 all-passes; mono), ~1.6 s */
static const uint16_t RV_LEN[6] = {1116, 1188, 1277, 1356, 556, 441};
static int16_t rvBuf[1116 + 1188 + 1277 + 1356 + 556 + 441] __attribute__((section(".pool")));
static struct { uint16_t idx[6], off[6]; int32_t lp[4]; uint8_t ready; } RV;
static inline int32_t clip16(int32_t x) { return x > 32767 ? 32767 : x < -32767 ? -32767 : x; }   /* a feedback path: gain 1 */
static int32_t reverb(int32_t in)
{
    int32_t acc = 0, c;
    if (!RV.ready) { uint32_t o = 0; for (c = 0; c < 6; c++) { RV.off[c] = (uint16_t)o; o += RV_LEN[c]; } RV.ready = 1; }
    for (c = 0; c < 4; c++) {
        int16_t *b = rvBuf + RV.off[c];
        int32_t y = b[RV.idx[c]];
        RV.lp[c] += ((y - RV.lp[c]) * 19660) >> 15;                         /* damping: the highs die first */
        b[RV.idx[c]] = (int16_t)clip16(in + ((RV.lp[c] * 29040) >> 15));        /* g 0.886: about 1.6 s */
        if (++RV.idx[c] >= RV_LEN[c]) RV.idx[c] = 0;
        acc += y;
    }
    acc >>= 2;
    for (c = 4; c < 6; c++) {
        int16_t *b = rvBuf + RV.off[c];
        int32_t y = b[RV.idx[c]], o = y - acc;
        b[RV.idx[c]] = (int16_t)clip16(acc + (y >> 1));
        if (++RV.idx[c] >= RV_LEN[c]) RV.idx[c] = 0;
        acc = o;
    }
    return acc;
}
#define CMASK 32767u
static int16_t capL[CMASK + 1u] __attribute__((section(".pool"))), capR[CMASK + 1u] __attribute__((section(".pool")));
static struct {
    volatile uint8_t req, flags;               /* from the keys: the buffer effect wanted (0 = none), the filter / crush */
    uint8_t mode, frozen, pflags;
    uint32_t ci, anchor, t, len, pos, rate, dec;
    int32_t xf, lpL, bpL, lpR, bpR, sw, swT, hc, hl, hr, crushG;
    uint32_t lpT, hpT;
} X;
static void punch_start(void)                    /* on the grid: the next 16th */
{
    uint32_t stepS = 661500u / P.bpm;
    X.mode = X.req; X.t = 0; X.anchor = X.ci; X.frozen = 0;
    switch (X.mode) {
    case PB_L4: X.len = stepS * 4u; break;
    case PB_L8: X.len = stepS * 2u; break;
    case PB_L16: X.len = stepS; break;
    case PB_L32: X.len = stepS / 2u; break;
    case PB_REV: X.len = stepS * 4u; X.frozen = 1; break;
    default: X.pos = X.ci << 16; X.rate = 65536u; X.dec = 65536u / (stepS * 6u) + 1u; X.len = 1;
    }
    if (X.len > CMASK) X.len = CMASK;
}
static inline int32_t edge_fade(uint32_t pos, uint32_t len)   /* no clicks where a loop joins: 96 samples in and out */
{
    uint32_t e = pos < len - pos ? pos : len - pos;
    return e >= 96u ? 32767 : (int32_t)(e * 341u);
}

static uint32_t inc_q8(int32_t y)
{
    int32_t n = y >> 2, oct, r;
    if (n < 0) n = 0;
    oct = n / 768; r = n - oct * 768;
    if (oct > 10) { oct = 10; r = 767; }
    return INC64[r] >> (10 - oct);
}
static inline int32_t noise(void) { uint32_t z = E.nz; z ^= z << 13; z ^= z >> 17; z ^= z << 5; E.nz = z; return (int32_t)z >> 16; }
static inline int32_t sat(int32_t x)
{
    int32_t x3;
    if (x > 32767) x = 32767; else if (x < -32767) x = -32767;
    x3 = (((x * x) >> 15) * x) >> 15;
    return (3 * x - x3) >> 1;
}
static void perc_hit(uint32_t h)
{
    if (!h || (P.mutes & 8u)) return;
    if (P.hType == 0) E.hEnv = h == 2u ? 26000 : 13000;
    else if (P.hType == 1) { E.cT = 0; E.cPulse = 0; E.cN = 0; E.cBurst = h == 2u ? 32767 : 16000; }
    else { E.cT = 0; E.cCnt = 0; E.cBurst = 1 + (int32_t)(P.hDec >> 4) * (int32_t)(661500u / P.bpm); }
}
static void on_step(void)
{
    if (E.playing && (E.s & 15u) == 0u && E.s > 0u && P.autoSong && !STEPS) {
        E.chBar++;
        if (E.chBar == P.bars - 1u) riseStart = (int32_t)E.s;
        if (E.chBar >= P.bars) { E.chBar = 0; riseStart = -1; apply_chapter(P.chapter + 1u); }
    }
    if (E.playing) perc_hit(perc_on(E.s));
    if (X.req && X.req != X.mode) punch_start();
}
static void control(void)
{
    uint32_t s = E.s, p16 = E.stepPh >> 16, base;
    acid_t oA; voice_t oB, oK;
    int32_t fcq;
    eval_acid(s, p16, &oA, 0); eval_bio(s, p16, &oB); eval_kick(s, p16, &oK);
    E.kInc = inc_q8(oK.y);
    E.kAmpT = (P.mutes & 4u) ? 0 : (oK.amp * P.kLvl) >> 7;
    if (oK.on && oK.kt < 1200u) E.kClk = P.kClick * 200;
    E.kDrv = 12 + (P.kDrive >> 2);
    E.duck = 32767 - ((((P.mutes & 4u) ? 0 : oK.amp) * P.kDuck) >> 7);
    E.aInc = inc_q8(oA.y);
    E.aAmpT = (!oA.gate || (P.mutes & 1u)) ? 0 : (((oA.acc ? 24000 + P.aAcc * 60 : 24000) * P.aLvl) >> 7);
    E.aAmpT = (E.aAmpT * E.duck) >> 15;
    fcq = (int32_t)(inc_q8(cutoff_at(&oA, s, p16)) / 20861u);
    E.f = fcq < 23000 ? fcq : 23000;
    {
        int32_t res = (fxFn & FN_SQUELCH) && P.res < 112u ? 112 : P.res;
        E.damp = 32768 - ((res * 31500) >> 7);               /* Q14: 2.0 .. 0.08 */
    }
    E.aDrv = 12 + (P.aDrive >> 2);
    E.bInc = inc_q8(oB.y);
    E.bAmpT = (P.mutes & 2u) ? 0 : ((((oB.amp * P.bLvl) >> 7) * E.duck) >> 15);
    E.bK = 400 + P.bBright * 180;
    base = P.bpm * 6494u;                                    /* 2^32 * 4 / (60 * 44100) per BPM */
    E.sinc = base * 100u / (100u + ((s & 1u) ? 0u - P.swing : P.swing));
    {
        uint32_t stepS = 661500u / P.bpm;                    /* frames per step */
        E.dlen = stepS * (P.dTime + 1u); if (E.dlen > DMASK) E.dlen = DMASK;
        E.dk = 1500 + P.dTone * 150;                         /* the low-pass in the feedback: ~330 Hz .. ~5 kHz */
    }
    E.hDk = 1 + (((127 - P.hDec) * 28) >> 7);
    E.hA = 20000 + P.hTone * 100;
    E.cInc = inc_q8(108 * 256 + 77 + (P.hTone - 64) * 32 + 246 - (int32_t)((246u * (E.cPulse >> 16)) >> 17));
    fcq = (int32_t)(inc_q8(115 * 256 + (P.hTone - 64) * 26) / 20861u);
    E.cF = fcq < 30000 ? fcq : 30000;
}

static volatile uint32_t cpu_us, cpu_max;
static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, t0 = fm1_ticks();
    if (E.startReq) { E.startReq = 0; E.s = 0; E.stepPh = 0; E.chBar = 0; E.playing = 1; run = 1; if (P.autoSong) { apply_chapter(0); riseStart = -1; } control(); }
    if (E.stopReq) { E.stopReq = 0; E.playing = 0; run = 0; riseStart = -1; }
    for (i = 0; i < n; i++) {
        int32_t mix = 0, wet = 0, dl, dr, md, l, r, rv = 0;
        uint32_t ns;
        if ((E.ctr++ & 15u) == 0u) control();
        ns = E.stepPh + E.sinc;
        if (ns < E.stepPh) { E.stepPh = ns; E.s++; on_step(); control(); } else E.stepPh = ns;

        /* T3 KICK: a sine (TONE pushes it towards a filtered saw) and a click */
        E.kPh += E.kInc;
        E.kAmp += (E.kAmpT - E.kAmp) >> 5;
        if (E.kAmp > 8) {
            int32_t sine = SINT[E.kPh >> 22], saw = (int32_t)(E.kPh >> 16) - 32768, v;
            E.kLp += ((saw - E.kLp) * 9000) >> 15;
            v = (sine * (127 - P.kTone) + E.kLp * P.kTone) >> 7;
            if (E.kClk > 20) { v += (noise() * E.kClk) >> 15; E.kClk -= E.kClk >> 7; }
            mix += (sat((v * E.kDrv) >> 4) * E.kAmp) >> 15;
        }
        /* T1 ACID: a wave and a sub -> Chamberlin SVF */
        E.aPh += E.aInc; E.aSub += E.aInc >> 1;
        E.aAmp += (E.aAmpT - E.aAmp) >> 6;
        {
            int32_t saw = (int32_t)(E.aPh >> 16) - 32768, sq = E.aPh < 0x80000000u ? 26000 : -26000, src, hp;
            src = P.aWave == 0 ? saw : P.aWave == 1 ? sq : P.aWave == 2 ? (E.aPh < 0x40000000u ? 26000 : -26000) : (saw + sq) >> 1;
            if (P.aSub) src = (src * (127 - (P.aSub >> 1)) + (E.aSub < 0x80000000u ? 26000 : -26000) * (P.aSub >> 1)) >> 7;
            E.lp += (E.f * E.bp) >> 15;
            hp = (src >> 1) - E.lp - ((E.damp * E.bp) >> 14);
            if (hp > 65535) hp = 65535; else if (hp < -65535) hp = -65535;
            E.bp += (E.f * hp) >> 15;
            if (E.bp > 60000) E.bp = 60000; else if (E.bp < -60000) E.bp = -60000;
            if (E.lp > 60000) E.lp = 60000; else if (E.lp < -60000) E.lp = -60000;
            if (E.aAmp > 8) { int32_t a = (sat((E.lp * E.aDrv) >> 4) * E.aAmp) >> 15; mix += a; wet += a; }
        }
        /* T2 BIO: the animal's wavetable, a little noise, a one-pole low-pass */
        E.bPh += E.bInc;
        E.bAmp += (E.bAmpT - E.bAmp) >> 7;
        if (E.bAmp > 8) {
            int32_t wt = BIO_WT[(P.bAnimal & 3u) * 256u + (E.bPh >> 24)], v = (wt * (127 - P.bNoise) + noise() * P.bNoise) >> 7;
            E.bLp += ((v - E.bLp) * E.bK) >> 15;
            int32_t bo = (((E.bLp * E.bAmp) >> 15) * 9) >> 4;
            mix += bo; wet += bo;
        }
        /* T4 PERC */
        if (P.hType == 0) {
            if (E.hEnv > 40) {
                int32_t nx = noise(), dlt = E.hy + nx - E.hx;
                E.hy = ((E.hA >> 1) * dlt) >> 14; E.hx = nx;
                mix += (((E.hy * E.hEnv) >> 15) * P.hLvl * 41) >> 14;
                E.hEnv -= (E.hEnv * E.hDk) >> 14;
            }
        } else if (P.hType == 1 && E.cT >= 0) {     /* cricket: 2..5 pulses at 23 Hz */
            uint32_t np = E.cPulse + 2240019u, pf;
            if (np < E.cPulse) E.cN++;
            E.cPulse = np;
            if (E.cN >= 2u + (P.hDec >> 5)) E.cT = -1;
            else {
                int32_t a2;
                pf = E.cPulse >> 16;
                a2 = pf < 45875u ? SINT[pf * 512u / 45875u] : 0;
                E.cPh += E.cInc;
                mix += (((((SINT[E.cPh >> 22] * a2) >> 15) * E.cBurst) >> 15) * P.hLvl * 94) >> 16;
                E.cT++;
            }
        } else if (P.hType == 2 && E.cT >= 0) {     /* cicada: 40.5 clicks a second into a 6.8 kHz resonance */
            if (E.cT >= E.cBurst) E.cT = -1;
            else {
                int32_t ex = (E.cCnt++ % 1089u) == 0u ? 30000 : (noise() >> 4), hp, edge, e2;
                E.cLp += (E.cF * E.cBp) >> 15;
                hp = ex - E.cLp - ((2600 * E.cBp) >> 15);
                if (hp > 65535) hp = 65535; else if (hp < -65535) hp = -65535;
                E.cBp += (E.cF * hp) >> 15;
                if (E.cBp > 60000) E.cBp = 60000; else if (E.cBp < -60000) E.cBp = -60000;
                edge = E.cT * 36; e2 = (E.cBurst - E.cT) * 22;
                if (edge > e2) edge = e2;
                if (edge > 32767) edge = 32767;
                mix += (((E.cBp * edge) >> 15) * P.hLvl * 262) >> 16;
                E.cT++;
            }
        }
        /* the delay, as in the acid film: ACID and BIO only (the drums stay dry), dotted eighth by default; the
         * echoes go left, right, left..., each one through a low-pass, so each is darker and further away */
        dl = dbufL[(E.didx - E.dlen) & DMASK]; dr = dbufR[(E.didx - E.dlen) & DMASK];
        E.dlp += ((dr - E.dlp) * E.dk) >> 15;
        dbufL[E.didx] = (int16_t)clip16(((wet * P.dMix) >> 8) + ((E.dlp * P.dFb) >> 7));   /* not sat(): it has gain 1.5 */
        dbufR[E.didx] = (int16_t)((dl * 29) >> 5);
        E.didx = (E.didx + 1u) & DMASK;
        md = 1 + (P.mDrive >> 5);
        if (P.rMix) rv = (reverb((wet >> 3) + ((dl + dr) >> 3)) * P.rMix) >> 7;   /* the acid, the animal and their echoes */
        l = sat((mix + ((dl * 13) >> 4) + rv) * md); r = sat((mix + ((dr * 13) >> 4) + rv) * md);
        /* punch-ins (FX held + a key): the whole mix */
        if (!X.frozen) { capL[X.ci] = (int16_t)l; capR[X.ci] = (int16_t)r; }
        X.ci = (X.ci + 1u) & CMASK;
        if (X.req && !X.mode && !E.playing) punch_start();                 /* stopped: no grid to wait for */
        if (X.mode) {
            int32_t bl = 0, br = 0, g = 32767, tgt = X.req == X.mode ? 32767 : 0;
            uint32_t p;
            if (X.mode <= PB_L32) {
                p = X.t % X.len;
                if (X.t >= X.len) X.frozen = 1;
                bl = capL[(X.anchor + p) & CMASK]; br = capR[(X.anchor + p) & CMASK]; g = edge_fade(p, X.len);
            } else if (X.mode == PB_REV) {
                p = X.t % X.len;
                bl = capL[(X.anchor - 1u - p) & CMASK]; br = capR[(X.anchor - 1u - p) & CMASK]; g = edge_fade(p, X.len);
            } else if (X.rate) {                                               /* tape stop: the reader slows to nothing */
                uint32_t a = (X.pos >> 16) & CMASK, b = (a + 1u) & CMASK, fr = (X.pos >> 1) & 32767u;
                bl = capL[a] + (((capL[b] - capL[a]) * (int32_t)fr) >> 15); br = capR[a] + (((capR[b] - capR[a]) * (int32_t)fr) >> 15);
                X.pos += X.rate; X.rate = X.rate > X.dec ? X.rate - X.dec : 0u;
            }
            X.t++;
            bl = (bl * g) >> 15; br = (br * g) >> 15;
            X.xf += (tgt - X.xf) >> 7;
            l += ((bl - l) * X.xf) >> 15; r += ((br - r) * X.xf) >> 15;
            if (!tgt && X.xf < 64) { X.mode = PB_NONE; X.frozen = 0; X.xf = 0; }
        }
        if (X.flags & (PF_LP | PF_HP)) {                                   /* sweeps over a bar: LP down, HP up */
            uint32_t bar = (661500u / P.bpm) * 16u, q8, fq;
            int32_t f, h;
            if (!(X.pflags & PF_LP) && (X.flags & PF_LP)) X.lpT = 0;
            if (!(X.pflags & PF_HP) && (X.flags & PF_HP)) X.hpT = 0;
            if (X.flags & PF_LP) { uint32_t u = X.lpT < bar ? X.lpT : bar; q8 = 124u * 256u - u * (72u * 256u) / bar; X.lpT++; }
            else { uint32_t u = X.hpT < bar ? X.hpT : bar; q8 = 20u * 256u + u * (76u * 256u) / bar; X.hpT++; }
            fq = inc_q8((int32_t)q8) / 20861u; f = fq < 23000u ? (int32_t)fq : 23000;
            X.lpL += (f * X.bpL) >> 15; h = l - X.lpL - ((13107 * X.bpL) >> 14);
            if (h > 65535) h = 65535; else if (h < -65535) h = -65535;
            X.bpL += (f * h) >> 15; if (X.bpL > 60000) X.bpL = 60000; else if (X.bpL < -60000) X.bpL = -60000;
            l = (X.flags & PF_LP) ? X.lpL : h;
            X.lpR += (f * X.bpR) >> 15; h = r - X.lpR - ((13107 * X.bpR) >> 14);
            if (h > 65535) h = 65535; else if (h < -65535) h = -65535;
            X.bpR += (f * h) >> 15; if (X.bpR > 60000) X.bpR = 60000; else if (X.bpR < -60000) X.bpR = -60000;
            r = (X.flags & PF_LP) ? X.lpR : h;
            l = sat(l); r = sat(r);
        } else { X.lpL = X.bpL = X.lpR = X.bpR = 0; }
        if (X.flags & PF_CRUSH) {                                          /* 5 bits, a sixth of the sample rate */
            if (++X.hc >= 6) { X.hc = 0; X.hl = l & ~2047; X.hr = r & ~2047; }
            l = X.hl; r = X.hr;
        }
        X.pflags = X.flags;
        out[2u * i] += (l * 7) >> 3; out[2u * i + 1u] += (r * 7) >> 3;
    }
    cpu_us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
    if (cpu_us > cpu_max) cpu_max = cpu_us;
}

/* ================================================================ the UI (main loop) */
#define INK RGB(29, 29, 27)
#define LIME RGB(196, 228, 90)
#define PAPER RGB(206, 234, 112)
#define REDC RGB(216, 32, 74)
static const uint16_t TCOL[4] = {RGB(29, 29, 27), RGB(176, 16, 58), RGB(29, 63, 174), RGB(122, 74, 0)};
static const char *const TRK[4] = {"ACID", "BIO", "KICK", "PERC"};
static uint16_t mixc(uint16_t a, uint16_t b, uint32_t t)   /* a towards b, t 0..16 */
{
    uint32_t r = ((a >> 11) * (16u - t) + (b >> 11) * t) >> 4, g = (((a >> 5) & 63u) * (16u - t) + ((b >> 5) & 63u) * t) >> 4,
             bl = ((a & 31u) * (16u - t) + (b & 31u) * t) >> 4;
    return (uint16_t)(r << 11 | g << 5 | bl);
}
/* text on any background (gfx.c's cv_text blends onto black) */
static int32_t tx(int32_t x, int32_t y, const felucca_font_t *f, const char *s, uint16_t fg, uint16_t bg)
{
    uint16_t ramp[16];
    uint32_t a;
    for (a = 0; a < 16u; a++) ramp[a] = mixc(bg, fg, a + (a == 15u));
    for (; *s; s++) {
        uint32_t gi = glyph(f, (uint8_t)*s), gx, gy, w = f->bw[gi], bpr = (w + 1u) / 2u;
        const uint8_t *gd = f->data + f->off[gi];
        for (gy = 0; gy < f->h; gy++)
            for (gx = 0; gx < w; gx++) {
                uint32_t v = gd[gy * bpr + gx / 2u];
                v = (gx & 1u) ? (v & 15u) : (v >> 4);
                if (v) cv_pset(x - f->pad + (int32_t)gx, y + (int32_t)gy, ramp[v]);
            }
        x += f->adv[gi];
    }
    return x;
}
static int32_t t5(int32_t x, int32_t y, const char *s, uint16_t c)       /* the 5x7 font, capitals */
{
    for (; *s; s++) {
        uint32_t ch = (uint8_t)*s, cx, cy;
        if (ch >= 'a' && ch <= 'z') ch -= 32u;
        if (ch < 32u || ch > 95u) ch = '?';
        for (cx = 0; cx < 5u; cx++) {
            uint32_t col = FONT5[(ch - 32u) * 5u + cx];
            for (cy = 0; cy < 7u; cy++) if (col >> cy & 1u) cv_pset(x + (int32_t)cx, y + (int32_t)cy, c);
        }
        x += 6;
    }
    return x;
}
static uint32_t slen(const char *s) { uint32_t n = 0; while (s[n]) n++; return n; }
static void scat(char *d, const char *s) { while (*d) d++; while (*s) *d++ = *s++; *d = 0; }
static void sint(char *d, int32_t v)
{
    char b[12]; int32_t i = 0, neg = v < 0;
    if (neg) v = -v;
    do { b[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (*d) d++;
    if (neg) *d++ = '-';
    while (i) *d++ = b[--i];
    *d = 0;
}
static const char *const NOTE[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void snote(char *d, int32_t m) { scat(d, NOTE[((m % 12) + 12) % 12]); sint(d, m / 12 - 1); }

/* ---- parameters the knobs can reach */
enum { F_NUM, F_LIST, F_ST, F_PCT, F_NOTE, F_OCT, F_PLUS, F_STEP };
typedef struct { const char *lab; uint16_t off; int16_t min, max; uint8_t sgn, fmt, part; const char *const *list; } pm_t;
#define O(f) ((uint16_t)__builtin_offsetof(params_t, f))
static const char *const L_WAVE[] = {"SAW", "SQUARE", "PULSE", "SAW+SQ"};
static const char *const L_ANIMAL[] = {"FLY", "BEE", "FROG", "WHALE"};
static const char *const L_ARP[] = {"OFF", "UP", "UP/DN", "RANDOM"};
static const char *const L_HTYPE[] = {"NOISE", "CRICKET", "CICADA"};
static const char *const L_HPAT[] = {"8TH", "16TH", "3-STEP", "GALLOP", "HALF", "BAR"};
static const char *const L_LRATE[] = {"1/16", "1/8", "1/4", "1/2", "1BAR", "2BAR", "4BAR", "8BAR"};
static const char *const L_SHAPE[] = {"SINE", "TRI", "SQUARE", "S&H"};
static const char *const L_DEST[] = {"CUTOFF", "PITCH"};
static const char *const L_DTIME[] = {"1/16", "1/8", "3/16", "1/4"};
static const char *const L_MODE7[] = {"LOCRIAN", "PHRYG", "AEOLIAN", "DORIAN", "MIXOLYD", "IONIAN", "LYDIAN"};
static const char *const L_CHORD[] = {"M7", "MAJ7", "SUS", "OCT", "5TH", "DIM"};
static const char *const L_ONOFF[] = {"OFF", "ON"};
static const char *const L_BPER[] = {"2 STEP", "4 STEP", "8 STEP", "16 STEP", "32 STEP", "64 STEP"};
static const char *const L_SRC[] = {"STEPS", "FORMULA"};
enum { PM_CUT, PM_RES, PM_ENV, PM_ADEC, PM_AWAVE, PM_ASUB, PM_ATUNE, PM_ALVL, PM_ADRIVE, PM_AACC, PM_AGATE, PM_AGLIDE,
       PM_BANIMAL, PM_BQUANT, PM_BARP, PM_BLVL, PM_BDRIFT, PM_BNOISE, PM_BBRIGHT, PM_BTUNE, PM_BPER, PM_BPHASE,
       PM_KTOP, PM_KTONE, PM_KCLICK, PM_KDRIVE, PM_KPDEC, PM_KBODY, PM_KBASE, PM_KDUCK,
       PM_HTYPE, PM_HTONE, PM_HPAT, PM_HLVL, PM_HDEC,
       PM_N, PM_OFF, PM_DENS, PM_SWING, PM_LRATE, PM_LDEP, PM_LSHAPE, PM_LDEST, PM_DTIME, PM_DFB, PM_DMIX, PM_MDRIVE,
       PM_SCALEC, PM_WALK, PM_ROOT, PM_ARPSET, PM_BLIPAMT, PM_BLIPN, PM_BPM, PM_AUTO, PM_BARS, PM_KLVL,
       PM_DTONE, PM_SLEN, PM_ASRC, PM_STRANS, PM_RMIX, PM_NONE };
static const pm_t PM[] = {
    {"CUTOFF", O(cut), 0, 127, 0, F_NUM, 0, 0}, {"RESO", O(res), 0, 120, 0, F_NUM, 0, 0}, {"ENV AMT", O(env), 0, 127, 0, F_NUM, 0, 0},
    {"DECAY", O(aDec), 0, 127, 0, F_NUM, 0, 0}, {"WAVE", O(aWave), 0, 3, 0, F_LIST, 0, L_WAVE}, {"SUB", O(aSub), 0, 127, 0, F_NUM, 0, 0},
    {"OCTAVE", O(aTune), -2, 1, 1, F_OCT, 0, 0}, {"ACID LVL", O(aLvl), 0, 127, 0, F_NUM, 0, 0}, {"DRIVE", O(aDrive), 0, 127, 0, F_NUM, 0, 0},
    {"ACCENT", O(aAcc), 0, 127, 0, F_NUM, 0, 0}, {"GATE", O(aGate), 10, 100, 0, F_PCT, 0, 0}, {"GLIDE", O(aGlide), 0, 127, 0, F_NUM, 0, 0},
    {"ANIMAL", O(bAnimal), 0, 3, 0, F_LIST, 1, L_ANIMAL}, {"QUANTIZE", O(bQuant), 0, 127, 0, F_NUM, 1, 0}, {"ARP", O(bArp), 0, 3, 0, F_LIST, 1, L_ARP},
    {"BIO LVL", O(bLvl), 0, 127, 0, F_NUM, 1, 0}, {"CONTOUR", O(bDrift), 0, 127, 0, F_NUM, 1, 0}, {"NOISE", O(bNoise), 0, 127, 0, F_NUM, 1, 0},
    {"BRIGHT", O(bBright), 0, 127, 0, F_NUM, 1, 0}, {"TUNE", O(bTune), -24, 24, 1, F_ST, 1, 0}, {"CALL", O(bPer), 0, 5, 0, F_LIST, 1, L_BPER},
    {"PHASE", O(bPhase), 0, 15, 0, F_PLUS, 1, 0},
    {"PITCH", O(kTop), 0, 48, 0, F_ST, 2, 0}, {"TONE", O(kTone), 0, 127, 0, F_NUM, 2, 0}, {"CLICK", O(kClick), 0, 127, 0, F_NUM, 2, 0},
    {"DRIVE", O(kDrive), 0, 127, 0, F_NUM, 2, 0}, {"P.DECAY", O(kPdec), 0, 127, 0, F_NUM, 2, 0}, {"BODY", O(kBody), 0, 127, 0, F_NUM, 2, 0},
    {"TUNE", O(kBase), -12, 24, 1, F_ST, 2, 0}, {"DUCK", O(kDuck), 0, 127, 0, F_NUM, 2, 0},
    {"SOURCE", O(hType), 0, 2, 0, F_LIST, 3, L_HTYPE}, {"TONE", O(hTone), 0, 127, 0, F_NUM, 3, 0}, {"PATTERN", O(hPat), 0, 5, 0, F_LIST, 3, L_HPAT},
    {"PERC LVL", O(hLvl), 0, 127, 0, F_NUM, 3, 0}, {"DECAY", O(hDec), 0, 127, 0, F_NUM, 3, 0},
    {"N", O(N), 3, 16, 0, F_NUM, 9, 0}, {"OFFSET", O(off), 0, 15, 0, F_NUM, 9, 0}, {"DENSITY", O(dens), 0, 127, 0, F_NUM, 9, 0},
    {"SWING", O(swing), 0, 40, 0, F_PCT, 9, 0}, {"RATE", O(lRate), 0, 7, 0, F_LIST, 9, L_LRATE}, {"DEPTH", O(lDep), 0, 127, 0, F_NUM, 9, 0},
    {"SHAPE", O(lShape), 0, 3, 0, F_LIST, 9, L_SHAPE}, {"DEST", O(lDest), 0, 1, 0, F_LIST, 9, L_DEST}, {"TIME", O(dTime), 0, 3, 0, F_LIST, 9, L_DTIME},
    {"FEEDBACK", O(dFb), 0, 110, 0, F_NUM, 9, 0}, {"MIX", O(dMix), 0, 127, 0, F_NUM, 9, 0}, {"MASTER DRV", O(mDrive), 0, 127, 0, F_NUM, 9, 0},
    {"MODE", O(scaleC), 0, 6, 0, F_LIST, 9, L_MODE7}, {"WALK", O(walk), 1, 6, 0, F_NUM, 9, 0}, {"ROOT", O(root), 21, 57, 1, F_NOTE, 9, 0},
    {"CHORD", O(arpSet), 0, 5, 0, F_LIST, 9, L_CHORD}, {"BLIP", O(blipAmt), 0, 12, 0, F_ST, 9, 0}, {"BLIP EVERY", O(blipN), 2, 8, 0, F_NUM, 9, 0},
    {"TEMPO", O(bpm), 80, 170, 0, F_NUM, 9, 0}, {"AUTO SONG", O(autoSong), 0, 1, 0, F_LIST, 9, L_ONOFF}, {"BARS/CH", O(bars), 2, 16, 0, F_NUM, 9, 0},
    {"KICK LVL", O(kLvl), 0, 127, 0, F_NUM, 2, 0},
    {"TONE", O(dTone), 0, 127, 0, F_NUM, 9, 0}, {"LENGTH", O(sLen), 1, 16, 0, F_NUM, 9, 0}, {"PLAYS", O(aSrc), 0, 1, 0, F_LIST, 9, L_SRC},
    {"TRANSPOSE", O(sTrans), -12, 12, 1, F_ST, 9, 0}, {"REVERB", O(rMix), 0, 127, 0, F_NUM, 9, 0},
};
static int32_t pget(uint32_t k) { const uint8_t *b = (const uint8_t *)&P + PM[k].off; return PM[k].sgn ? (int32_t)(int8_t)*b : (int32_t)*b; }
static void pset(uint32_t k, int32_t v)
{
    int32_t mx = PM[k].max;
    if (k == PM_OFF) mx = P.N - 1;
    if (k == PM_SCALEC && P.scaleM == 5) mx = 4;
    if (v < PM[k].min) v = PM[k].min;
    if (v > mx) v = mx;
    *((uint8_t *)&P + PM[k].off) = (uint8_t)v;
    if (k == PM_N && P.off >= P.N) P.off = P.N - 1u;
}
static void pfmt(uint32_t k, char *s)
{
    int32_t v = pget(k);
    s[0] = 0;
    if (k == PM_ADEC && STEPS) { sint(s, (int32_t)tau_ms((uint32_t)v)); scat(s, "MS"); return; }
    switch (PM[k].fmt) {
    case F_LIST: scat(s, PM[k].list[v]); break;
    case F_ST: if (v > 0) scat(s, "+"); sint(s, v); scat(s, "ST"); break;
    case F_PCT: sint(s, v); scat(s, "%"); break;
    case F_NOTE: snote(s, v); break;
    case F_OCT: if (v > 0) scat(s, "+"); sint(s, v); break;
    case F_PLUS: scat(s, "+"); sint(s, v); break;
    default: sint(s, v);
    }
}

/* ---- pages: 4 cells = KNOB1..4 */
enum { G_HOME, G_EDIT, G_ENV, G_LFO, G_FX, G_SEL, G_ARP, G_SEQ, G_GLO, G_N };
typedef struct { const char *name; uint8_t k[4]; } page_t;
static const page_t PG_HOME = {"HOME", {PM_CUT, PM_RES, PM_ENV, PM_ADEC}};
static const page_t PG_EDIT[4][2] = {
    {{"OSC", {PM_AWAVE, PM_ASUB, PM_ATUNE, PM_ALVL}}, {"FILTER", {PM_CUT, PM_RES, PM_ENV, PM_ADRIVE}}},
    {{"ANIMAL", {PM_BANIMAL, PM_BQUANT, PM_BARP, PM_BLVL}}, {"TIMBRE", {PM_BDRIFT, PM_BNOISE, PM_BBRIGHT, PM_BTUNE}}},
    {{"TONE", {PM_KTOP, PM_KTONE, PM_KCLICK, PM_KDRIVE}}, {"LEVEL", {PM_KLVL, PM_KDUCK, PM_NONE, PM_NONE}}},
    {{"SOURCE", {PM_HTYPE, PM_HTONE, PM_HPAT, PM_HLVL}}, {"SOURCE", {PM_HTYPE, PM_HTONE, PM_HPAT, PM_HLVL}}}};
static const uint8_t NEDIT[4] = {2, 2, 2, 1};
static const page_t PG_ENV[4] = {{"ENV", {PM_ADEC, PM_AACC, PM_AGATE, PM_AGLIDE}}, {"CALL", {PM_BPER, PM_BPHASE, PM_BDRIFT, PM_BQUANT}},
                                 {"ENV", {PM_KPDEC, PM_KBODY, PM_KBASE, PM_KDUCK}}, {"ENV", {PM_HDEC, PM_HTONE, PM_HPAT, PM_HLVL}}};
static const page_t PG_OTHER[] = {
    {"LFO>ACID", {PM_LRATE, PM_LDEP, PM_LSHAPE, PM_LDEST}}, {"DELAY", {PM_DTIME, PM_DFB, PM_DMIX, PM_DTONE}},
    {"SCALE", {PM_NONE, PM_SCALEC, PM_WALK, PM_ROOT}}, {"ARP BLIP", {PM_ARPSET, PM_NONE, PM_BLIPAMT, PM_BLIPN}},
    {"LINE", {PM_N, PM_OFF, PM_DENS, PM_SWING}}, {"GLOBAL", {PM_BPM, PM_NONE, PM_AUTO, PM_BARS}}};
static const page_t PG_STEPS = {"STEPS", {PM_SLEN, PM_ASRC, PM_SWING, PM_STRANS}};
static const page_t PG_SPACE = {"SPACE", {PM_RMIX, PM_MDRIVE, PM_NONE, PM_NONE}};

static struct {
    uint8_t group, page, part, rec, rootOct;
    int8_t held; uint32_t heldT; uint8_t used;
    int8_t touch; uint32_t touchT;
    char toast[32]; uint32_t toastT;
    uint8_t keyNote[27], keyTrk[27];
    int8_t recKey; uint8_t recTrk, recNote; uint32_t recLast;
    uint32_t seed; uint8_t dirty; uint32_t dirtyT;
    int8_t stepHeld; uint8_t stepKey, stepNew, stepEdited, lastNote, recPos; uint32_t stepT;
} U = {.held = -1, .touch = -1, .recKey = -1, .stepHeld = -1};

static const page_t *cur_page(void)
{
    switch (U.group) {
    case G_HOME: return &PG_HOME;
    case G_EDIT: return &PG_EDIT[U.part][U.page % NEDIT[U.part]];
    case G_ENV: return &PG_ENV[U.part];
    case G_LFO: return &PG_OTHER[0];
    case G_FX: return U.page % 2u == 0u ? &PG_OTHER[1] : &PG_SPACE;
    case G_SEL: return &PG_OTHER[2];
    case G_ARP: return &PG_OTHER[3];
    case G_SEQ: return U.page % 2u == 0u ? &PG_STEPS : &PG_OTHER[4];
    default: return &PG_OTHER[5];
    }
}
static uint32_t npages(void) { return U.group == G_EDIT ? NEDIT[U.part] : U.group == G_SEQ || U.group == G_FX ? 2u : 1u; }
static void toast(const char *a, const char *b)
{
    U.toast[0] = 0; scat(U.toast, a); if (b) scat(U.toast, b);
    U.toastT = fm1_ms; U.touch = -1;
}
static void changed(void) { U.dirty = 1; U.dirtyT = fm1_ms; }
static int steps_page(void) { return U.group == G_SEQ && U.page % 2u == 0u; }

/* ---- TB-3PO's idea: OCT+ throws a new ACID line, OCT- mutates one or two things (drums and BIO stay) */
static uint32_t rng_s = 1;
static uint32_t rnd(uint32_t n) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 17; rng_s ^= rng_s << 5; return rng_s % n; }
#define ACID_BITS (T_UP | T_SLIDE | T_ARP | T_BLIP | T_SCALE)
static void new_line(void)
{
    static const uint8_t NS[8] = {3, 4, 5, 5, 6, 7, 7, 9}, MC[4] = {2, 3, 3, 1}, WK[5] = {1, 2, 3, 3, 5}, AS[5] = {0, 0, 1, 2, 4}, BA[3] = {3, 5, 7}, DN[4] = {127, 127, 112, 96};
    uint32_t bits = 0;
    U.seed = (fm1_ms * 2654435761u) ^ (U.seed << 7) ^ 0x5bd1e995u;
    rng_s = U.seed | 1u;
    P.N = NS[rnd(8)]; P.off = (uint8_t)rnd(P.N);
    if (rnd(10) < 8u) bits |= T_UP;
    if (rnd(10) < 6u) bits |= T_SLIDE;
    if ((bits & T_UP) && rnd(2)) bits |= T_ARP;
    if (rnd(20) < 7u) bits |= T_BLIP;
    if (rnd(10) < 6u) bits |= T_SCALE;
    P.terms = (uint16_t)((P.terms & ~ACID_BITS) | bits);
    P.scaleM = rnd(3) ? 7 : 5; P.scaleC = P.scaleM == 7 ? MC[rnd(4)] : 3;
    P.walk = WK[rnd(5)]; P.arpSet = AS[rnd(5)]; P.blipAmt = BA[rnd(3)]; P.blipN = rnd(2) ? 3 : 5; P.dens = DN[rnd(4)];
    P.autoSong = 0; P.aSrc = 1;
    {
        char h[8] = "#"; static const char HX[] = "0123456789ABCDEF"; uint32_t i;
        for (i = 0; i < 4u; i++) h[1 + i] = HX[(U.seed >> (12 - 4 * i)) & 15u];
        h[5] = 0;
        toast("NEW LINE ", h);
    }
    changed();
}
static void mutate_line(void)
{
    uint32_t k = 1u + (rnd(5) < 2u), j, before = P.N | P.off << 8 | (uint32_t)P.terms << 16, after;
    rng_s ^= fm1_ms | 1u;
    for (j = 0; j < k; j++)
        switch (rnd(5)) {
        case 0: { int32_t n = (int32_t)P.N + (int32_t)rnd(5) - 2; P.N = (uint8_t)(n < 3 ? 3 : n > 9 ? 9 : n); if (P.off >= P.N) P.off = P.N - 1u; break; }
        case 1: P.off = (uint8_t)rnd(P.N); break;
        case 2: { static const uint16_t TB[4] = {T_SLIDE, T_BLIP, T_ARP, T_SCALE}; P.terms ^= TB[rnd(4)]; break; }
        case 3: { static const uint8_t AS[4] = {0, 1, 2, 4}; P.arpSet = AS[rnd(4)]; break; }
        default: { static const uint8_t WK[4] = {1, 2, 3, 5}; P.walk = WK[rnd(4)]; if (P.scaleM == 7) P.scaleC = (uint8_t)(1u + rnd(3)); }
        }
    after = P.N | P.off << 8 | (uint32_t)P.terms << 16;
    if (after == before) P.off = (uint8_t)((P.off + 1u) % P.N);
    P.autoSong = 0; P.aSrc = 1;
    toast("MUTATE", 0); changed();
}

/* ---- the lanes */
static uint32_t now_step(void) { return E.s + (E.stepPh >= 0x80000000u); }
static void write_lane(uint32_t t, uint32_t s, uint32_t v)
{
    uint32_t i = t == 0u && STEPS ? s % step_len() : s & (LANE - 1u);
    P.lane[t][i] = (uint8_t)v;
    if (t == 0u && STEPS && i < 16u) { P.accM &= (uint16_t)~(1u << i); P.sldM &= (uint16_t)~(1u << i); }
    count_lanes(); changed();
}
static void clear_lane(int all)
{
    uint32_t t, i;
    for (t = 0; t < 4u; t++) if (all || t == U.part) for (i = 0; i < LANE; i++) P.lane[t][i] = NONE;
    count_lanes(); changed();
    toast(all ? "CLEARED ALL" : "CLEARED ", all ? 0 : TRK[U.part]);
}

/* ---- the keys: play the selected track; with REC on, write into its lane */
static const uint16_t TBIT[4] = {0, T_BIO, T_KICK, T_PERC};
static void key_down(uint32_t k)
{
    uint32_t t = U.part;
    int32_t note = 53 + (int32_t)k + 12 * (int32_t)(int8_t)U.rootOct - (t == 0 ? 24 : 0);
    if (note < 0) note = 0;
    if (P.keyLock && t < 2u) note = snap_q8(note << 8) >> 8;
    U.keyNote[k] = (uint8_t)note; U.keyTrk[k] = (uint8_t)t;
    liveS[t] = (E.s << 16) + (E.stepPh >> 16);
    liveN[t] = (int16_t)note; live[t] = (int16_t)note;
    if (t == 3u && (P.terms & T_PERC)) perc_hit(2);
    if (TBIT[t] && !(P.terms & TBIT[t])) P.terms |= TBIT[t];
    if (U.rec && E.playing) { uint32_t q = now_step(); write_lane(t, q, (uint32_t)note); U.recKey = (int8_t)k; U.recTrk = (uint8_t)t; U.recNote = (uint8_t)note; U.recLast = q; }
    else if (U.rec && t == 0u && STEPS) {                          /* stopped: each key fills the next step */
        char s[24] = "STEP ", nn[8] = "";
        sint(s, U.recPos + 1); write_lane(0, U.recPos, (uint32_t)note); U.lastNote = (uint8_t)note;
        U.recPos = (uint8_t)((U.recPos + 1u) % step_len());
        snote(nn, note); scat(s, " "); scat(s, nn); toast(s, 0);
        return;
    }
    { char s[8] = ""; snote(s, note); toast(TRK[t], " "); scat(U.toast, s); }
}
static void key_up(uint32_t k)
{
    uint32_t t = U.keyTrk[k];
    if (live[t] == (int16_t)U.keyNote[k]) { liveE[t] = (E.s << 16) + (E.stepPh >> 16); live[t] = -1; }
    if (U.recKey == (int8_t)k) U.recKey = -1;
}
static void tick_rec(void)
{
    if (U.recKey < 0 || !U.rec || !E.playing) return;
    while ((int32_t)(E.s - U.recLast) > 0) { U.recLast++; write_lane(U.recTrk, U.recLast, U.recNote | 128u); }
}

/* ---- holding a button: the keys become that layer */
static const uint8_t NQ[11] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 16};
static const uint16_t TERM_KEYS[10] = {T_KICK, T_FOUR, T_FILL, T_UP, T_SLIDE, T_ARP, T_SCALE, T_BLIP, T_BIO, T_PERC};
static const char *const TERM_NAMES[10] = {"KICK", "4/4", "FILL", "UP", "SLIDE", "ARP", "SCALE", "BLIP", "BIO", "PERC"};
static int black_key(uint32_t k) { return (0x54Au >> ((53u + k) % 12u)) & 1u; }
static int key_index(uint32_t k, int *white)   /* key 0..26 -> white 0..15 or black 0..10 */
{
    uint32_t i, w = 0, b = 0;
    for (i = 0; i < k; i++) { if (black_key(i)) b++; else w++; }
    *white = !black_key(k);
    return (int)(*white ? w : b);
}
/* ---- the steps: TB-3PO's idea on the steps themselves. OCT+ writes a new line you can then edit, OCT- changes 2 or 3 steps */
static const int8_t GEN_W[12] = {0, 0, 0, 0, 0, 12, 12, 7, 3, 10, 15, 5};      /* root-heavy, natural minor */
static void seed_now(void) { U.seed = (fm1_ms * 2654435761u) ^ (U.seed << 7) ^ 0x5bd1e995u; rng_s = U.seed | 1u; }
static void gen_steps(void)
{
    uint32_t i, L = step_len();
    seed_now();
    P.accM = P.sldM = 0;
    for (i = 0; i < LANE; i++) P.lane[0][i] = NONE;
    for (i = 0; i < L; i++) {
        uint32_t r = rnd(100);
        if (i == 0u) { P.lane[0][i] = (uint8_t)P.root; P.accM |= 1u; continue; }
        if (r < 20u) continue;                                            /* a rest */
        if (r < 27u && P.lane[0][i - 1u] != NONE) P.lane[0][i] = (uint8_t)(step_note(i - 1u, L) | 128);   /* a tie */
        else P.lane[0][i] = (uint8_t)(P.root + GEN_W[rnd(12)]);
        if (rnd(100) < 28u) P.accM |= (uint16_t)(1u << i);
        if (rnd(100) < 22u) P.sldM |= (uint16_t)(1u << i);
    }
    P.aSrc = 0; P.autoSong = 0; count_lanes();
    {
        char h[8] = "#"; static const char HX[] = "0123456789ABCDEF"; uint32_t j;
        for (j = 0; j < 4u; j++) h[1 + j] = HX[(U.seed >> (12 - 4 * j)) & 15u];
        h[5] = 0;
        toast("NEW STEPS ", h);
    }
    changed();
}
static void mutate_steps(void)
{
    uint32_t k = 2u + rnd(2), j, L = step_len();
    rng_s ^= fm1_ms | 1u;
    for (j = 0; j < k; j++) {
        uint32_t i = rnd(L), b = 1u << i;
        switch (rnd(4)) {
        case 0: P.lane[0][i] = (uint8_t)(P.root + GEN_W[rnd(12)]); break;
        case 1: P.accM ^= (uint16_t)b; break;
        case 2: P.sldM ^= (uint16_t)b; break;
        default: P.lane[0][i] = P.lane[0][i] == NONE ? (uint8_t)(P.root + GEN_W[rnd(12)]) : NONE;
        }
    }
    P.aSrc = 0; count_lanes(); toast("MUTATE STEPS", 0); changed();
}
/* holding a step (a white key on SEQ > STEPS): KNOB1 note, KNOB2 rest / note / tie, KNOB3 accent, KNOB4 slide; OCT-/+ an octave */
static void step_preview(int on)
{
    if (E.playing) return;
    if (on && U.stepHeld >= 0 && P.lane[0][U.stepHeld] != NONE) {
        int32_t n = step_note((uint32_t)U.stepHeld, step_len());
        liveS[0] = (E.s << 16) + (E.stepPh >> 16); liveN[0] = (int16_t)n; live[0] = (int16_t)n;
    } else if (live[0] >= 0) { liveE[0] = (E.s << 16) + (E.stepPh >> 16); live[0] = -1; }
}
static void step_set_note(uint32_t i, int32_t n)
{
    if (n < 12) n = 12;
    if (n > 96) n = 96;
    P.lane[0][i] = (uint8_t)n; U.lastNote = (uint8_t)n;
}
static void step_key(uint32_t k, int down)
{
    int w, i = key_index(k, &w);
    if (!w) return;                                                /* black keys: nothing here */
    if (down) {
        if ((uint32_t)i >= step_len()) { char s[16] = "LENGTH IS "; sint(s, (int32_t)step_len()); toast(s, 0); return; }
        U.stepNew = P.lane[0][i] == NONE;
        if (U.stepNew) { step_set_note((uint32_t)i, U.lastNote ? U.lastNote : P.root); count_lanes(); changed(); }
        U.stepHeld = (int8_t)i; U.stepKey = (uint8_t)k; U.stepT = fm1_ms; U.stepEdited = 0;
        step_preview(1);
    } else if (U.stepHeld >= 0 && U.stepKey == k) {
        uint32_t j = (uint32_t)U.stepHeld;
        if (!U.stepNew && !U.stepEdited && fm1_ms - U.stepT < 400u) {          /* a tap on a step that was on: off */
            P.lane[0][j] = NONE; P.accM &= (uint16_t)~(1u << j); P.sldM &= (uint16_t)~(1u << j); count_lanes(); changed();
        }
        step_preview(0); U.stepHeld = -1;
    }
}
static void step_turn(uint32_t knob, int32_t d)
{
    uint32_t j = (uint32_t)U.stepHeld, L = step_len(), b = 1u << j, v = P.lane[0][j];
    switch (knob) {
    case 0: {                                                      /* NOTE: with KEY LOCK, the notes of the scale */
        int32_t n = step_note(j, L), dir = d > 0 ? 1 : -1, c = d > 0 ? d : -d;
        while (c--) { int32_t g = 0; do { n += dir; g++; } while (P.keyLock && g < 4 && (snap_q8(n << 8) >> 8) != n); }
        step_set_note(j, n); break;
    }
    case 1: {                                                      /* GATE: REST, NOTE, TIE */
        int32_t g = v == NONE ? 0 : (v & 128u) ? 2 : 1;
        g += d > 0 ? 1 : -1; g = g < 0 ? 0 : g > 2 ? 2 : g;
        if (g == 0) P.lane[0][j] = NONE;
        else if (g == 1) step_set_note(j, v == NONE ? (U.lastNote ? U.lastNote : P.root) : (int32_t)(v & 127u));
        else P.lane[0][j] = (uint8_t)(step_note((j + L - 1u) % L, L) | 128);
        break;
    }
    case 2: if (d > 0) P.accM |= (uint16_t)b; else P.accM &= (uint16_t)~b; break;
    default: if (d > 0) P.sldM |= (uint16_t)b; else P.sldM &= (uint16_t)~b; break;
    }
    U.stepEdited = 1; U.touch = (int8_t)knob; U.touchT = fm1_ms;
    count_lanes(); changed();
    if (live[0] >= 0) step_preview(1);
}
static void step_cell(uint32_t knob, const char **lab, char *val)
{
    static const char *const LAB[4] = {"NOTE", "GATE", "ACCENT", "SLIDE"};
    uint32_t j = (uint32_t)U.stepHeld, v = P.lane[0][j];
    *lab = LAB[knob]; val[0] = 0;
    switch (knob) {
    case 0: if (v == NONE) scat(val, "-"); else snote(val, step_note(j, step_len())); break;
    case 1: scat(val, v == NONE ? "REST" : (v & 128u) ? "TIE" : "NOTE"); break;
    case 2: scat(val, P.accM >> j & 1u ? "ON" : "OFF"); break;
    default: scat(val, P.sldM >> j & 1u ? "ON" : "OFF");
    }
}

/* FX held: white keys 1-6 replay the sound (loops 1/4..1/32, reverse, tape stop), 7-9 colour it (LP and HP sweeps, crush),
 * 10-14 change ACID's curve (y + 12, mirror, x / 2, 2x, squelch), 15 rise (let go: the drop), 16 throw into the delay */
static const char *const FX_LAB[16] = {"L1/4", "L1/8", "L1/16", "L1/32", "REV", "STOP", "LP", "HP", "CRUSH",
                                       "Y+12", "MIRR", "X/2", "2X", "SQLCH", "RISE", "THROW"};
static const char *const FX_SAY[16] = {"LOOP 1/4", "LOOP 1/8", "LOOP 1/16", "LOOP 1/32", "REVERSE", "TAPE STOP", "LOW-PASS DOWN",
                                       "HIGH-PASS UP", "CRUSH", "Y = Y + 12", "Y = 2C - Y", "X = X / 2", "X = 2X", "SQUELCH", "RISE", "THROW"};
static uint8_t fxThrowKeep, fxThrow;
static int8_t fxBufKey = -1;
static void fx_key(uint32_t i, int down)
{
    static const uint8_t FN[5] = {FN_OCT, FN_MIRROR, FN_HALF, FN_DOUBLE, FN_SQUELCH}, PF[3] = {PF_LP, PF_HP, PF_CRUSH};
    if (i >= 16u) return;
    if (down) toast(FX_SAY[i], 0);
    if (i < 6u) {
        if (down) { X.req = (uint8_t)(i + 1u); fxBufKey = (int8_t)i; }
        else if (fxBufKey == (int8_t)i) { X.req = 0; fxBufKey = -1; }
    } else if (i < 9u) { if (down) X.flags |= PF[i - 6u]; else X.flags &= (uint8_t)~PF[i - 6u]; }
    else if (i < 14u) { if (down) fxFn |= FN[i - 9u]; else fxFn &= (uint8_t)~FN[i - 9u]; }
    else if (i == 14u) { riseStart = down ? (int32_t)E.s : -1; if (!down) toast("DROP", 0); }
    else if (down && !fxThrow) { fxThrowKeep = P.dMix; P.dMix = 127; fxThrow = 1; }
    else if (!down && fxThrow) { P.dMix = fxThrowKeep; fxThrow = 0; }
}
static void fx_release_all(void)                 /* FX let go: everything back */
{
    X.req = 0; X.flags = 0; fxFn = 0; fxBufKey = -1; riseStart = -1;
    if (fxThrow) { P.dMix = fxThrowKeep; fxThrow = 0; }
}
static int fx_on(uint32_t i)
{
    static const uint8_t FN[5] = {FN_OCT, FN_MIRROR, FN_HALF, FN_DOUBLE, FN_SQUELCH}, PF[3] = {PF_LP, PF_HP, PF_CRUSH};
    if (i < 6u) return fxBufKey == (int8_t)i;
    if (i < 9u) return (X.flags & PF[i - 6u]) != 0;
    if (i < 14u) return (fxFn & FN[i - 9u]) != 0;
    return i == 14u ? riseStart >= 0 : fxThrow;
}

static void layer_key(uint32_t k, int down)
{
    int w, i = key_index(k, &w);
    U.used = 1;
    switch (U.held) {
    case BTN_SEQ:
        if (!down) return;
        if (w && i < 10) { P.terms ^= TERM_KEYS[i]; toast(TERM_NAMES[i], (P.terms & TERM_KEYS[i]) ? " ON" : " OFF"); }
        else if (!w) { P.N = NQ[i]; if (P.off >= P.N) P.off = P.N - 1u; { char s[6] = "N="; sint(s, P.N); toast(s, 0); } }
        changed(); break;
    case BTN_GLO:
        if (down && w && i < 4) { P.mutes ^= (uint8_t)(1u << i); toast(TRK[i], (P.mutes >> i & 1u) ? " MUTED" : " ON"); changed(); }
        break;
    case BTN_REC:
        if (!down || !w) return;
        if (i == 0) clear_lane(0);
        else if (i == 1) clear_lane(1);
        else if (i == 2 && U.part < 2u) { P.laneMode[U.part] ^= 1u; toast(TRK[U.part], P.laneMode[U.part] ? " EXACT" : " FOLLOW"); changed(); }
        else if (i == 3) { P.keyLock ^= 1u; toast("KEY LOCK ", P.keyLock ? "ON" : "OFF"); changed(); }
        break;
    case BTN_FX:
        if (w) fx_key((uint32_t)i, down);
        break;
    case BTN_SEL:
        if (down) { P.root = (int8_t)(53 + (int32_t)k - 24); { char s[8] = ""; snote(s, P.root); toast("ROOT ", s); } changed(); }
        break;
    case BTN_HOME:
        if (down && w && i < 6) { apply_chapter((uint32_t)i); riseStart = -1; toast(CHAPTERS[i].name, 0); changed(); }
        if (down && w && (i == 8 || i == 9)) { load_steps(i == 9); count_lanes(); toast(i == 9 ? "STEPS: FILM" : "STEPS: BLANK", 0); changed(); }
        break;
    case BTN_EDIT: case BTN_ENV:
        if (!down || !w) return;
        if (i < 4) { U.part = (uint8_t)i; U.page = 0; toast(TRK[i], 0); }
        else if (i >= 8 && (uint32_t)(i - 8) < NPRE[U.part]) { apply_preset(U.part, (uint32_t)(i - 8)); toast(pre_name(U.part, P.pre[U.part]), 0); changed(); }
        break;
    case BTN_ARP:
        if (down && w && i < 6) { P.arpSet = (uint8_t)i; P.terms |= T_ARP; toast("CHORD ", L_CHORD[i]); changed(); }
        break;
    default: break;
    }
}
static const char *layer_label(uint32_t k, char *buf)       /* what key k does in the held layer */
{
    int w, i = key_index(k, &w);
    buf[0] = 0;
    switch (U.held) {
    case BTN_SEQ: if (w && i < 10) return TERM_NAMES[i]; if (!w) { scat(buf, "N"); sint(buf, NQ[i]); return buf; } break;
    case BTN_GLO: if (w && i < 4) return TRK[i]; break;
    case BTN_REC: if (w && i == 0) return "CLR"; if (w && i == 1) return "ALL"; if (w && i == 2) return P.laneMode[U.part] ? "EXACT" : "FOLLW"; if (w && i == 3) return "LOCK"; break;
    case BTN_FX: if (w && i < 16) return FX_LAB[i]; break;
    case BTN_SEL: snote(buf, 53 + (int32_t)k - 24); return buf;
    case BTN_HOME: if (w && i < 6) return CHAPTERS[i].name; if (w && i == 8) return "BLANK"; if (w && i == 9) return "FILM"; break;
    case BTN_EDIT: case BTN_ENV: if (w && i < 4) return TRK[i]; if (w && i >= 8 && (uint32_t)(i - 8) < NPRE[U.part]) { scat(buf, "P"); sint(buf, i - 7); return buf; } break;
    case BTN_ARP: if (w && i < 6) return L_CHORD[i]; break;
    default: break;
    }
    return "";
}
static int layer_on(uint32_t k)
{
    int w, i = key_index(k, &w);
    switch (U.held) {
    case BTN_SEQ: return w ? (i < 10 && (P.terms & TERM_KEYS[i])) : NQ[i] == P.N;
    case BTN_GLO: return w && i < 4 && !(P.mutes >> i & 1u);
    case BTN_REC: return w && ((i == 2 && P.laneMode[U.part] == 0) || (i == 3 && P.keyLock));
    case BTN_SEL: return 53 + (int32_t)k - 24 == P.root;
    case BTN_HOME: return w && !STEPS && i == (int)P.chapter;
    case BTN_FX: return w && fx_on((uint32_t)i);
    case BTN_EDIT: case BTN_ENV: return w && (i == (int)U.part || (i >= 8 && i - 8 == (int)P.pre[U.part]));
    case BTN_ARP: return w && i == (int)P.arpSet;
    default: return 0;
    }
}
static int has_layer(int b) { return b == BTN_SEQ || b == BTN_GLO || b == BTN_REC || b == BTN_FX || b == BTN_SEL || b == BTN_HOME || b == BTN_EDIT || b == BTN_ENV || b == BTN_ARP; }

static void tap(int b)
{
    static const int8_t GRP[14] = {-1, -1, G_FX, G_SEL, G_ENV, G_LFO, G_EDIT, G_GLO, G_HOME, -2, G_ARP, G_SEQ, -1, -1};
    if (b == BTN_REC) {
        U.rec ^= 1u; recNow = U.rec; U.recPos = 0;
        if (U.rec) {
            P.autoSong = 0;
            if (!E.playing && U.part == 0u && STEPS) toast("REC: KEYS FILL STEP 1, 2..", 0);
            else { if (!E.playing) E.startReq = 1; toast("REC ", TRK[U.part]); }
        } else { U.recKey = -1; toast("REC OFF", 0); }
        return;
    }
    if (GRP[b] == -2) { settings_save(&P, sizeof P); U.dirty = 0; toast("SAVED", 0); return; }   /* SAVE: keep it in flash */
    if (GRP[b] < 0) return;
    if (U.group == (uint8_t)GRP[b]) U.page = (uint8_t)((U.page + 1u) % npages());
    else { U.group = (uint8_t)GRP[b]; U.page = 0; }
    if (U.group == G_SEQ) U.part = 0;                               /* SEQ is ACID's */
}

static void turn(uint32_t enc, int32_t d)
{
    if (U.held >= 0) U.used = 1;
    if (steps_page() && U.stepHeld >= 0 && enc >= ENC_KNOB1 && enc <= ENC_KNOB4) { step_turn(enc - ENC_KNOB1, d); return; }
    if (enc == ENC_SELECT) {
        if (npages() > 1u) { int32_t p = (int32_t)U.page + d; U.page = (uint8_t)(p < 0 ? 0 : p >= (int32_t)npages() ? (int32_t)npages() - 1 : p); }
        else if (STEPS) toast("STEPS PLAY (SEQ)", 0);
        else { int32_t c = (int32_t)P.chapter + d; c = c < 0 ? 0 : c > 5 ? 5 : c; apply_chapter((uint32_t)c); toast(CHAPTERS[c].name, 0); changed(); }
        return;
    }
    if (enc == ENC_ALGORITHM) {
        int32_t p = (int32_t)U.part + d; uint32_t k;
        for (k = 0; k < 27u; k++) if (fm1_in.notes >> k & 1u) key_up(k);
        U.part = (uint8_t)(p < 0 ? 0 : p > 3 ? 3 : p);
        if (U.group != G_EDIT && U.group != G_ENV) U.group = G_EDIT;
        U.page = 0; toast(TRK[U.part], 0); return;
    }
    if (enc == ENC_PRESETS) {
        if (U.group != G_EDIT && U.group != G_ENV) { U.group = G_EDIT; U.page = 0; }
        apply_preset(U.part, (uint32_t)((int32_t)P.pre[U.part] + d + NPRE[U.part]) % NPRE[U.part]);
        toast(pre_name(U.part, P.pre[U.part]), 0); changed(); return;
    }
    {
        uint32_t i = enc - ENC_KNOB1, k = cur_page()->k[i];
        int32_t step, span;
        if (k == PM_NONE) return;
        span = PM[k].max - PM[k].min;
        step = span > 40 ? (d > 1 || d < -1 ? d * 3 : d * 2) : d;           /* wide ranges move faster */
        pset(k, pget(k) + step);
        U.touch = (int8_t)i; U.touchT = fm1_ms;
        changed();
    }
}

/* ================================================================ the screen */
#define GX0 8
#define COLS 224
#define GY0 60
#define GY1 164
#define AXY 126
static inline int32_t py(int32_t q8) { return AXY - (((q8 - 44 * 256) * 17) / 2560); }      /* 1.7 px a semitone */
static uint16_t pal_paper[17];                               /* ink over paper, 0..16 */
static int32_t clampy(int32_t y) { return y < GY0 ? GY0 : y > GY1 ? GY1 : y; }

static void chip(int32_t x, int32_t y, uint32_t t, int ring)
{
    char d[2] = {(char)('1' + t), 0};
    if (ring) cv_rect(x - 1, y - 1, 13, 13, REDC);
    cv_rect(x, y, 11, 11, TCOL[t]);
    t5(x + 3, y + 2, d, LIME);
}
static void draw_top(void)                                   /* 0..58: the page, the transport, the 4 cells */
{
    const page_t *pg = cur_page();
    char s[32];
    uint32_t i, x = 4;
    int edit = U.group == G_EDIT || U.group == G_ENV;
    cv_begin(240, 58, PAPER);
    if (edit) { chip(3, 3, U.part, U.rec); x = 18; }
    s[0] = 0;
    if (edit) { scat(s, TRK[U.part]); scat(s, ">"); }
    if (steps_page() && U.stepHeld >= 0) { scat(s, "STEP "); sint(s, U.stepHeld + 1); }
    else scat(s, pg->name);
    if (npages() > 1u) { scat(s, " "); sint(s, U.page % npages() + 1u); scat(s, "/"); sint(s, npages()); }
    t5((int32_t)x, 5, s, INK);
    s[0] = 0;
    if (edit) scat(s, pre_name(U.part, P.pre[U.part]));        /* on a track's pages: its preset instead of the clock */
    else { if (E.playing) { sint(s, (E.s >> 4) % 99u + 1u); scat(s, "."); sint(s, ((E.s & 15u) >> 2) + 1u); scat(s, "  "); } sint(s, P.bpm); }
    i = 236u - 6u * slen(s);
    t5((int32_t)i, 5, s, INK);
    if (E.playing) { cv_rect((int32_t)i - 9, 5, 2, 7, INK); cv_rect((int32_t)i - 7, 6, 2, 5, INK); cv_rect((int32_t)i - 5, 7, 2, 3, INK); }
    else cv_rect((int32_t)i - 9, 5, 6, 7, mixc(PAPER, INK, 8));
    if (U.rec) cv_rect((int32_t)i - 20, 5, 7, 7, REDC);
    if (riseStart >= 0) t5((int32_t)i - 48, 5, "RISE", REDC);
    for (i = 0; i < 4u; i++) {
        int32_t x0 = 3 + (int32_t)i * 59, k = pg->k[i], hot = U.touch == (int8_t)i && fm1_ms - U.touchT < 800u;
        uint16_t fg = hot ? LIME : INK, bg = hot ? INK : PAPER;
        if (hot) cv_rect(x0, 18, 57, 38, INK);
        else { cv_rect(x0, 18, 57, 1, mixc(PAPER, INK, 7)); cv_rect(x0, 55, 57, 1, mixc(PAPER, INK, 7)); cv_rect(x0, 18, 1, 38, mixc(PAPER, INK, 7)); cv_rect(x0 + 56, 18, 1, 38, mixc(PAPER, INK, 7)); }
        if (k == PM_NONE && U.group == G_GLO && i == 1u) {          /* GLOBAL: how much of the 5.8 ms the sound takes */
            s[0] = 0; sint(s, (int32_t)(cpu_max * 100u / 5805u)); scat(s, "%");
            t5(x0 + 3, 21, "AUDIO CPU", mixc(PAPER, INK, 12)); tx(x0 + 3, 31, &FONT_S, s, fg, bg);
            continue;
        }
        if (steps_page() && U.stepHeld >= 0) {
            const char *lab;
            step_cell(i, &lab, s);
            t5(x0 + 3, 21, lab, hot ? LIME : mixc(PAPER, INK, 12)); tx(x0 + 3, 31, &FONT_S, s, fg, bg);
            continue;
        }
        if (k == PM_NONE) continue;
        t5(x0 + 3, 21, PM[k].lab, hot ? LIME : mixc(PAPER, INK, 12));
        pfmt((uint32_t)k, s);
        if (text_w(&FONT_S, s) <= 54) tx(x0 + 3, 31, &FONT_S, s, fg, bg);
        else t5(x0 + 3, 36, s, fg);
        {
            int32_t lo = PM[k].min, hi = k == PM_OFF ? P.N - 1 : PM[k].max, v = pget((uint32_t)k);
            cv_rect(x0 + 3, 50, 51, 2, hot ? mixc(INK, LIME, 5) : mixc(PAPER, INK, 4));
            if (hi > lo) cv_rect(x0 + 3, 50, 51 * (v - lo) / (hi - lo), 2, fg);
        }
    }
    cv_blit(0, 0);
}

static void draw_readout(void);
static void draw_graph(void)                                 /* 58..166: the four functions on one pair of axes */
{
    uint32_t c, s0 = E.s & ~31u, colNow = ((E.s & 31u) * 7u) + ((E.stepPh >> 16) * 7u >> 16);
    int edit = U.group == G_EDIT || U.group == G_ENV, fo = edit ? (int)U.part : -1;
    int32_t pyA = 0, pyB = 0, pyK = 0, gA0 = 0, gB0 = 0, gK0 = 0, yNowA = 0, yNowB = 0, yNowK = 0, bNow = 0, kNow = 0;
    uint16_t cA, cB, cK, cAf, cBf, cKf;
    cv_begin(240, 108, PAPER);

    cA = mixc(PAPER, TCOL[0], fo < 0 || fo == 0 ? 16 : 5); cB = mixc(PAPER, TCOL[1], fo < 0 || fo == 1 ? 16 : 5); cK = mixc(PAPER, TCOL[2], fo < 0 || fo == 2 ? 16 : 5);
    cAf = mixc(PAPER, TCOL[0], 4); cBf = mixc(PAPER, TCOL[1], 4); cKf = mixc(PAPER, TCOL[2], 3);
    /* axes */
    cv_rect(GX0 - 2, AXY - 58, COLS + 6, 1, mixc(PAPER, INK, 13));
    cv_rect(GX0 + COLS / 2, GY0 - 58, 1, GY1 - GY0, mixc(PAPER, INK, 13));
    t5(GX0 + COLS - 4, AXY - 58 - 9, "X", INK); t5(GX0 + COLS / 2 - 8, 3, "Y", INK);
    for (c = 0; c < COLS; c++) {
        uint32_t s = s0 + c / 7u, p16 = (c % 7u) * 9362u, x = GX0 + c, past = c <= colNow;
        acid_t a; voice_t b, k;
        int32_t ya, yb, yk, fc;
        eval_acid(s, p16, &a, 0); eval_bio(s, p16, &b); eval_kick(s, p16, &k);
        ya = clampy(py(a.y)) - 58; yb = clampy(py(b.y)) - 58; yk = clampy(py(k.y)) - 58;
        if (P.mutes & 1u) a.gate = 0;
        if (P.mutes & 2u) b.on = 0;
        if (P.mutes & 4u) k.on = 0;
        /* the ACID's harmonics under its filter: the spectrum, lighter the further above the cutoff */
        if (past && a.gate && (fo < 0 || fo == 0) && (c & 1u) == 0u) {
            static const uint8_t HS[8] = {12, 19, 24, 28, 31, 34, 36, 38};
            uint32_t h;
            fc = cutoff_at(&a, s, p16);
            for (h = 0; h < 8u; h++) {
                int32_t hp = a.y + (HS[h] << 8), dd = (hp - fc) >> 8, lv = 9 - (int32_t)h;
                int32_t yy = py(hp) - 58;
                if (dd > 0) lv -= dd / 3;
                if (dd > -2 && dd < 3) lv += P.res >> 5;
                if (lv <= 1 || yy < GY0 - 58) continue;
                cv_rect((int32_t)x, yy, 1, 2, mixc(PAPER, INK, (uint32_t)(lv > 9 ? 9 : lv)));
            }
            if (c % 3u == 0u) { int32_t yc = py(fc) - 58; if (yc > GY0 - 58 && yc < GY1 - 58) cv_rect((int32_t)x, yc, 2, 1, mixc(PAPER, INK, U.page == 1u && fo == 0 ? 16 : 9)); }
        }
        /* PERC: a band at the top */
        if (past && (P.terms & T_PERC) && !(P.mutes & 8u) && (fo < 0 || fo == 3)) {
            uint32_t h = perc_on(s);
            if (h && p16 < (P.hType ? 40000u : 20000u)) cv_rect((int32_t)x, 4 + (int32_t)((c * 37u) % 9u), 1, P.hType == 2 ? 10 : 3, mixc(PAPER, TCOL[3], h == 2u ? 14 : 8));
        }
        if (c) {
            if (k.on && gK0) cv_line((int32_t)x - 1, pyK, (int32_t)x, yk, past ? cK : cKf);
            if (k.on && gK0 && past && (fo < 0 || fo == 2)) cv_line((int32_t)x - 1, pyK + 1, (int32_t)x, yk + 1, cK);
            if (a.gate && gA0 && (pyA - ya < 9 && ya - pyA < 9)) cv_line((int32_t)x - 1, pyA, (int32_t)x, ya, past ? cA : cAf);
            if (a.gate && gA0 && past && fo == 0) cv_line((int32_t)x - 1, pyA - 1, (int32_t)x, ya - 1, cA);
            if (b.on && gB0 && (c % 6u) < 4u) { cv_line((int32_t)x - 1, pyB, (int32_t)x, yb, past ? cB : cBf); if (past) cv_line((int32_t)x - 1, pyB + 1, (int32_t)x, yb + 1, cB); }
        }
        pyA = ya; pyB = yb; pyK = yk; gA0 = a.gate; gB0 = b.on; gK0 = k.on;
        if (c == colNow) { yNowA = ya; yNowB = yb; yNowK = yk; bNow = b.on; kNow = k.on; }
    }
    /* the playheads: ACID a red dot, BIO a red ring, KICK a blue dot */
    {
        int32_t x = GX0 + (int32_t)colNow;
        if (kNow) cv_rect(x - 2, yNowK - 2, 5, 5, cK);
        if (bNow) { cv_rect(x - 3, yNowB - 3, 7, 1, REDC); cv_rect(x - 3, yNowB + 3, 7, 1, REDC); cv_rect(x - 3, yNowB - 3, 1, 7, REDC); cv_rect(x + 3, yNowB - 3, 1, 7, REDC); }
        cv_rect(x - 3, yNowA - 2, 7, 5, REDC); cv_rect(x - 2, yNowA - 3, 5, 7, REDC);
    }
    if (U.group == G_SEQ && U.page % 2u == 1u) { cv_rect(4, 4, 160, 11, LIME); t5(6, 6, "OCT+ NEW LINE  OCT- MUTATE", INK); }
    draw_readout();
    cv_oy = 0;
    cv_blit(0, 58);
}
static void draw_readout(void)                               /* what a knob did (with its track), or a message */
{
    {
        char s[40]; s[0] = 0;
        if (U.touch >= 0 && fm1_ms - U.touchT < 1000u && steps_page() && U.stepHeld >= 0) {
            const char *lab; char v[12];
            step_cell((uint32_t)U.touch, &lab, v);
            scat(s, "STEP "); sint(s, U.stepHeld + 1); scat(s, " "); scat(s, lab); scat(s, " "); scat(s, v);
        } else if (U.touch >= 0 && fm1_ms - U.touchT < 1000u) {
            uint32_t k = cur_page()->k[U.touch];
            if (PM[k].part < 4u) { scat(s, TRK[PM[k].part]); scat(s, " "); }
            scat(s, PM[k].lab); scat(s, " "); pfmt(k, s + slen(s));
        } else if (fm1_ms - U.toastT < 1100u) scat(s, U.toast);
        if (s[0]) {
            int32_t w = text_w(&FONT_S, s) + 14;
            if (w > 236) w = 236;
            cv_rect((240 - w) / 2, 80, w, 22, INK);
            tx((240 - w) / 2 + 7, 83, &FONT_S, s, LIME, INK);
        }
    }
}

static void draw_steps(void)                                 /* SEQ > STEPS: ACID's 16 steps, each over its white key */
{
    uint32_t L = step_len(), i, now = E.playing && STEPS ? E.s % L : 99u;
    int32_t lo = P.root - 5, prevY = -1;
    cv_begin(240, 108, PAPER);
    if (!STEPS) { cv_rect(4, 3, 232, 11, REDC); t5(6, 5, "THE FORMULA PLAYS: KNOB2 -> STEPS", LIME); }
    else { cv_rect(4, 3, 232, 11, LIME); t5(6, 5, "HOLD STEP+KNOBS  OCT+ NEW  OCT- MUTATE", INK); }
    cv_rect(0, 86 - 10, 240, 1, mixc(PAPER, INK, 4));                                  /* the root */
    for (i = 0; i < 16u; i++) {
        int32_t x = 2 + (int32_t)i * 15, y, n;
        uint32_t v = P.lane[0][i], acc = P.accM >> i & 1u, sld = P.sldM >> i & 1u;
        char num[4] = "";
        if (i >= L) { cv_rect(x, 18, 14, 84, mixc(PAPER, INK, 2)); prevY = -1; continue; }
        if (i == now) cv_rect(x, 18, 14, 84, mixc(PAPER, INK, 4));
        if (U.stepHeld == (int8_t)i) { cv_rect(x - 1, 17, 16, 1, REDC); cv_rect(x - 1, 102, 16, 1, REDC); cv_rect(x - 1, 17, 1, 86, REDC); cv_rect(x + 14, 17, 1, 86, REDC); }
        sint(num, (int32_t)i + 1);
        t5(x + (i < 9u ? 4 : 1), 93, num, mixc(PAPER, INK, i == now ? 16 : 9));
        if (v == NONE) { cv_rect(x + 5, 84, 4, 1, mixc(PAPER, INK, 7)); prevY = -1; continue; }
        n = step_note(i, L);
        y = 86 - (n - lo) * 2; y = y < 20 ? 20 : y > 88 ? 88 : y;
        if ((v & 128u) && prevY >= 0) cv_rect(x - 3, y - 2, 17, 5, mixc(PAPER, INK, 12));        /* a tie: one long bar */
        else cv_rect(x + 1, y - 2 - (int32_t)acc, 12, 5 + 2 * (int32_t)acc, acc ? REDC : INK);
        if (sld && i + 1u < L && P.lane[0][i + 1u] != NONE) {                                    /* a slide: a line into the next note */
            int32_t n2 = step_note(i + 1u, L), y2 = 86 - (n2 - lo) * 2;
            y2 = y2 < 20 ? 20 : y2 > 88 ? 88 : y2;
            cv_line(x + 12, y, x + 16, y2, INK); cv_line(x + 12, y + 1, x + 16, y2 + 1, INK);
        }
        prevY = y;
    }
    draw_readout();
    cv_blit(0, 58);
}

static void draw_rows(void)                                  /* 166..240: the four tracks */
{
    uint32_t t, k, sNow = E.s, bar0 = sNow & ~15u;
    cv_begin(240, 74, PAPER);
    cv_rect(0, 0, 240, 1, mixc(PAPER, INK, 9));
    for (t = 0; t < 4u; t++) {
        int32_t y = 2 + (int32_t)t * 18, on = t == 0 || (P.terms & TBIT[t]), muted = P.mutes >> t & 1u, sel = t == U.part;
        uint16_t bg = sel ? mixc(PAPER, RGB(255, 255, 232), 9) : PAPER, ink = on && !muted ? INK : mixc(PAPER, INK, 6);
        char nm[16];
        if (sel) { cv_rect(0, y - 1, 240, 18, bg); cv_rect(0, y - 1, 3, 18, U.rec ? REDC : TCOL[t]); }
        chip(5, y + 2, t, 0);
        tx(20, y, &FONT_S, TRK[t], ink, bg);
        nm[0] = 0;
        if (muted) scat(nm, "MUTE"); else if (!on) scat(nm, "OFF");
        else { if (laneN[t]) { scat(nm, "*"); sint(nm, laneN[t]); scat(nm, " "); } scat(nm, pre_name(t, P.pre[t])); }
        nm[9] = 0;
        t5(58, y + 5, nm, mixc(PAPER, INK, on && !muted ? 11 : 5));
        for (k = 0; k < 16u; k++) {
            uint32_t s = bar0 + k, x = 116u + k * 5u + (k >> 2) * 2u, h = 0;
            acid_t a; voice_t v;
            cv_rect((int32_t)x, y + 15, 4, 1, mixc(PAPER, INK, 4));
            if (on && !muted) {
                if (t == 0 && STEPS) {                      /* the steps as written (also when stopped) */
                    uint32_t i = s % step_len(), v = P.lane[0][i];
                    if (v != NONE) h = (v & 128u) ? 5u : (P.accM >> i & 1u) ? 12u : 8u;
                } else if (t == 0) { eval_acid(s, 0, &a, 1); if (a.gate) h = a.acc ? 12 : 8; }
                else if (t == 1) { eval_bio(s, 20000u, &v); if (v.on) h = 4u + (uint32_t)(v.amp >> 12); }
                else if (t == 2) { eval_kick(s, 0, &v); if (v.on && v.kt == 0u) h = 10; }
                else { uint32_t hp = perc_on(s); h = hp == 2u ? 9u : hp ? 5u : 0u; }
            }
            if (h) cv_rect((int32_t)x, y + 15 - (int32_t)h, 4, (int32_t)h, s == sNow && E.playing ? REDC : TCOL[t]);
            if (s == sNow && E.playing) cv_rect((int32_t)x, y + 16, 4, 1, U.rec ? REDC : INK);
        }
    }
    cv_blit(0, 166);
}

static void vlabel(int32_t x, int32_t y, const char *l, uint32_t max, uint16_t c)   /* one letter under the other, as JIANT does */
{
    uint32_t i;
    char ch[2] = {0, 0};
    for (i = 0; l[i] && i < max; i++) { ch[0] = l[i]; t5(x, y + (int32_t)i * 8, ch, c); }
}
static void draw_keymap(void)                                /* holding a button: the keys as they are now */
{
    static const char *const NAME[14] = {"OCT-", "OCT+", "FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC"};
    uint32_t k, w = 0;
    char buf[8], t[24] = "HOLD ";
    cv_begin(240, 108, PAPER);
    scat(t, NAME[U.held]);
    t5(5, 4, t, INK);
    for (k = 0; k < 27u; k++) if (!black_key(k)) {
        int32_t x = 2 + (int32_t)w * 15, on = layer_on(k);
        const char *l = layer_label(k, buf);
        cv_rect(x, 56, 14, 48, on ? INK : mixc(PAPER, INK, 9)); if (!on) cv_rect(x + 1, 57, 12, 46, PAPER);
        vlabel(x + 4, 60, l, 5, on ? LIME : INK);
        w++;
    }
    for (k = 0, w = 0; k < 27u; k++) {
        if (black_key(k)) {
            int32_t x = 2 + (int32_t)w * 15 - 7, on = layer_on(k);
            const char *l = layer_label(k, buf);
            cv_rect(x, 16, 14, 36, on ? INK : mixc(PAPER, INK, 9)); if (!on) cv_rect(x + 1, 17, 12, 34, PAPER);
            vlabel(x + 4, 19, l, 4, on ? LIME : INK);
        } else w++;
    }
    cv_blit(0, 58);
}

/* ================================================================ the main loop */
static void app_main(void)
{
    uint32_t prev = 0, lastFrame = 0, lastTop = 0, k, ledT = 0;
    if (settings_load(&P, sizeof P) != 0 || P.magic != MAGIC) defaults();
    count_lanes();
    for (k = 0; k <= 16u; k++) pal_paper[k] = mixc(PAPER, INK, k);
    lcd_fill(0, 0, 240, 240, PAPER);
    toast(APP_NAME " ", APP_VER);                                           /* the version, for a second */
    for (;;) {
        uint32_t keys, down, up, b, rel, e;
        platform_poll();
        /* keys */
        keys = fm1_in.notes;
        down = (keys & ~prev) | fm1_input_note_edges();
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) { if (U.held >= 0 && has_layer(U.held)) layer_key(k, 1); else if (steps_page() && !U.rec) step_key(k, 1); else key_down(k); }
            if (up >> k & 1u) {
                if (U.stepHeld >= 0 && U.stepKey == k) step_key(k, 0);
                else if (U.held >= 0 && has_layer(U.held)) layer_key(k, 0); else key_up(k);
            }
        }
        /* buttons: PLAY and OCT at once; the others: a tap = page, a hold = layer */
        b = fm1_input_edges(&rel);
        for (k = 0; k < 14u; k++) {
            if (b >> k & 1u) {
                if (k == BTN_PLAY) { if (E.playing) E.stopReq = 1; else E.startReq = 1; }
                else if (k == BTN_OCT_UP || k == BTN_OCT_DOWN) {
                    if (steps_page() && U.stepHeld >= 0) {
                        step_set_note((uint32_t)U.stepHeld, step_note((uint32_t)U.stepHeld, step_len()) + (k == BTN_OCT_UP ? 12 : -12));
                        U.stepEdited = 1; count_lanes(); changed(); if (live[0] >= 0) step_preview(1);
                    } else if (steps_page()) { if (k == BTN_OCT_UP) gen_steps(); else mutate_steps(); }
                    else if (U.group == G_SEQ) { if (k == BTN_OCT_UP) new_line(); else mutate_line(); }
                    else { int32_t o = (int8_t)U.rootOct + (k == BTN_OCT_UP ? 1 : -1); if (o >= -1 && o <= 1) U.rootOct = (uint8_t)(int8_t)o; { char s[4] = ""; sint(s, o); toast("OCT ", s); } }
                } else { U.held = (int8_t)k; U.heldT = fm1_ms; U.used = 0; }
            }
            if (rel >> k & 1u && U.held == (int8_t)k) {
                if (k == BTN_FX) fx_release_all();
                if (!U.used && fm1_ms - U.heldT < 450u) tap((int)k);
                U.held = -1;
            }
        }
        /* encoders */
        for (e = 0; e < 7u; e++) { int32_t d = fm1_enc_take(e); if (d) turn(e, d); }
        tick_rec();
        /* LEDs: the page's button, PLAY, REC */
        if (fm1_ms - ledT > 50u) {
            static const int8_t GBTN[G_N] = {BTN_HOME, BTN_EDIT, BTN_ENV, BTN_LFO, BTN_FX, BTN_SEL, BTN_ARP, BTN_SEQ, BTN_GLO};
            ledT = fm1_ms;
            for (k = 2; k < 12u; k++) fm1_led_key(k, (int)k == GBTN[U.group]);
            fm1_led_key(BTN_PLAY, E.playing && ((E.s & 3u) < 2u));
            fm1_led_key(BTN_REC, U.rec);
        }
        /* the screen: the graph about 30 times a second, the rest when it changes */
        if (fm1_ms - lastFrame >= 33u) {
            lastFrame = fm1_ms;
            int fxShow = U.held == BTN_FX && (fxBufKey >= 0 || X.flags || fxFn || riseStart >= 0 || fxThrow);
            if (U.held >= 0 && has_layer(U.held) && fm1_ms - U.heldT > 160u && !fxShow) draw_keymap();
            else if (steps_page() && !fxShow) draw_steps(); else draw_graph();
            draw_rows();
            if (fm1_ms - lastTop >= 100u || (U.touch >= 0 && fm1_ms - U.touchT < 900u)) { lastTop = fm1_ms; draw_top(); }
        }
        /* keep the settings when nothing has changed for 20 s and the music is stopped (an erase stalls the sound) */
        if (U.dirty && !E.playing && fm1_ms - U.dirtyT > 20000u) { settings_save(&P, sizeof P); U.dirty = 0; }
    }
}
