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
#define APP_VER "0.46"
#endif
#define CV_MAX (240u * 110u)
#include "platform.c"
#include "acid_tables.h"
/* T3's 909 kit: X0X's port of 9W9 / ER-99 (GPL-3.0), unchanged, in this one unit (float: the FPU, checked in 0.34) */
#include "dsp/drum909.c"
/* 0.41: and the 808 kit: X0X's port of 8W8 (GPL-3.0; the rim shot sc808, MIT), unchanged */
#include "dsp/drum808.c"
#include "songs8.h"                              /* 0.39: the eight acid songs (tools/export_songs.mjs) */
#undef LIN
#undef EXP
#undef DRIVE
#undef DIST
#undef SENDS
#undef LEVEL
#undef PAN
#undef NP
#undef FITS

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
    /* 0.32: after the lanes, so a 0.31 save (which ends here) still loads */
    uint8_t aFilt;                               /* ACID's filter: 0 = 4-pole ladder (the 303's kind), 1 = 2-pole SVF */
    uint8_t mot[4][64];                          /* knob motion over 4 bars: CUTOFF RESO ENV DECAY, 255 = none */
    /* 0.33 */
    uint16_t accHi, sldHi;                       /* ACID steps 17-32: accent, slide */
    /* 0.35: T3's kit and its drum machine (909: 11 rows of 16 steps, each with 4 knobs) */
    uint8_t kit, drSel;                          /* 0 = FUNC (the function kick), 1 = 909; the row being edited */
    uint16_t drRow[11], drAcc[11], drMute;       /* steps on, accented; muted rows */
    uint8_t drPot[11][4];                        /* KNOB1-4 of each row: TUNE, DECAY, LEVEL, one more (see KN9) */
    /* 0.36: USB-MIDI */
    uint8_t mClk, mNote, mCh;                    /* clock OFF / OUT / IN; notes out OFF / ON; ACID's channel - 1 (BIO next, drums on 10) */
    /* 0.39 */
    uint8_t bootDemo, demoSong;                  /* at power-on: play a song (until STOP); which: 0 = any, 1-8 */
    /* 0.41: T2 ZOO's 303 (VOICE is bAnimal: 0 ACID B, 1-6 the calls, 7-10 the old BIO animals) */
    uint8_t zCut, zRes, zEnv, zDec;
} params_t;
static params_t P;
/* what is kept in flash: the current state and 6 slots (SAVE held: white keys 1-6) */
#define NSLOT 6u
typedef struct { params_t cur; params_t slot[NSLOT]; uint8_t used[8]; } store_t;
_Static_assert(sizeof(store_t) <= 4084u, "the settings sector holds 4 KiB");
static store_t ST;
static int32_t duckEnv;                                     /* the 909 BD's push on the duck, Q15 */
static uint32_t accB(void) { return P.accM | (uint32_t)P.accHi << 16; }          /* the 32 steps' accents */
static uint32_t sldB(void) { return P.sldM | (uint32_t)P.sldHi << 16; }
static void setAccB(uint32_t v) { P.accM = (uint16_t)v; P.accHi = (uint16_t)(v >> 16); }
static void setSldB(uint32_t v) { P.sldM = (uint16_t)v; P.sldHi = (uint16_t)(v >> 16); }
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
typedef struct { const char *name; uint8_t v[14]; } preset_t;
/* ACID: aWave aSub aTune aLvl res env aDrive aDec aAcc aGate aGlide */
static const preset_t PRE_ACID[8] = {
    {"TB CLASSIC", {0, 0, 0, 100, 88, 72, 30, 60, 70, 70, 0}},   {"SQUARE 303", {1, 0, 0, 92, 80, 64, 40, 50, 60, 65, 20}},
    {"SQUELCH AA", {0, 0, 0, 96, 104, 110, 50, 30, 80, 75, 0}},  {"RUBBER", {0, 90, 0, 100, 50, 46, 20, 90, 40, 85, 50}},
    {"SCREAM", {0, 0, 0, 82, 112, 100, 110, 40, 100, 60, 10}},   {"DEEP SUB", {1, 110, 0, 108, 40, 30, 10, 70, 30, 80, 30}},
    {"GLASS", {2, 0, 1, 86, 96, 90, 20, 20, 60, 40, 0}},         {"DIRTY S+Q", {3, 40, 0, 90, 70, 80, 90, 55, 70, 70, 15}}};
/* 0.41: T2 ZOO, a second 303 that also does animals (acid-cat). bAnimal(VOICE) bPer(CALL every) bPhase bQuant bTune
 * bDrift bNoise bBright bLvl zCut zRes zEnv zDec. ACID B: an arpeggio of the chord in 8ths; the calls: one every bar
 * (or two), on the root; the last two: the old BIO wavetables */
static const preset_t PRE_BIO[8] = {
    {"ACID B", {0, 0, 0, 0, 12, 32, 20, 90, 84, 52, 96, 80, 40}},   {"MEOW", {1, 3, 8, 0, 0, 32, 20, 90, 96, 64, 100, 0, 40}},
    {"MEW MEW", {2, 2, 4, 0, 0, 32, 20, 90, 92, 64, 100, 0, 40}},  {"MOO", {3, 4, 0, 0, 0, 32, 20, 90, 100, 64, 100, 0, 40}},
    {"QUACK", {4, 3, 12, 0, 0, 32, 20, 90, 90, 64, 100, 0, 40}},   {"WOOF", {5, 3, 0, 0, 0, 32, 20, 90, 96, 64, 100, 0, 40}},
    {"HOUSEFLY", {7, 3, 0, 0, 0, 32, 30, 90, 60, 64, 100, 0, 40}}, {"WHALE SONG", {10, 4, 0, 127, 12, 127, 6, 40, 110, 64, 100, 0, 40}}};
/* KICK: kTop kTone kClick kDrive kPdec kBody kBase kDuck */
static const preset_t PRE_KICK[6] = {
    {"CLEAN", {36, 0, 20, 20, 38, 60, 0, 80}},     {"909", {30, 0, 70, 70, 50, 55, 2, 70}},
    {"808 LONG", {20, 0, 0, 10, 20, 110, 254, 100}}, {"ZAP", {48, 20, 40, 40, 90, 40, 0, 60}},
    {"AA RAW", {25, 127, 0, 30, 38, 60, 12, 127}},  {"THUD", {12, 0, 10, 50, 30, 70, 251, 90}}};
/* 0.41: T4 KEYS (was PERC; the same five bytes): hType = SOUND, hTone = TONE, hDec = DECAY, hPat = RHYTHM, hLvl */
static const preset_t PRE_PERC[8] = {
    {"WARM PAD", {0, 50, 70, 0, 80}},  {"PLUCK", {1, 80, 50, 4, 78}},  {"GUITAR", {2, 64, 70, 3, 82}},
    {"E.PIANO", {3, 60, 64, 2, 86}},   {"BELL", {4, 76, 90, 1, 64}},   {"PIANO", {5, 70, 70, 1, 86}},
    {"DRONE", {6, 60, 110, 0, 72}},    {"PLUCK ARP", {1, 96, 40, 5, 70}}};
static const uint8_t NPRE[4] = {8, 8, 6, 8};
static const char *pre_name(uint32_t t, uint32_t k)
{
    return t == 0 ? PRE_ACID[k].name : t == 1 ? PRE_BIO[k].name : t == 2 ? PRE_KICK[k].name : PRE_PERC[k].name;
}
static void apply_preset(uint32_t t, uint32_t k)
{
    const uint8_t *v;
    k %= NPRE[t]; P.pre[t] = (uint8_t)k;
    if (t == 0) { v = PRE_ACID[k].v; P.aWave = v[0]; P.aSub = v[1]; P.aTune = (int8_t)v[2]; P.aLvl = v[3]; P.res = v[4]; P.env = v[5]; P.aDrive = v[6]; P.aDec = v[7]; P.aAcc = v[8]; P.aGate = v[9]; P.aGlide = v[10]; }
    else if (t == 1) { v = PRE_BIO[k].v; P.bAnimal = v[0]; P.bPer = v[1]; P.bPhase = v[2]; P.bQuant = v[3]; P.bTune = (int8_t)v[4]; P.bDrift = v[5]; P.bNoise = v[6]; P.bBright = v[7]; P.bLvl = v[8];
        P.zCut = v[9]; P.zRes = v[10]; P.zEnv = v[11]; P.zDec = v[12]; P.terms |= T_BIO; }
    else if (t == 2) { v = PRE_KICK[k].v; P.kTop = v[0]; P.kTone = v[1]; P.kClick = v[2]; P.kDrive = v[3]; P.kPdec = v[4]; P.kBody = v[5]; P.kBase = (int8_t)v[6]; P.kDuck = v[7]; P.terms |= T_KICK; }
    else { v = PRE_PERC[k].v; P.hType = v[0]; P.hTone = v[1]; P.hDec = v[2]; P.hPat = v[3]; P.hLvl = v[4]; P.terms |= T_PERC; }
}
/* ---- the 909 rows: names, and which of 9W9's params KNOB1-4 reach (TUNE, DECAY, LEVEL, one more; -1 none) */
static const char *const DRN[11] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP", "CH", "OH", "CR", "RD"};
static const int8_t KN9[11][4] = {{0, 2, 3, 1}, {0, 3, 4, 2}, {0, 1, 2, 3}, {0, 1, 2, 3}, {0, 1, 2, 3}, {1, -1, 0, 2},
                                  {1, 2, 0, 3}, {2, 0, 1, 3}, {2, 0, 1, 3}, {0, 2, 1, 3}, {0, 2, 1, 3}};
/* 0.41: the 808 on the same 11 rows (8W8's tracks: no ride, so the last row is the cowbell); LT MT HT switch
 * tom / conga, RS rim / clave, CP clap / maracas (KNOB4). KNOB1-4: TUNE, DECAY, LEVEL, one more, as on the 909 */
static const char *const DRN8[11] = {"BD", "SD", "LT", "MT", "HT", "RS", "CP", "CH", "OH", "CY", "CB"};
static const uint8_t T8[11] = {D8_BD, D8_SD, D8_LT, D8_MT, D8_HT, D8_RS, D8_CP, D8_CH, D8_OH, D8_CY, D8_CB};
static const int8_t KN8[11][4] = {{3, 2, 0, 1}, {3, 4, 0, 2}, {1, 2, 0, 3}, {1, 2, 0, 3}, {1, 2, 0, 3}, {1, 2, 0, 3},
                                  {1, 2, 0, 4}, {1, 2, 0, -1}, {2, 1, 0, -1}, {2, 1, 0, -1}, {1, 2, 0, -1}};
static const char *drn(uint32_t v) { return (P.kit == 2u ? DRN8 : DRN)[v % 11u]; }
static const x0x_param_t *dk_param(uint32_t kit, uint32_t v, uint32_t i)   /* KNOB i of row v on this kit */
{
    int32_t ix = kit == 2u ? KN8[v][i] : KN9[v][i];
    if (ix < 0) return 0;
    return kit == 2u ? drum808_param(T8[v], ix) : drum909_param((int)v, ix);
}
static void drum_pots(params_t *q)                   /* the kit's own knob defaults */
{
    uint32_t v, i;
    for (v = 0; v < 11u; v++) for (i = 0; i < 4u; i++) { const x0x_param_t *pp = dk_param(q->kit, v, i); q->drPot[v][i] = pp ? pp->def : 0; }
    if (q->kit != 2u) q->drPot[6][2] = 70;                                       /* the 909 clap: well under 9W9's 122 */
    else {                                           /* the 808: each row's LEVEL (linear, 64 = x1) set so it sounds as loud as the 909's row */
        static const uint8_t L8[11] = {127, 96, 127, 127, 127, 101, 50, 40, 24, 51, 69};
        for (v = 0; v < 11u; v++) q->drPot[v][2] = L8[v];
    }
}
static void drum_defaults(params_t *q)
{
    uint32_t v, i;
    q->kit = 0; q->drSel = 0; q->drMute = 0;
    memset(q->drRow, 0, sizeof q->drRow); memset(q->drAcc, 0, sizeof q->drAcc);
    q->drRow[0] = 0x1111u; q->drRow[1] = 0x1010u; q->drRow[7] = 0x4444u;          /* BD 4/4, SD 2 and 4, CH off-beats */
    q->drAcc[0] = 0x0001u;
    (void)v; (void)i;
    drum_pots(q);
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
    P.bootDemo = 1; P.demoSong = 0;
    for (t = 0; t < 4u; t++) for (i = 0; i < LANE; i++) P.lane[t][i] = NONE;
    for (t = 0; t < 4u; t++) apply_preset(t, 0);
    apply_chapter(0);
    /* the start: a blank page that still pulses. ACID plays its root on all 16 steps (yours to write over),
     * a four-to-the-floor kick and an off-beat hat; no animal, no song mode */
    P.aSrc = 0; P.sLen = 16; P.sTrans = 0; setAccB(0); setSldB(0);
    for (i = 0; i < 16u; i++) P.lane[0][i] = (uint8_t)P.root;
    P.terms = (uint16_t)((P.terms | T_FOUR) & ~(T_BIO | T_PERC));   /* 0.41: ZOO and KEYS wait for a key; the 909's hats instead of PERC */
    P.cut = 40;
    P.aFilt = 0;
    memset(P.mot, 255, sizeof P.mot);
    drum_defaults(&P);
    P.kit = 1; drum_pots(&P);
}
/* ---- the patterns: a 16-step line and its sound, ready to play; then yours to change. HOME held: white keys 9-16;
 * on SEQ > STEPS the PRESETS knob walks them. Steps: {semitones above the root, accent, slide, 0 note / 1 rest / 2 tie} */
#define PN_ 0
#define PR_ 1
#define PT_ 2
typedef struct {
    const char *name, *lab;
    int8_t st[16][4];
    uint8_t bpm, cut, res, env, dec, acc, gate, drive, dMix, dFb, dTone, rMix, hPat;
} pattern_t;
static const pattern_t PATS[8] = {
    {"BLANK", "BLANK", {{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},
                        {0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0},{0,0,0,0}},
     120, 40, 88, 72, 30, 70, 70, 30, 60, 54, 64, 40, 0},
    /* the acid film's drop (function-sounds/video/acid-film.html, 0:48-0:56) */
    {"FILM", "FILM", {{0,1,0,0},{0,0,0,0},{12,0,1,0},{0,0,0,0},{3,0,0,0},{0,0,0,1},{12,1,0,0},{10,0,1,0},
                      {0,0,0,0},{0,1,0,0},{7,0,0,0},{12,0,1,0},{12,0,0,0},{3,0,0,1},{15,1,0,0},{0,0,0,0}},
     120, 80, 100, 68, 18, 110, 50, 30, 52, 54, 64, 40, 1},
    /* a rolling 16th line, octave jumps */
    {"ROLLER", "ROLL", {{0,1,0,0},{0,0,0,0},{12,0,1,0},{0,0,0,0},{0,0,0,0},{12,1,0,0},{0,0,0,0},{0,0,0,0},
                        {0,1,0,0},{0,0,0,0},{12,0,1,0},{0,0,0,0},{3,0,0,0},{0,0,0,0},{12,1,1,0},{10,0,0,0}},
     128, 70, 92, 70, 24, 100, 45, 40, 40, 40, 64, 30, 1},
    /* sparse, long slides, a lot of resonance, a long decay */
    {"SQUELCH", "SQLCH", {{0,1,0,0},{0,0,0,2},{12,0,1,0},{15,0,0,0},{0,0,0,1},{12,1,1,0},{7,0,0,0},{7,0,0,2},
                          {0,0,0,0},{0,0,0,1},{3,1,1,0},{10,0,0,0},{12,0,0,0},{12,0,0,2},{15,1,0,0},{0,0,0,1}},
     124, 60, 112, 90, 60, 120, 60, 50, 52, 54, 64, 50, 1},
    /* few notes on the off-grid, made for the delay */
    {"DUB 3/16", "DUB", {{0,1,0,0},{0,0,0,1},{0,0,0,1},{0,0,0,0},{0,0,0,1},{0,0,0,1},{12,0,1,0},{10,0,0,0},
                         {0,0,0,1},{0,0,0,1},{7,1,0,0},{0,0,0,1},{0,0,0,1},{3,0,1,0},{0,0,0,0},{0,0,0,1}},
     118, 74, 96, 60, 40, 90, 55, 30, 90, 72, 50, 60, 0},
    /* a minor arpeggio up and down */
    {"ARP MINOR", "ARP", {{0,1,0,0},{3,0,0,0},{7,0,0,0},{12,0,1,0},{15,0,0,0},{12,0,0,0},{7,0,0,0},{3,0,0,0},
                          {0,1,0,0},{3,0,0,0},{7,0,0,0},{12,0,0,0},{17,1,1,0},{15,0,0,0},{12,0,0,0},{10,0,1,0}},
     130, 66, 100, 76, 30, 100, 50, 40, 45, 50, 64, 35, 1},
    /* two notes, ties and rests: the hypnotic kind */
    {"HYPNO", "HYPNO", {{0,1,0,0},{0,0,0,2},{0,0,0,1},{10,0,0,0},{0,0,0,0},{0,0,0,1},{0,1,0,0},{0,0,0,1},
                        {0,0,0,0},{0,0,0,2},{0,0,0,1},{10,0,1,0},{12,0,0,0},{0,0,0,1},{0,1,0,0},{0,0,0,1}},
     125, 56, 104, 84, 48, 100, 50, 40, 56, 60, 64, 40, 0},
    /* fast, octaves, screaming */
    {"RAVE", "RAVE", {{0,1,0,0},{12,0,0,0},{0,0,0,0},{12,1,0,0},{0,0,0,0},{12,0,1,0},{15,0,0,0},{12,0,0,0},
                      {0,1,0,0},{12,0,0,0},{0,0,0,0},{12,0,0,0},{3,1,1,0},{15,0,0,0},{10,0,1,0},{12,0,0,0}},
     136, 78, 110, 100, 20, 120, 40, 90, 35, 40, 64, 25, 1},
};
#define NPATS 8u
static uint8_t patNow;
static void load_pattern(uint32_t k)
{
    const pattern_t *q = &PATS[k % NPATS];
    uint32_t i;
    patNow = (uint8_t)(k % NPATS);
    P.aSrc = 0; P.sLen = 16; P.sTrans = 0; setAccB(0); setSldB(0); P.autoSong = 0;
    for (i = 0; i < LANE; i++) P.lane[0][i] = NONE;
    for (i = 0; i < 16u; i++) {
        const int8_t *t = q->st[i];
        if (t[3] == PN_) P.lane[0][i] = (uint8_t)(P.root + t[0]);
        else if (t[3] == PT_ && i) P.lane[0][i] = (uint8_t)((P.root + t[0]) | 128);
        if (t[1]) setAccB(accB() | 1u << i);
        if (t[2]) setSldB(sldB() | 1u << i);
    }
    P.bpm = q->bpm; P.aWave = 0; P.aSub = 0; P.aTune = 0; P.aLvl = 100; P.cut = q->cut; P.res = q->res; P.env = q->env;
    P.aDec = q->dec; P.aAcc = q->acc; P.aGate = q->gate; P.aGlide = 0; P.aDrive = q->drive;
    P.dTime = 2; P.dFb = q->dFb; P.dMix = q->dMix; P.dTone = q->dTone; P.rMix = q->rMix;
    P.kDuck = 40; P.hPat = q->hPat; P.terms |= T_KICK | T_FOUR; P.terms &= (uint16_t)~(T_BIO | T_PERC); P.mutes = 0;
    memset(P.mot, 255, sizeof P.mot);                                  /* a new line: no old knob motion */
}
/* the meow the 303's own way (acid-cat test, HOME + black key 1): FILM's first half, then C E E D C A an octave up, slid,
 * and the mouth recorded in the knob motion lanes: CUTOFF opens and closes, RESO 0.84, no ENV. Off the call the lanes
 * are empty, so the knobs still play the bass. (The lane tops out near 2 kHz, CUTOFF all the way up: the film opens to 2.5 kHz.) */
static void mot_count(void);
static void load_meow_line(void)
{
    static const uint8_t ST6[6] = {3, 7, 7, 5, 3, 0}, CUT7[7] = {85, 117, 127, 127, 114, 100, 89};
    uint32_t i, b;
    load_pattern(1);
    setSldB(sldB() & ~(1u << 7));                                    /* no slide into the call */
    for (i = 0; i < 6u; i++) P.lane[0][8u + i] = (uint8_t)(P.root + 12 + ST6[i]);
    P.lane[0][14] = NONE; P.lane[0][15] = (uint8_t)P.root;
    setAccB(accB() & 0xFFu); setSldB((sldB() & 0xFFu) | 0x1F00u);   /* steps 9-13 slide on */
    for (b = 0; b < 4u; b++)
        for (i = 0; i < 7u; i++) {
            P.mot[0][b * 16u + 8u + i] = CUT7[i];
            if (i < 6u) { P.mot[1][b * 16u + 8u + i] = 107; P.mot[2][b * 16u + 8u + i] = 0; }
        }
    mot_count();
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
static uint32_t step_len(void) { return P.sLen >= 1u && P.sLen <= 32u ? P.sLen : 16u; }
static int legato_into(uint32_t i, uint32_t L)     /* step i goes on from the step before it: no new attack */
{
    uint32_t v = P.lane[0][i], pi = (i + L - 1u) % L, pv = P.lane[0][pi];
    if (v == NONE || pv == NONE || L < 2u) return 0;
    return (v & 128u) || (sldB() >> pi & 1u);
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
    o->acc = (uint8_t)(accB() >> ((i + L - k) % L) & 1u);
    if (!(v & 128u) && legato_into(i, L)) {          /* slid into: glide from the note before (RC, tau 22 ms + GLIDE) */
        int32_t pn = step_note((i + L - 1u) % L, L), tau = 22 + (P.aGlide >> 1);
        uint32_t x = p16 * (15000u / P.bpm) / (256u * (uint32_t)tau);
        o->y += (((pn - n) << 8) * expneg((int32_t)(x > 2047u ? 2047u : x))) >> 15;
    }
    o->slide = (uint8_t)((sldB() >> i & 1u) && nv != NONE);
    o->gate = o->slide || (nv != NONE && (nv & 128u) && L > 1u) || p16 < P.aGate * 655u;
    o->y += acid_mods(s, p16);
}

/* ================================================================ 0.39: song mode
 * The eight songs of acid-8-tracks (tools/songs/songs8.js): each bar's 303 line(s), 909 rows and the knob lanes come from
 * firmware/gen/songs8.h. A song plays from INTRO to OUTRO; the player turns the knobs as the song was written (HAND),
 * until a knob is turned on the device: then that knob is yours until the next section. STOP = back to your own set. */
typedef struct { uint8_t v[48], f[48]; } win_t;      /* 3 bars of a 303 line (the bar before, this one, the next), lane-style */
static win_t WA, WB;
static volatile uint32_t wBase;                      /* the E step at WA.v[0] */
static params_t PB;                                  /* your own set, kept while a song plays */
static struct {
    volatile uint8_t on, ended, loop, demo;
    uint8_t song, own, sec, gapNext;
    int32_t off; volatile int32_t jump;              /* song bar = E bar + off; a jump (song bar) waits for the next bar */
    uint32_t curEb; int32_t curSb;                   /* the E bar whose song bar was decided last, and that song bar */
    int32_t k[4];                                    /* the lanes' CUTOFF RESO ENV DECAY, in the knobs' units */
    int32_t bq8, bres, benv, bdec, gain;             /* the second 303's cutoff (Q8 MIDI), res, env, decay ms; output gain Q15 */
    float dhpA, rise;                                /* the drums' high-pass, the riser 0..1 */
    float padLvl, padBri, padPump, tpLvl;            /* DEEP's pads, RAGA's tanpura (the songs' own T4) */
} SG = {.curEb = 0xFFFFFFFFu, .jump = -1, .gain = 32767};
static struct { uint32_t ph, inc; int32_t amp, ampT, y1, y2, y3, y4, g, kq, drv, panL, send; uint32_t ctr; uint8_t on, wave; } V2;   /* the second 303: a song's, or T2 ZOO */
static struct { float lp, bp; uint32_t n; } RZ;                                                         /* the riser's noise */
static float dhpX, dhpY;
static void drum_masks(uint32_t s, uint16_t *on, uint16_t *ac);
static inline const sg_song_t *sgs(void) { return &SONGS[SG.song % SG_N]; }
static void win_fill(win_t *w, uint32_t voice, int32_t sb)        /* sb: the song bar (0-based) at w's middle */
{
    uint32_t k, j;
    const sg_song_t *g = sgs();
    for (k = 0; k < 3u; k++) {
        int32_t b = sb - 1 + (int32_t)k;
        uint32_t idx = b >= 0 && b < (int32_t)g->nb ? SG_BARS[((uint32_t)g->first + (uint32_t)b) * 3u + voice] : 0xFFFFu;
        for (j = 0; j < 16u; j++) {
            uint32_t i = k * 16u + j;
            if (idx == 0xFFFFu) { w->v[i] = NONE; w->f[i] = 0; continue; }
            {
                int32_t n = SG_ACID[idx * 32u + j * 2u]; uint32_t f = (uint8_t)SG_ACID[idx * 32u + j * 2u + 1u];
                w->v[i] = (f & 4u) ? NONE : (uint8_t)((33 + n) | ((f & 8u) ? 128 : 0));
                w->f[i] = (uint8_t)(f & 19u);
            }
        }
    }
}
static int32_t sg_sec_of(int32_t sb)                 /* the section (index) of song bar sb (0-based) */
{
    const sg_song_t *g = sgs(); int32_t i, r = 0;
    for (i = 0; i < g->nsec; i++) if (sb + 1 >= g->secBar[i]) r = i;
    return r;
}
static int32_t sg_map(uint32_t eb)                   /* the song bar of E bar eb; at a new bar: jumps, loops, the end (ISR only) */
{
    const sg_song_t *g = sgs();
    int32_t sb;
    if (eb == SG.curEb) return SG.curSb;
    sb = (int32_t)eb + SG.off;
    if (SG.jump >= 0) { sb = SG.jump; SG.jump = -1; }
    else if (SG.loop && SG.curSb >= 0) {                                    /* LOOP: at the section's end, back to its start */
        int32_t sc = sg_sec_of(SG.curSb), end = sc + 1 < g->nsec ? g->secBar[sc + 1] - 1 : (int32_t)g->nb - 1;
        if (sb >= end) sb = g->secBar[sc] - 1;
    }
    SG.off = sb - (int32_t)eb;
    if (sb >= (int32_t)g->nb) { SG.ended = 1; sb = (int32_t)g->nb - 1; }
    if (sg_sec_of(sb) != SG.sec) { SG.sec = (uint8_t)sg_sec_of(sb); SG.own = 0; }   /* a new section: the lanes take the knobs back */
    {
        uint32_t i; SG.gapNext = 0;
        for (i = 0; i < g->ngap; i++) if (g->gap[i] == sb + 2) SG.gapNext = 1;       /* the bar before a gap bar: its last 16th is silent */
    }
    SG.curEb = eb; SG.curSb = sb;
    wBase = eb * 16u - 16u;
    win_fill(&WA, 0, sb); win_fill(&WB, 1, sb);
    return sb;
}
static int win_leg(const win_t *w, uint32_t i)
{
    uint32_t v = w->v[i], pv = w->v[i - 1u];
    if (v == NONE || pv == NONE) return 0;
    return (v & 128u) || (w->f[i - 1u] & 2u);
}
static int32_t win_note(const win_t *w, uint32_t i)
{
    for (;; i--) { uint32_t v = w->v[i]; if (v != NONE && !(v & 128u)) return (int32_t)v; if (!i) return 33; }
}
static void eval_win(const win_t *w, uint32_t s, uint32_t p16, acid_t *o, int32_t tr, uint32_t gate)
{
    int32_t rel = (int32_t)s - (int32_t)wBase, n;
    uint32_t i, k, v, nv;
    o->acc = o->slide = o->rest = 0; o->gate = 0; o->age = 0x7FFFFFFFu; o->y = (33 + tr) << 8;
    if (!run || rel < 1 || rel > 46) { o->rest = 1; return; }
    i = (uint32_t)rel; v = w->v[i]; nv = w->v[i + 1u];
    n = win_note(w, i); o->y = (n + tr) << 8;
    if (v == NONE) { o->rest = 1; return; }
    for (k = 0; k + 1u < i && win_leg(w, i - k); k++) { }
    o->age = (k << 16) + p16;
    o->acc = (uint8_t)(w->f[i - k] & 1u);
    if (!(v & 128u) && win_leg(w, i)) {
        int32_t pn = win_note(w, i - 1u);
        uint32_t x = p16 * (15000u / P.bpm) / (256u * 22u);
        o->y += (((pn - n) << 8) * expneg((int32_t)(x > 2047u ? 2047u : x))) >> 15;
    }
    o->slide = (uint8_t)((w->f[i] & 2u) && nv != NONE);
    if ((w->f[i] & 16u) && k == 0u) {                                       /* two hits in the step */
        if (p16 >= 32768u) o->age = p16 - 32768u;
        o->gate = p16 < 27525u || (p16 >= 32768u && p16 < 60293u);
    } else o->gate = o->slide || (nv != NONE && (nv & 128u)) || p16 < gate * 655u;
}

static void eval_acid0(uint32_t s, uint32_t p16, acid_t *o, int noglide)
{
    uint32_t t = P.terms, N = P.N, i = (s + P.off) % N, c = ((s + 32u) / N) >> P.arpSpd;
    int32_t base = P.root, y, laneRest = 0, legato = 0;
    trig_t tg;
    if (SG.on) { int32_t tr = P.sTrans; if (live[0] >= 0 && !recNow && run) tr += live[0] - P.root; eval_win(&WA, s, p16, o, tr, P.aGate); return; }
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
/* knob motion (X0X's idea): with REC on and the music playing, turning CUTOFF / RESO / ENV / DECAY writes the value
 * into its 64-step lane (4 bars); playing back, the lane moves the knob, smoothly from step to step. Turning a knob
 * takes over from its lane while you turn it. REC held + white key 5 clears all four. */
static const uint8_t MOT_OFF[4] = {__builtin_offsetof(params_t, cut), __builtin_offsetof(params_t, res),
                                   __builtin_offsetof(params_t, env), __builtin_offsetof(params_t, aDec)};
static volatile uint32_t motTouch[4];
static uint8_t motAny;                                                   /* bit m: lane m has values */
static int32_t mv(uint32_t m, uint32_t s, uint32_t p16)                  /* the value now: the lane's, or the knob's */
{
    uint32_t i = s & 63u, a = P.mot[m][i], b;
    if (SG.on && !(SG.own >> m & 1u)) return SG.k[m];
    if (a == 255u || !run || fm1_ms - motTouch[m] < 500u) return ((const uint8_t *)&P)[MOT_OFF[m]];
    b = P.mot[m][(i + 1u) & 63u];
    if (b == 255u) b = a;
    return (int32_t)((a * (65536u - p16) + b * p16) >> 16);
}
static void mot_count(void)
{
    uint32_t m, i;
    motAny = 0;
    for (m = 0; m < 4u; m++) for (i = 0; i < 64u; i++) if (P.mot[m][i] != 255u) { motAny |= (uint8_t)(1u << m); break; }
}
static uint32_t tau_ms(uint32_t dec)             /* the steps' DECAY: 200 ms .. 2 s, as on a 303 (log) */
{
    static const uint16_t TAU[17] = {200, 233, 272, 316, 369, 430, 501, 584, 681, 794, 925, 1078, 1257, 1465, 1707, 1990, 2000};
    uint32_t i = dec >> 3, f = dec & 7u;
    return (TAU[i] * (8u - f) + TAU[i + 1u] * f) >> 3;
}
static int32_t cutoff_at(const acid_t *o, uint32_t s, uint32_t p16)
{
    int32_t base = (24 + ((mv(0, s, p16) * 72) >> 7)) << 8, e, amt;
    if (P.lDep && P.lDest == 0) base += ((lfo_at(s, p16) * P.lDep) >> 15) * 6144 / 127;
    {   /* the filter envelope runs from the attack (over ties and slides); on the steps an accent closes fast, as on a 303 */
        uint32_t a = o->age > (24u << 16) ? (24u << 16) : o->age, x;
        if (STEPS || SG.on) {                            /* e^(-t / tau): kk = 64 / tau in steps; an accent: 200 ms */
            uint32_t kk = 960000u / ((o->acc ? 200u : tau_ms((uint32_t)mv(3, s, p16))) * P.bpm);
            x = (a * (kk ? kk : 1u)) >> 14;
        } else x = (a * (2u + ((127u - (uint32_t)mv(3, s, p16)) >> 4))) >> 8;
        e = expneg((int32_t)(x > 4095u ? 4095u : x));
    }
    amt = ((mv(2, s, p16) * 48) >> 7) + (o->acc ? (P.aAcc * 16) >> 7 : 0);
    if (fxFn & FN_SQUELCH) { base += 10 << 8; amt += 14; }
    return base + ((amt * e * 256) >> 15);
}

/* ---- animal calls (acid-cat test), after function-sounds/video/acid-cat.html: the resonant peak is one formant that
 * moves (the mouth), the steps and slides are the call's pitch curve. FX held + a black key: one call over the line,
 * from the next 16th while it plays (at once when stopped), with its own CUTOFF curve (in Hz: it does not follow the
 * pitch, so a low call keeps its mouth), RESO, envelope and wave; then the line goes on. Black keys 1-5: low (the
 * "bass meow"), 6-10: the animal's own register, 11: a long meow (a bar). Pitches: semitones above the root. */
typedef struct {
    const char *name, *lab;
    uint8_t n, div, oct, lowOct, wave, res, envS, envMs;   /* steps (div 2: 32nds); octaves above the root; wave 255: the line's */
    int8_t st[16];
    uint16_t acc, sld, rest;
    uint8_t nc; uint16_t ct[6]; int16_t cq[6];             /* CUTOFF by hand: at 1/16 steps, a Q8 note (log Hz) */
} call_t;
static const call_t CALLS[6] = {
    /* C E E D C A, slid, the accent on the E: the mouth opens 520 -> 2500 Hz while the pitch is highest, then closes */
    {"MEOW", "MEOW", 6, 1, 3, 1, 255, 107, 0, 0, {3, 7, 7, 5, 3, 0}, 0x2, 0x1F, 0,
     5, {0, 24, 51, 77, 96}, {18404, 25364, 24375, 20836, 19039}},          /* 520 2500 2000 900 600 Hz */
    {"MEW", "MEW", 3, 1, 3, 1, 255, 107, 0, 0, {3, 7, 3}, 0x2, 0x3, 0,
     4, {0, 12, 36, 48}, {19039, 25183, 20836, 19039}},                     /* 600 2400 900 600 */
    /* C D (held) C A: low, the mouth opens slowly (m -> oo) */
    {"MOO", "MOO", 12, 1, 1, 0, 255, 91, 0, 0, {3, 5, 5, 5, 5, 5, 5, 5, 3, 0, 0, 0}, 0, 0x7FF, 0,
     5, {0, 38, 102, 154, 192}, {13703, 16389, 20836, 19393, 14978}},       /* 180 330 900 650 240 */
    /* square, a lot of resonance, short envelopes: four notes down, D C B A */
    {"QUACK", "QUACK", 8, 1, 2, 1, 1, 114, 19, 100, {5, 0, 3, 0, 2, 0, 0, 0}, 0x41, 0, 0xAA,
     2, {0, 128}, {19722, 19722}},                                          /* 700 */
    /* 32nds: accented, a fast envelope, D -> B; "woof woof ... woof". No noise, no rasp: only half a dog */
    {"WOOF", "WOOF", 16, 2, 2, 1, 3, 70, 26, 80, {5, 2, 0, 0, 5, 2, 0, 0, 0, 0, 0, 0, 5, 2, 0, 0}, 0x1011, 0x1011, 0xCFCC,
     2, {0, 128}, {17764, 17014}},                                          /* 450 380 */
    /* a bar: opens, stays open on the E, closes slowly */
    {"MEOW LONG", "LONG", 16, 1, 3, 1, 255, 109, 0, 0, {3, 7, 7, 7, 7, 7, 7, 5, 5, 5, 3, 3, 0, 0, 0, 0}, 0x2, 0x7FFF, 0,
     6, {0, 45, 122, 173, 218, 256}, {18404, 25364, 25089, 23100, 20314, 18404}},   /* 520 2500 2350 1500 800 520 */
};
static volatile int8_t callKey = -1, callReq = -1;          /* the black key sounding; the one waiting for the 16th */
static volatile uint32_t callT0;                            /* its start, Q16 steps */
static volatile uint8_t callWave = 255;
static const call_t *call_of(int32_t k, int *low) { *low = k < 5; return &CALLS[k < 5 ? k : k < 10 ? k - 5 : 5]; }
static int call_shape(const call_t *c, uint32_t pos, int32_t base, acid_t *o, int32_t *cq, int32_t *res)   /* pos: Q16 steps into the call */
{
    int32_t x;
    uint32_t u, j, ph, a, k, t, unitUs;
    u = pos * c->div; j = u >> 16; ph = u & 65535u;
    if (pos > (32u << 16) || j >= c->n) return 0;
    unitUs = 15000000u / ((uint32_t)P.bpm * c->div);
    o->slide = o->rest = 0;
    for (a = j; a > 0u && (c->rest >> a & 1u); a--) { }      /* a rest: the note before, gate closed */
    o->y = (base + c->st[a]) << 8;
    if (c->rest >> j & 1u) { o->gate = 0; o->acc = 0; o->age = 0x7FFFFFFFu; }
    else {
        for (a = j; a > 0u && (c->sld >> (a - 1u) & 1u) && !(c->rest >> (a - 1u) & 1u); a--) { }   /* back to the attack */
        if (a < j) {                                         /* slid into: the 303's glide, RC 22 ms */
            x = (int32_t)(ph * (unitUs / 1000u) / (256u * 22u));
            o->y += (((c->st[j - 1u] - c->st[j]) << 8) * expneg(x > 2047 ? 2047 : x)) >> 15;
        }
        o->acc = (uint8_t)(c->acc >> a & 1u);
        o->slide = (uint8_t)((c->sld >> j & 1u) && j + 1u < c->n && !(c->rest >> (j + 1u) & 1u));
        o->gate = o->slide || ph < 40000u;
        o->age = ((j - a) << 16) + ph;
    }
    t = pos >> 8;                                            /* 1/256 steps */
    for (k = 1; k + 1u < c->nc && t >= c->ct[k] * 16u; k++) { }
    if (t >= c->ct[k] * 16u) x = c->cq[k];
    else x = c->cq[k - 1u] + (int32_t)((c->cq[k] - c->cq[k - 1u]) * (int32_t)(t - c->ct[k - 1u] * 16u)) / (int32_t)((c->ct[k] - c->ct[k - 1u]) * 16u);
    if (c->envS && o->gate) {                               /* the envelope from the attack, e^(-t / tau) */
        uint32_t ms = ((o->age >> 6) * (unitUs / 1000u)) >> 10, q = ms * 256u / c->envMs;
        x += (c->envS * expneg((int32_t)(q > 4095u ? 4095u : q))) >> 7;
    }
    *cq = x; *res = c->res;
    return 1;
}
static int call_eval(uint32_t s, uint32_t p16, acid_t *o, int32_t *cq, int32_t *res)
{
    int32_t key = callKey;
    int low;
    const call_t *c;
    if (key < 0) return 0;
    c = call_of(key, &low);
    if (!call_shape(c, (s << 16) + p16 - callT0, P.root + P.sTrans + 12 * (low ? c->lowOct : c->oct), o, cq, res)) { callKey = -1; return 0; }
    return 1;
}

static void eval_kick(uint32_t s, uint32_t p16, voice_t *o)
{
    uint32_t t = P.terms, bar = s & 15u, kt = 0xFFFFFFFFu;
    int32_t note = 57, drop;
    trig_t tg;
    o->on = 0; o->amp = 0; o->kt = 0; o->y = P.root << 8;
    if (!(t & T_KICK) || P.kit) return;
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

/* 0.41: T2 ZOO's 303. Its notes come as BIO's did: your notes (EXACT), or its own clock (CALL every 2..64 steps,
 * PHASE), on the root or on your notes (FOLLOW), ARP walking the chord. ACID B: a plain 303 note each time (accent on
 * every 4th); MEOW..LONG: the acid-cat calls, the key's pitch as the call's root, CUTOFF moving the mouth */
typedef struct { acid_t o; int32_t cq, res; uint8_t wave; } zoo_t;
static int zoo_eval(uint32_t s, uint32_t p16, zoo_t *z)
{
    uint32_t mode = P.bAnimal, per = BIO_PER[P.bPer % 6u], age = 0, len, ci = 0;
    int32_t center, follow = -1;
    trig_t tg;
    memset(z, 0, sizeof *z); z->o.rest = 1; z->o.age = 0x7FFFFFFFu; z->wave = 255;
    if (!(P.terms & T_BIO) || mode >= 7u) return 0;
    if ((P.laneMode[1] == 1u && laneN[1]) ||                                   /* EXACT: your notes; a key: while held, and its call */
        (liveN[1] >= 0 && trig(1, s, p16, 31, &tg) && tg.live && (live[1] >= 0 || tg.age < ((mode ? 16u : 1u) << 16)))) {
        if (!trig(1, s, p16, 31, &tg)) return 0;
        center = (tg.note & 127) + P.bTune; age = tg.age; len = tg.len; ci = 0;
    } else {
        uint32_t sp;
        if (!run) return 0;
        if (laneN[1] && trig(1, s, p16, 31, &tg)) follow = tg.note & 127;        /* FOLLOW: your note is the root */
        sp = s + 1024u - P.bPhase;
        age = ((sp & (per - 1u)) << 16) + p16; ci = sp / per; len = per << 16;
        center = (follow >= 0 ? follow : P.root + 12) + P.bTune;
        if (P.bArp) {
            static const uint8_t UD[6] = {0, 1, 2, 3, 2, 1};
            uint32_t k = P.bArp == 1u ? (ci & 3u) : P.bArp == 2u ? UD[ci % 6u] : (hash32(ci + 99u) & 3u);
            if (k) center += ARP_SETS[P.arpSet % 6u][k - 1u];
        }
    }
    center += P.sTrans;
    if (mode == 0u) {                                /* ACID B */
        uint32_t a = age > (24u << 16) ? (24u << 16) : age, kk, q;
        int32_t e;
        z->o.rest = 0; z->o.y = center << 8; z->o.acc = (uint8_t)((ci & 3u) == 0u); z->o.age = age;
        z->o.gate = age < (len > (2u << 16) ? len / 2u : len * 3u / 5u);
        kk = 960000u / ((z->o.acc ? 200u : tau_ms(P.zDec)) * P.bpm); q = (a * (kk ? kk : 1u)) >> 14;
        e = expneg((int32_t)(q > 4095u ? 4095u : q));
        z->cq = 24 * 256 + P.zCut * 144 + ((P.zEnv * 88 * e) >> 15) + (z->o.acc ? 6 * 256 : 0);   /* as ACID: CUTOFF 0..127 = MIDI 24..95 */
        z->res = P.zRes; z->wave = 0;
        return 1;
    }
    {
        const call_t *c = &CALLS[mode - 1u];
        int32_t cq, res;
        if (!call_shape(c, age, center - 12 + 12 * ((int32_t)c->oct - 2), &z->o, &cq, &res)) return 0;
        z->cq = cq + (P.zCut - 64) * 24;             /* CUTOFF: the mouth an octave smaller .. bigger */
        res = res * ((int32_t)P.zRes + 28) >> 7; z->res = res > 120 ? 120 : res;
        z->wave = c->wave;
        return 1;
    }
}
static void eval_bio(uint32_t s, uint32_t p16, voice_t *o)
{
    uint32_t a = (P.bAnimal - 7u) & 3u, per = BIO_PER[P.bPer % 6u], drone = a < 2u;
    int32_t u = -1, center = 0, gateEnd = 32767, follow = -1, off = 0, amp = 0, y;
    trig_t tg;
    o->on = 0; o->amp = 0; o->y = 0; o->kt = 0;
    if (!(P.terms & T_BIO)) return;
    if (P.bAnimal < 7u) {                         /* 0.41: the 303 (ZOO): its gate and pitch, for the rows, the graph and MIDI */
        zoo_t z;
        if (zoo_eval(s, p16, &z) && z.o.gate) { o->on = 1; o->y = z.o.y; o->amp = z.o.acc ? 30000 : 24000; }
        return;
    }
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

static uint32_t perc_pat(uint32_t s)               /* 0.41: T4 KEYS plays a chord at step s (2), or one note of it (ARP: 1) */
{
    uint32_t j = s & 15u;
    if (laneN[3]) { uint32_t v = P.lane[3][s & (LANE - 1u)]; return v != NONE && !(v & 128u) ? 2u : 0u; }   /* your notes: a chord on each */
    switch (P.hPat) {
    case 0: return j == 0u ? 2u : 0u;                                   /* HOLD: a bar */
    case 1: return (j & 7u) == 0u ? 2u : 0u;                            /* HALF */
    case 2: return (j & 3u) == 0u ? 2u : 0u;                            /* BEATS */
    case 3: return (j & 3u) == 2u ? 2u : 0u;                            /* OFFBEAT stabs */
    case 4: return j == 0u || j == 3u || j == 6u || j == 8u || j == 11u || j == 14u ? 2u : 0u;   /* 3-3-2, twice */
    default: return 1u;                                                 /* ARP: every 16th, up the chord */
    }
}
static uint32_t perc_on(uint32_t s) { return (P.terms & T_PERC) && run ? perc_pat(s) : 0u; }

/* ---------------------------------------------------------------- the sound (audio ISR) */
#define DMASK 32767u
static int16_t dbufL[DMASK + 1u] __attribute__((section(".pool"))), dbufR[DMASK + 1u] __attribute__((section(".pool")));
static struct {
    volatile uint32_t s, stepPh;
    volatile uint8_t playing, chBar, startReq, stopReq;
    uint32_t sinc, ctr;
    uint32_t aPh, aSub, aInc; int32_t aAmp, aAmpT, lp, bp, f, damp, aDrv; uint8_t acc, kick_;
    int32_t y1, y2, y3, y4, g, kq;                           /* the 4-pole ladder */
    uint32_t bPh, bInc; int32_t bAmp, bAmpT, bLp, bK;
    uint32_t kPh, kInc; int32_t kAmp, kAmpT, kClk, kLp, kDrv, duck;
    uint32_t nz; int32_t hEnv, hDk, hA, hy, hx;
    int32_t cT, cBurst, cF, cLp, cBp; uint32_t cPh, cInc, cPulse, cCnt, cN;
    uint32_t didx, dlen; int32_t dlp, dk;
    uint8_t tick;
} E = {.nz = 22222u, .cT = -1};

enum { PB_NONE, PB_L4, PB_L8, PB_L16, PB_L32, PB_REV, PB_STOP };
enum { PF_LP = 1, PF_HP = 2, PF_CRUSH = 4 };
/* a small room after the delay (Freeverb's tunings: 4 combs with damping, 2 all-passes; mono), ~1.6 s */
static const uint16_t RV_LEN[6] = {1116, 1188, 1277, 1356, 556, 441};
static int16_t rvBuf[1116 + 1188 + 1277 + 1356 + 556 + 441] __attribute__((section(".pool")));
static struct { uint16_t idx[6], off[6]; int32_t lp[4]; uint8_t ready; } RV;
static inline int32_t rz(int32_t x, int n) { return (x + ((x >> 31) & ((1 << n) - 1))) >> n; }   /* >> n rounding toward 0: feedback loops decay to 0, not to -1 */
static inline int32_t clip16(int32_t x) { return x > 32767 ? 32767 : x < -32767 ? -32767 : x; }   /* a feedback path: gain 1 */
static int32_t reverb(int32_t in)
{
    int32_t acc = 0, c;
    if (!RV.ready) { uint32_t o = 0; for (c = 0; c < 6; c++) { RV.off[c] = (uint16_t)o; o += RV_LEN[c]; } RV.ready = 1; }
    for (c = 0; c < 4; c++) {
        int16_t *b = rvBuf + RV.off[c];
        int32_t y = b[RV.idx[c]];
        RV.lp[c] += rz((y - RV.lp[c]) * 19660, 15);                         /* damping: the highs die first */
        b[RV.idx[c]] = (int16_t)clip16(in + rz(RV.lp[c] * 29040, 15));        /* g 0.886: about 1.6 s */
        if (++RV.idx[c] >= RV_LEN[c]) RV.idx[c] = 0;
        acc += y;
    }
    acc = rz(acc, 2);
    for (c = 4; c < 6; c++) {
        int16_t *b = rvBuf + RV.off[c];
        int32_t y = b[RV.idx[c]], o = y - acc;
        b[RV.idx[c]] = (int16_t)clip16(acc + rz(y, 1));
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
/* MIDI out: the ISR queues, the main loop sends (midi_send is main-loop only). Notes: ACID on mCh+1, BIO on mCh+2,
 * T3 (FUNC kick or the 909, GM drum notes) and PERC on 10; ACID's accents velocity 120, else 90 */
#define MQN 128u
static volatile uint32_t mqOut[MQN], mqW, mqR;
static void mq_push(uint32_t st, uint32_t d1, uint32_t d2)
{
    if (mqW - mqR < MQN) { mqOut[mqW % MQN] = st | d1 << 8 | d2 << 16; mqW++; }
}
static int16_t mNoteA = -1, mNoteB = -1;                    /* sounding ACID / BIO notes */
static uint32_t mDrumOff;                                   /* drum notes to let go at the next step: bit = GM note - 35 */
static const uint8_t GM9[11] = {36, 38, 41, 45, 48, 37, 39, 42, 46, 49, 51};
static void mn_on(uint32_t ch, uint32_t n, uint32_t v) { mq_push(0x90u | ch, n & 127u, v); }
static void mn_off(uint32_t ch, uint32_t n) { mq_push(0x80u | ch, n & 127u, 0); }
/* float helpers (the FPU; no libm) */
static float f_log2(float x) { union { float f; uint32_t i; } u = {x}; float e = (float)((int32_t)((u.i >> 23) & 255u) - 128); u.i = (u.i & 0x007FFFFFu) | 0x3F800000u; return e + (-0.34484843f * u.f + 2.02466578f) * u.f - 0.67487759f; }
static float f_exp2(float x) { float xi = (float)(int32_t)(x < 0 ? x - 1.0f : x), f = x - xi; union { float f; uint32_t i; } u; u.i = (uint32_t)((int32_t)xi + 127) << 23; return u.f * (1.0f + f * (0.6565f + f * 0.3435f)); }
/* ================================================================ 0.41: T4 KEYS, 8 voices (float)
 * PAD: a few slightly detuned sines, slow in and out (the DEEP song's pad). PLUCK / GUITAR: Karplus-Strong (a burst
 * of noise round a delay line as long as the note's period, a little darker each time round). E.PIANO / BELL: two-
 * operator FM (DX7's way; a tine on the e-piano, a 3.5 ratio for the bell). PIANO: FM with a fast-closing index and
 * a second partial. DRONE: a plucked string's harmonics, the high ones swaying (the tanpura's buzz) */
enum { KT_PAD, KT_PLUCK, KT_GUITAR, KT_EPIANO, KT_BELL, KT_PIANO, KT_DRONE };
#define KV_N 8u
#define KS_LEN 1024u
typedef struct {
    float p0, p1, p2, inc, env, atk, dk, rel, mi, mdk, t2, g, bri, sw[4];
    uint32_t wait, age;                              /* samples before it starts (a strum); samples sounding */
    uint16_t len, pos, fill; float ksLp, ksFb;   /* fill: how much of the string has its noise yet */
    uint8_t on, gate, note, type, from, midi;        /* from: 0 the steps, 1 a key, 2 a song's pad, 3 a song's tanpura */
} kv_t;
static kv_t KV[KV_N];
static int16_t ksBuf[KV_N][KS_LEN];
static float SINF[1025];
static uint32_t kvNz = 0x1234567u, kvArp;
static volatile uint16_t kvQ[16]; static volatile uint32_t kvQw, kvQr;   /* the keys of T4 (main loop -> sound) */
static void sinf_init(void)                          /* sin(2 pi i / 1024) by rotation (no libm) */
{
    const float a = 6.2831853f / 1024.0f, c = 1.0f - a * a / 2.0f + a * a * a * a / 24.0f, sn = a - a * a * a / 6.0f;
    float x = 1.0f, y = 0.0f, t; uint32_t i;
    for (i = 0; i <= 1024u; i++) { SINF[i] = y; t = x * c - y * sn; y = x * sn + y * c; x = t; }
}
static inline float fsin(float x)                    /* x in turns */
{
    float i; int32_t k;
    x -= (float)(int32_t)x; if (x < 0.0f) x += 1.0f;
    i = x * 1024.0f; k = (int32_t)i;
    return SINF[k] + (SINF[k + 1] - SINF[k]) * (i - (float)k);
}
static inline float kv_noise(void) { kvNz ^= kvNz << 13; kvNz ^= kvNz >> 17; kvNz ^= kvNz << 5; return (float)(int32_t)kvNz * (1.0f / 2147483648.0f); }
static float tau_k(float sec) { return f_exp2(-1.442695f / (sec * 44100.0f)); }   /* the per-sample factor of e^(-t / tau) */
static void kv_release(kv_t *v)
{
    if (!v->on || !v->gate) return;
    v->gate = 0;
    if (v->midi) { mn_off((P.mCh + 2u) & 15u, v->note); v->midi = 0; }
    if (v->type == KT_PLUCK || v->type == KT_GUITAR) v->ksFb *= 0.995f;       /* a hand on the string */
}
static void kv_start(uint32_t note, uint32_t type, float g, float bri, uint32_t wait, uint32_t from)
{
    uint32_t k, best = 0, n3 = 0, old = 0, oldAge = 0; float lo = 1e9f, f;
    kv_t *v;
    if (from == 3u) {                                /* the tanpura: four strings at most (the oldest gives way) */
        for (k = 0; k < KV_N; k++) if (KV[k].on && KV[k].from == 3u) { n3++; if (KV[k].age >= oldAge) { oldAge = KV[k].age; old = k; } }
        if (n3 >= 4u) KV[old].on = 0;
    }
    for (k = 0; k < KV_N; k++) {                     /* a free voice, else the quietest one let go of, else the quietest */
        float e = KV[k].on ? KV[k].env + (KV[k].gate ? 2.0f : 0.0f) : -1.0f;
        if (e < lo) { lo = e; best = k; }
    }
    v = &KV[best];
    if (v->midi) mn_off((P.mCh + 2u) & 15u, v->note);
    memset(v, 0, sizeof *v);
    f = 440.0f * f_exp2(((float)(int32_t)note - 69.0f) / 12.0f);
    v->inc = f / 44100.0f; v->note = (uint8_t)note; v->type = (uint8_t)type; v->g = g; v->bri = bri; v->wait = wait; v->from = (uint8_t)from;
    v->gate = 1; v->on = 1; v->env = 0.0f;
    {
        float dec = 0.4f + (float)P.hDec * (from >= 2u ? 0.0f : 0.045f);      /* DECAY: ~0.4 .. 6 s */
        switch (type) {
        case KT_PAD: v->atk = 1.0f / (0.35f * 44100.0f); v->dk = 1.0f; v->rel = tau_k(0.15f + dec * 0.12f); break;
        case KT_DRONE: v->atk = 1.0f / (0.01f * 44100.0f); v->dk = tau_k(from == 3u ? 1.8f : 1.0f + dec); v->rel = tau_k(from == 3u ? 0.05f : 1.5f);
            v->sw[0] = v->sw[1] = v->sw[2] = v->sw[3] = 1.0f; break;
        case KT_EPIANO: v->atk = 1.0f / 90.0f; v->dk = tau_k(0.6f + dec * 0.6f); v->rel = tau_k(0.15f); v->mi = 1.0f + 2.2f * bri; v->mdk = tau_k(0.3f); v->t2 = 1.0f; break;
        case KT_BELL: v->atk = 1.0f / 40.0f; v->dk = tau_k(1.0f + dec); v->rel = tau_k(0.8f); v->mi = 1.5f + 4.0f * bri; v->mdk = tau_k(0.9f); break;
        case KT_PIANO: v->atk = 1.0f / 60.0f; v->dk = tau_k((0.5f + dec * 0.5f) * (f < 200.0f ? 1.3f : f > 800.0f ? 0.6f : 1.0f)); v->rel = tau_k(0.12f);
            v->mi = 0.8f + 2.0f * bri; v->mdk = tau_k(0.12f); v->t2 = 1.0f; break;
        default: {                                   /* PLUCK / GUITAR: fill the string with noise, darker with less TONE */
            uint32_t L = (uint32_t)(44100.0f / f + 0.5f), i; float lp = 0.0f, c = 0.15f + 0.8f * bri;
            if (L >= KS_LEN) L = KS_LEN - 1u;
            if (L < 4u) L = 4u;
            v->len = (uint16_t)L; v->pos = 0; v->atk = 1.0f; v->env = 1.0f; v->dk = 1.0f; v->rel = 1.0f;
            v->ksFb = 1.0f - 1.0f / ((0.25f + dec * (type == KT_GUITAR ? 0.6f : 0.25f)) * f);   /* the loss per trip: a decay in seconds */
            if (v->ksFb < 0.9f) v->ksFb = 0.9f;
            v->ksLp = 0.0f; v->fill = 0; (void)i; (void)lp; (void)c;   /* the noise goes in as the string first goes round (kv_run) */
        }
        }
    }
    if (from <= 1u && P.mNote && !(P.mutes & 8u)) { mn_on((P.mCh + 2u) & 15u, note & 127u, 90); v->midi = 1; }
}
static void kv_release_from(uint32_t from) { uint32_t k; for (k = 0; k < KV_N; k++) if (KV[k].from == from) kv_release(&KV[k]); }
static void kv_all_off(void) { uint32_t k; for (k = 0; k < KV_N; k++) kv_release(&KV[k]); }
static float kv_gain(void) { return (float)P.hLvl * (1.0f / 127.0f); }
static void perc_hit(uint32_t h)                     /* T4 at a step (h from perc_on): its chord, or the next note of it */
{
    static const int8_t DRONE[4] = {-12, 0, 7, 12};
    const uint8_t *iv = ARP_SETS[P.arpSet % 6u];
    int32_t root, n[4], i;
    trig_t tg;
    if (!h || (P.mutes & 8u)) return;
    root = P.root + (P.hType == KT_DRONE ? 12 : 24);   /* A1 -> A3 (the drone an octave under) */
    if (laneN[3] && trig(3, E.s, 0, 0, &tg)) root = tg.note & 127;
    n[0] = root; for (i = 1; i < 4; i++) n[i] = root + iv[i - 1];
    if (P.hType == KT_DRONE) for (i = 0; i < 4; i++) n[i] = root + DRONE[i];
    if (h == 1u) {                                   /* ARP: one note, up and round */
        kv_release_from(0);
        kv_start((uint32_t)n[kvArp++ & 3u], P.hType, kv_gain(), (float)P.hTone / 127.0f, 0, 0);
        return;
    }
    kv_release_from(0);
    for (i = 0; i < 4; i++) kv_start((uint32_t)n[i], P.hType, kv_gain() * 0.7f, (float)P.hTone / 127.0f, P.hType == KT_GUITAR ? (uint32_t)i * 530u : 0u, 0);   /* a guitar strums: 12 ms a string */
}
static void kv_keys(void)                            /* T4's keys (queued by the main loop) */
{
    while (kvQr != kvQw) {
        uint32_t q = kvQ[kvQr & 15u], k; kvQr++;
        if (q & 256u) kv_start(q & 127u, P.hType, kv_gain(), (float)P.hTone / 127.0f, 0, 1);
        else for (k = 0; k < KV_N; k++) if (KV[k].from == 1u && KV[k].note == (q & 127u)) kv_release(&KV[k]);
    }
}
static inline float kv_run(float duck)              /* one sample of all eight */
{
    uint32_t k; float out = 0.0f;
    for (k = 0; k < KV_N; k++) {
        kv_t *v = &KV[k]; float x;
        if (!v->on) continue;
        if (v->wait) { v->wait--; continue; }
        v->age++;
        if (v->from == 3u && v->age > 4u * 44100u) v->gate = 0;          /* the tanpura's string: 4 s, then let go fast */
        if (v->type == KT_PLUCK || v->type == KT_GUITAR) {
            uint32_t a = v->pos, b = a + 1u >= v->len ? 0u : a + 1u;
            while (v->fill < v->len && v->fill <= a + 1u) {             /* the first time round: the pluck's noise, a little darker with less TONE */
                uint32_t i = v->fill++, L = v->len;
                float x = kv_noise();
                if (v->type == KT_GUITAR && i >= L / 5u) x -= 0.6f * (float)ksBuf[k][i - L / 5u] * (1.0f / 16000.0f);   /* picked a fifth of the way along */
                v->ksLp += (0.15f + 0.8f * v->bri) * (x - v->ksLp); ksBuf[k][i] = (int16_t)(v->ksLp * 16000.0f);
            }
            float y = ((float)ksBuf[k][a] + (float)ksBuf[k][b]) * 0.5f * v->ksFb;
            ksBuf[k][a] = (int16_t)y; v->pos = (uint16_t)b;
            x = y * (1.0f / 16000.0f);
            v->env = x > 0.0f ? (x > v->env * 0.9995f ? x : v->env * 0.9995f) : (-x > v->env * 0.9995f ? -x : v->env * 0.9995f);
            if (v->env < 0.0005f) { v->on = 0; continue; }
            out += x * v->g * 1.4f;
            continue;
        }
        if (v->gate && v->atk < 1.0f) { v->env += v->atk; if (v->env >= 1.0f) { v->env = 1.0f; v->atk = 1.0f; } }   /* the attack */
        else if (v->gate) { v->env *= v->dk; v->atk = 1.0f; }
        else v->env *= v->rel;
        if (v->env < 0.001f && (!v->gate || v->atk >= 1.0f)) { if (!v->gate || v->type != KT_PAD) { v->on = 0; continue; } }
        v->p0 += v->inc; if (v->p0 >= 1.0f) v->p0 -= 1.0f;
        switch (v->type) {
        case KT_PAD:
            v->p1 += v->inc * 1.003f; if (v->p1 >= 1.0f) v->p1 -= 1.0f;
            v->p2 += v->inc * 4.004f; if (v->p2 >= 1.0f) v->p2 -= 1.0f;
            x = fsin(v->p0) + 0.5f * fsin(v->p1 + 0.16f) + 0.18f * fsin(2.0f * v->p0) + v->bri * (0.32f * fsin(3.0f * v->p0) + 0.22f * fsin(v->p2));
            x *= 0.5f;
            break;
        case KT_DRONE: {                             /* 7 harmonics, 1/h, the high ones swaying: sin(h x) by recurrence from sin x, cos x */
            float s1 = fsin(v->p0), c2 = 2.0f * fsin(v->p0 + 0.25f), a = 0.0f, b = s1, t;
            if ((v->age & 63u) == 1u) {                  /* the sway, every 64 samples */
                uint32_t n;
                v->p1 += 64.0f * 0.9f / 44100.0f; if (v->p1 >= 1.0f) v->p1 -= 1.0f;
                for (n = 0; n < 4u; n++) v->sw[n] = (0.6f + 0.4f * fsin(v->p1 + (float)(n + 4u) * 0.16f)) / (float)(n + 4u);
            }
            x = b;                                       /* h = 1 */
            t = c2 * b - a; a = b; b = t; x += b * 0.5f;          /* 2 */
            t = c2 * b - a; a = b; b = t; x += b * (1.0f / 3.0f); /* 3 */
            t = c2 * b - a; a = b; b = t; x += b * v->sw[0];      /* 4..7 */
            t = c2 * b - a; a = b; b = t; x += b * v->sw[1];
            t = c2 * b - a; a = b; b = t; x += b * v->sw[2];
            t = c2 * b - a; a = b; b = t; x += b * v->sw[3];
            x *= 0.45f;
            break;
        }
        case KT_EPIANO:
            v->mi *= v->mdk; v->t2 *= 0.9984f;
            x = fsin(v->p0 + 0.16f * v->mi * fsin(v->p0)) + 0.12f * v->t2 * fsin(14.0f * v->p0);
            x *= 0.6f;
            break;
        case KT_BELL:
            v->mi *= v->mdk;
            x = fsin(v->p0 + 0.16f * v->mi * fsin(3.5f * v->p0)) * 0.5f;
            break;
        default:                                     /* PIANO */
            v->mi *= v->mdk; v->t2 *= 0.9985f;
            x = fsin(v->p0 + 0.16f * v->mi * fsin(v->p0)) + 0.3f * fsin(2.003f * v->p0) * v->t2 + 0.08f * v->t2 * v->t2 * kv_noise();
            x *= 0.5f;
        }
        out += x * v->env * v->g * (v->from == 2u ? SG.padLvl * (1.0f - 0.75f * SG.padPump * duck) : v->from == 3u ? SG.tpLvl : 1.0f);
    }
    return out;
}
static void midi_all_off(void)
{
    if (mNoteA >= 0) mn_off(P.mCh & 15u, (uint32_t)mNoteA);
    if (mNoteB >= 0) mn_off((P.mCh + 1u) & 15u, (uint32_t)mNoteB);
    mNoteA = mNoteB = -1;
    { uint32_t b; for (b = 0; b < 32u; b++) if (mDrumOff >> b & 1u) mn_off(9, 35u + b); mDrumOff = 0; }
    kv_all_off();
}
static volatile uint32_t extTarget;                         /* external clock: ticks (24 a beat = 6 a step) received since START */
/* one sample of the step clock. With CLOCK = IN it never runs past the ticks received (it waits at the next one),
 * and runs at double speed when two ticks behind */
static inline int clk_adv(uint32_t *ph, uint32_t *st, uint32_t inc)
{
    uint32_t np;
    if (P.mClk == 2u) {
        uint32_t cur = *st * 6u + (((*ph >> 16) * 6u) >> 16), nab;
        if (cur + 2u < extTarget) inc <<= 1;
        np = *ph + inc;
        nab = (*st + (np < *ph)) * 6u + (((np >> 16) * 6u) >> 16);
        if (nab > extTarget) return 0;                       /* wait for the next tick */
    } else np = *ph + inc;
    if (np < *ph) { *ph = np; (*st)++; return 1; }
    *ph = np; return 0;
}
static void midi_step(uint32_t s)                           /* the notes that start (or end) at step s */
{
    uint32_t chA = P.mCh & 15u, b;
    if (!P.mNote) return;
    for (b = 0; b < 32u; b++) if (mDrumOff >> b & 1u) mn_off(9, 35u + b);
    mDrumOff = 0;
    if (!(P.mutes & 1u)) {                                 /* ACID: a note at each attack, held over ties and slides */
        acid_t a0, a1; int32_t nt;
        eval_acid(s, 0, &a0, 0); eval_acid(s, 60000u, &a1, 0);
        nt = (a1.y + 128) >> 8; nt = nt < 0 ? 0 : nt > 127 ? 127 : nt;
        if (!a0.gate || a0.rest) { if (mNoteA >= 0) { mn_off(chA, (uint32_t)mNoteA); mNoteA = -1; } }
        else if (a0.age == 0u || nt != mNoteA) {
            int16_t old = mNoteA;
            mn_on(chA, (uint32_t)nt, a0.acc ? 120u : 90u); mNoteA = (int16_t)nt;
            if (old >= 0 && old != nt) mn_off(chA, (uint32_t)old);           /* on first, then off: a slide on a mono synth */
            else if (old == nt && a0.age == 0u) { mn_off(chA, (uint32_t)nt); mn_on(chA, (uint32_t)nt, a0.acc ? 120u : 90u); }
        }
    }
    if (!(P.mutes & 4u)) {                                 /* T3 */
        if (P.kit) {
            uint32_t v, j = s & 15u;
            uint16_t on[11], ac[11];
            drum_masks(s, on, ac);
            for (v = 0; v < 11u; v++) if ((on[v] >> j & 1u) && !(P.drMute >> v & 1u)) {
                mn_on(9, GM9[v], (ac[v] >> j & 1u) ? 120u : 90u); mDrumOff |= 1u << (GM9[v] - 35u);
            }
        } else { voice_t k; eval_kick(s, 0, &k); if (k.on && k.kt == 0u) { mn_on(9, 36, 100); mDrumOff |= 1u << 1; } }
    }
}
static void sg_keys(uint32_t s)                      /* a song's T4: DEEP's pad chord at each bar, RAGA's tanpura on the quarters */
{
    const sg_song_t *g = sgs();
    int32_t sb = SG.curSb;
    uint32_t i;
    if (g->pad && (s & 15u) == 0u) {
        uint32_t c = sb >= 0 && sb < (int32_t)g->nb ? SG_PADBAR[g->padOff + (uint32_t)sb] : 255u;
        kv_release_from(2);
        if (c != 255u) for (i = 0; i < 5u; i++) if (SG_PADC[c * 5u + i]) kv_start(SG_PADC[c * 5u + i] + 12u, KT_PAD, 0.32f, SG.padBri, 0, 2);
    }
    if (g->tpFirst && (s & 3u) == 0u && sb + 1 >= g->tpFirst && sb + 1 <= g->tpLast)
        kv_start(g->tpNotes[(s & 15u) >> 2], KT_DRONE, 0.55f, 0.5f, 0, 3);
}
static void on_step(void)
{
    if (SG.on && E.playing) { sg_map(E.s >> 4); sg_keys(E.s); }
    midi_step(E.s);
    if (E.playing && (E.s & 15u) == 0u && E.s > 0u && P.autoSong && !STEPS) {
        E.chBar++;
        if (E.chBar == P.bars - 1u) riseStart = (int32_t)E.s;
        if (E.chBar >= P.bars) { E.chBar = 0; riseStart = -1; apply_chapter(P.chapter + 1u); }
    }
    if (E.playing && !SG.on) perc_hit(perc_on(E.s));
    if (X.req && X.req != X.mode) punch_start();
    if (callReq >= 0) { callT0 = E.s << 16; callKey = callReq; callReq = -1; }   /* a call: on the 16th */
}
/* 0.38: the four ACID knobs out as CC on ACID's channel (NOTES = ON): CUTOFF 74, RESO 71, ENV 77, DECAY 75.
 * The value heard (the knob, or its motion lane), checked every 64 control ticks (~23 ms), sent when it changes,
 * and all four again every ~2 s for a page that connects late */
static uint8_t ccLast[4] = {255u, 255u, 255u, 255u};
static uint32_t ccDiv, ccRef;
static void cc_out(uint32_t s, uint32_t p16)
{
    static const uint8_t CCN[4] = {74u, 71u, 77u, 75u};
    uint32_t m;
    if (!P.mNote || ++ccDiv < 64u) return;
    ccDiv = 0;
    if (++ccRef >= 90u) { ccRef = 0; for (m = 0; m < 4u; m++) ccLast[m] = 255u; }
    for (m = 0; m < 4u; m++) {
        int32_t v = mv(m, s, p16);
        v = v < 0 ? 0 : v > 127 ? 127 : v;
        if ((uint32_t)v != ccLast[m]) { ccLast[m] = (uint8_t)v; mq_push(0xB0u | (P.mCh & 15u), CCN[m], (uint32_t)v); }
    }
}
static int32_t hz_q8(int32_t hz) { return 69 * 256 + (int32_t)(3072.0f * f_log2((float)(hz < 20 ? 20 : hz) / 440.0f)); }
static int32_t sg_lane(uint32_t l, uint32_t x64, int lg)           /* lane l of the song at x64 (1/64 bars); lg: interpolate in octaves */
{
    const sg_lane_t *L = &sgs()->lane[l];
    uint32_t lo = 0, hi = L->n - 1u, k0, k1; int32_t v0, v1, p0, p1;
    if (!L->n) return 0;
    if (L->n == 1u || x64 <= (SG_KF[L->off] >> 16)) { v0 = (int16_t)(SG_KF[L->off] & 0xFFFFu); return lg ? hz_q8(v0) : v0; }
    if (x64 >= (SG_KF[L->off + hi] >> 16)) { v0 = (int16_t)(SG_KF[L->off + hi] & 0xFFFFu); return lg ? hz_q8(v0) : v0; }
    while (hi - lo > 1u) { uint32_t m = (lo + hi) >> 1; if ((SG_KF[L->off + m] >> 16) <= x64) lo = m; else hi = m; }
    k0 = SG_KF[L->off + lo]; k1 = SG_KF[L->off + hi];
    p0 = (int32_t)(k0 >> 16); p1 = (int32_t)(k1 >> 16); v0 = (int16_t)(k0 & 0xFFFFu); v1 = (int16_t)(k1 & 0xFFFFu);
    if (lg) { int32_t q0 = hz_q8(v0), q1 = hz_q8(v1); return q0 + (q1 - q0) * ((int32_t)x64 - p0) / (p1 - p0 ? p1 - p0 : 1); }   /* Q8 MIDI */
    return v0 + (v1 - v0) * ((int32_t)x64 - p0) / (p1 - p0 ? p1 - p0 : 1);
}
static int32_t dec_units(int32_t ms)                 /* a decay in ms -> the DECAY knob (inverse of tau_ms) */
{
    int32_t d;
    for (d = 0; d < 127; d++) if ((int32_t)tau_ms((uint32_t)d + 1u) > ms) return d;
    return 127;
}
static void sg_lanes(uint32_t s, uint32_t p16)       /* the song's knobs, ~700 times a second */
{
    const sg_song_t *g = sgs();
    int32_t sb = (s >> 4) == SG.curEb ? SG.curSb : (int32_t)(s >> 4) + SG.off, v;
    uint32_t x = (uint32_t)(sb < 0 ? 0 : sb) * 64u + (s & 15u) * 4u + (p16 >> 14), i;
    v = ((sg_lane(SL_CUT, x, 1) - 24 * 256) * 128 / 72) >> 8; SG.k[0] = v < 0 ? 0 : v > 180 ? 180 : v;   /* past 127: the songs open to 8 kHz */
    v = sg_lane(SL_RES, x, 0) * 120 / 1000; SG.k[1] = v < 0 ? 0 : v > 120 ? 120 : v;
    v = sg_lane(SL_ENV, x, 0) * 115 / 1000; SG.k[2] = v < 0 ? 0 : v > 127 ? 127 : v;
    SG.k[3] = dec_units(sg_lane(SL_DEC, x, 0));
    v = sg_lane(SL_ACC, x, 0) * 127 / 1000; P.aAcc = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
    v = (sg_lane(SL_DRV, x, 0) * 16 / 100 - 12) * 4; P.aDrive = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
    v = sg_lane(SL_WAVE, x, 0); P.aWave = (uint8_t)(v >= 750 ? 1 : v >= 150 ? 3 : 0);
    v = sg_lane(SL_DLY, x, 0) * 256 / 1000; P.dMix = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
    v = sg_lane(SL_FB, x, 0) * 128 / 1000; P.dFb = (uint8_t)(v < 0 ? 0 : v > 110 ? 110 : v);
    v = sg_lane(SL_VERB, x, 0) * 260 / 1000; P.rMix = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
    SG.gain = (int32_t)(32767.0f * f_exp2((float)sg_lane(SL_LVL, x, 0) * 0.016609640f));   /* dB/10 -> gain */
    if (SG.gain > 36000) SG.gain = 36000;
    SG.padLvl = (float)sg_lane(SL_PLVL, x, 0) * 0.001f; SG.padBri = (float)sg_lane(SL_PBRI, x, 0) * 0.001f;
    SG.padPump = (float)sg_lane(SL_PPUMP, x, 0) * 0.001f; SG.tpLvl = (float)sg_lane(SL_TPL, x, 0) * 0.001f;
    v = sg_lane(SL_DHP, x, 0); SG.dhpA = v > 25 ? 1.0f - 6.2831853f * (float)v / 44100.0f : 1.0f;
    SG.bq8 = sg_lane(SL_BCUT, x, 1); SG.bres = sg_lane(SL_BRES, x, 0); SG.benv = sg_lane(SL_BENV, x, 0); SG.bdec = sg_lane(SL_BDEC, x, 0);
    SG.rise = 0.0f;
    for (i = 0; i < g->nris; i++) {
        int32_t a = (int32_t)(g->ris[i][0] - 1u) * 64, l = (int32_t)g->ris[i][1] * 64;
        if ((int32_t)x >= a && (int32_t)x < a + l) SG.rise = (float)((int32_t)x - a) / (float)l;
    }
}
static void control(void)
{
    uint32_t s = E.s, p16 = E.stepPh >> 16, base;
    acid_t oA; voice_t oB, oK;
    int32_t fcq, callCq = 0, callRes = 0, callOn;
    cc_out(s, p16);
    kv_keys();
    if (SG.on && (E.ctr & 63u) == 0u) sg_lanes(s, p16);
    eval_acid(s, p16, &oA, 0); eval_bio(s, p16, &oB); eval_kick(s, p16, &oK);
    callOn = call_eval(s, p16, &oA, &callCq, &callRes);   /* an animal call takes ACID over (and sounds when ACID is muted) */
    callWave = callOn ? CALLS[callKey < 5 ? callKey : callKey < 10 ? callKey - 5 : 5].wave : 255u;
    if (SG.on && sgs()->two) {   /* the second 303: its own line, cutoff, resonance, envelope; panned right, more into the delay */
        acid_t o2; int32_t fc, e, kk, a;
        eval_win(&WB, s, p16, &o2, P.sTrans, 50);
        V2.inc = inc_q8(o2.y);
        V2.ampT = (!o2.gate || (P.mutes & 1u)) ? 0 : ((((o2.acc ? 30000 : 24000) * 92) >> 7) * E.duck) >> 15;
        a = (int32_t)(o2.age > (24u << 16) ? (24u << 16) : o2.age);
        kk = (int32_t)(960000u / ((uint32_t)(o2.acc ? 200 : (SG.bdec > 50 ? SG.bdec : 50)) * P.bpm));
        e = expneg((int32_t)(((uint32_t)a * (uint32_t)(kk ? kk : 1)) >> 14 > 4095u ? 4095u : ((uint32_t)a * (uint32_t)(kk ? kk : 1)) >> 14));
        fc = SG.bq8 + ((((SG.benv * 43 * 256) / 1000) * e) >> 15) + (o2.acc ? 6 * 256 : 0);
        fc = (int32_t)(inc_q8(fc) / 20861u);
        V2.g = 32767 - expneg((fc < 40000 ? fc : 40000) >> 7);
        V2.kq = SG.bres * 15 / 4;                               /* res 0..1000 -> Q12 0..3.7 */
        V2.drv = 33; V2.panL = 22; V2.send = (int32_t)sgs()->bdly; V2.wave = 0; V2.on = 1;
    } else {                     /* 0.41: T2 ZOO */
        zoo_t z;
        int32_t fc;
        V2.on = (uint8_t)(zoo_eval(s, p16, &z) || V2.amp > 8);
        if (V2.on) {
            if (z.o.gate) V2.inc = inc_q8(z.o.y);
            V2.ampT = (!z.o.gate || (P.mutes & 2u)) ? 0 : ((((z.o.acc ? 30000 : 24000) * P.bLvl) >> 7) * E.duck) >> 15;
            fc = (int32_t)(inc_q8(z.cq ? z.cq : 60 * 256) / 20861u);
            V2.g = 32767 - expneg((fc < 40000 ? fc : 40000) >> 7);
            V2.kq = z.res * 126; V2.drv = 33; V2.panL = 32; V2.send = 100; if (z.wave != 255u) V2.wave = z.wave;
        }
    }
    if (P.mNote && E.playing) {
        if (mNoteA >= 0 && !oA.gate) { mn_off(P.mCh & 15u, (uint32_t)mNoteA); mNoteA = -1; }
        if (oB.on && mNoteB < 0 && !(P.mutes & 2u)) { int32_t nb = (oB.y + 128) >> 8; mNoteB = (int16_t)(nb < 0 ? 0 : nb > 127 ? 127 : nb); mn_on((P.mCh + 1u) & 15u, (uint32_t)mNoteB, 90); }
        else if (!oB.on && mNoteB >= 0) { mn_off((P.mCh + 1u) & 15u, (uint32_t)mNoteB); mNoteB = -1; }
    }
    E.kInc = inc_q8(oK.y);
    E.kAmpT = (P.mutes & 4u) ? 0 : (oK.amp * P.kLvl) >> 7;
    if (oK.on && oK.kt < 1200u) E.kClk = P.kClick * 200;
    E.kDrv = 12 + (P.kDrive >> 2);
    if (P.kit && duckEnv > 0) duckEnv -= (duckEnv >> 8) + 1;                     /* ~90 ms */
    E.duck = 32767 - ((((P.mutes & 4u) ? 0 : P.kit ? duckEnv : oK.amp) * P.kDuck) >> 7);
    E.aInc = inc_q8(oA.y);
    E.aAmpT = (!oA.gate || ((P.mutes & 1u) && !callOn)) ? 0 : (((oA.acc ? 24000 + P.aAcc * 60 : 24000) * P.aLvl) >> 7);
    E.aAmpT = (E.aAmpT * E.duck) >> 15;
    fcq = (int32_t)(inc_q8(callOn ? callCq : cutoff_at(&oA, s, p16)) / 20861u);
    E.f = fcq < 23000 ? fcq : 23000;
    E.g = 32767 - expneg((fcq < 40000 ? fcq : 40000) >> 7);   /* one pole of the ladder: g = 1 - e^-w */
    {
        int32_t res = callOn ? callRes : mv(1, s, p16);
        if ((fxFn & FN_SQUELCH) && res < 112 && !callOn) res = 112;
        E.damp = 32768 - ((res * 31500) >> 7);               /* Q14: 2.0 .. 0.08 */
        E.kq = res * 126;                                    /* the ladder's feedback, Q12: 0 .. 3.7 (no self-oscillation, as on a 303) */
    }
    E.aDrv = 12 + (P.aDrive >> 2);
    E.bInc = inc_q8(oB.y);
    E.bAmpT = (P.mutes & 2u) || P.bAnimal < 7u ? 0 : ((((oB.amp * P.bLvl) >> 7) * E.duck) >> 15);
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

static volatile uint32_t cpu_us, cpu_max, cpu_avg8, cpu_pk;   /* this block; the worst lately; the average (x256); the peak shown */
#define D8_GAIN 450                                         /* the 808 against the 909 (tuned by ear and RMS) */
static drum909_t D9;
static drum808_t D8;
static float drBuf[256], d8Rev[256], d8Dly[256];   /* the 808 always writes its two sends: nowhere to go here */
static volatile uint8_t d8ready;
static volatile uint16_t drDirty = 0x7FFu, drAud;            /* rows whose knobs changed; rows to play now (a black key) */
static uint8_t d9ready;
static void dk_hit(uint32_t v, float vel)                  /* a hit on the kit; vel 1 = an accent */
{
    if (P.kit == 2u) { if (d8ready) drum808_trigger(&D8, T8[v], vel >= 0.99f ? 1.0f : D8_VEL_NORMAL); }
    else drum909_trigger(&D9, (int)v, vel);
}
static void dk_render(float *b, uint32_t off, uint32_t n)
{
    if (P.kit == 2u) { if (d8ready) drum808_render(&D8, b + off, d8Rev + off, d8Dly + off, (int)n); }
    else drum909_render(&D9, b + off, 0, 0, (int)n);
}
static void drum_apply(void)                                /* between blocks: the knobs, the auditions */
{
    uint32_t m, v, i;
    if (!d9ready) return;                                   /* (its tables are made at boot, in the main loop) */
    m = drDirty;
    {   /* at most three rows a block: a whole kit at once (a song, a kit change) was one slow block */
        uint32_t take = 0, n = 0;
        for (v = 0; v < 11u && n < 3u; v++) if (m >> v & 1u) { take |= 1u << v; n++; }
        drDirty = (uint16_t)(drDirty & ~take); m = take;
    }
    for (v = 0; v < 11u; v++) if (m >> v & 1u) for (i = 0; i < 4u; i++) {
        if (P.kit == 2u) { if (KN8[v][i] >= 0 && d8ready) drum808_set(&D8, T8[v], KN8[v][i], P.drPot[v][i]); }
        else if (KN9[v][i] >= 0) drum909_set(&D9, (int)v, KN9[v][i], P.drPot[v][i]);
    }
    m = drAud; drAud = 0;
    for (v = 0; v < 11u; v++) if (m >> v & 1u) { dk_hit(v, 1.0f); if (v == 0u) duckEnv = 32767; }
}
static void drum_masks(uint32_t s, uint16_t *on, uint16_t *ac)   /* the 11 rows' steps for the bar of step s */
{
    uint32_t v;
    if (SG.on) {
        int32_t sb = (s >> 4) == SG.curEb ? SG.curSb : (int32_t)(s >> 4) + SG.off;
        const sg_song_t *g = sgs();
        uint32_t idx = sb >= 0 && sb < (int32_t)g->nb ? SG_BARS[((uint32_t)g->first + (uint32_t)sb) * 3u + 2u] : 0xFFFFu;
        for (v = 0; v < 11u; v++) { on[v] = idx == 0xFFFFu ? 0 : SG_DRUM[idx * 22u + v * 2u]; ac[v] = idx == 0xFFFFu ? 0 : SG_DRUM[idx * 22u + v * 2u + 1u]; }
        return;
    }
    for (v = 0; v < 11u; v++) { on[v] = P.drRow[v]; ac[v] = P.drAcc[v]; }
}
static void drum_step(uint32_t s)                           /* the hits of step s */
{
    uint32_t v, j = s & 15u;
    uint16_t on[11], ac[11];
    if (SG.on) sg_map(s >> 4);
    if (P.mutes & 4u) return;
    drum_masks(s, on, ac);
    for (v = 0; v < 11u; v++)
        if ((on[v] >> j & 1u) && !(P.drMute >> v & 1u)) {
            dk_hit(v, (ac[v] >> j & 1u) ? 1.0f : 88.0f / 127.0f);   /* as X0X: a plain hit ~3 dB under */
            if (v == 0u) duckEnv = 32767;
        }
}
/* 0.34: the FPU check. The same float sums in the audio ISR and in the main loop (which the ISR interrupts): 0 = not
 * run yet, 1 = right every time, 2+ = a wrong result was seen (the ISR would be corrupting floats). X0X uses this FPU. */
static volatile uint32_t fpuIsr, fpuMain, fpu_us;
static int fpu_sum(uint32_t n)
{
    volatile float k = 0.5f;
    float s = 0.0f, ph = 0.0f, q;
    uint32_t i;
    for (i = 0; i < n; i++) { s += (float)i * k; ph += 0.25f; if (ph >= 1.0f) ph -= 1.0f; }
    q = (1.5f * 1.25f) / 0.5f;                                   /* 3.75 */
    return s == 0.5f * (float)(n * (n - 1u) / 2u) && ph == 0.0f && q == 3.75f;   /* all exact in single precision */
}
static void app_audio(int32_t *out, uint32_t n)
{
    uint32_t i, t0 = fm1_ticks();
    int started = 0;
    if (E.startReq) {
        E.startReq = 0; E.s = 0; E.stepPh = 0; E.chBar = 0; E.playing = 1; run = 1; E.tick = 0;
        if (P.autoSong) { apply_chapter(0); riseStart = -1; }
        if (SG.on) { SG.curEb = 0xFFFFFFFFu; SG.curSb = -1; SG.sec = 255; sg_map(0); sg_lanes(0, 0); }
        if (P.mClk == 1u) { mq_push(0xFAu, 0, 0); mq_push(0xF8u, 0, 0); }
        control(); started = 1; midi_step(0);
    }
    if (E.stopReq) { E.stopReq = 0; E.playing = 0; run = 0; riseStart = -1; if (P.mClk == 1u) mq_push(0xFCu, 0, 0); midi_all_off(); kv_release_from(0); kv_release_from(2); }
    if (P.kit) {   /* the 909 / 808: find this block's step edges first (the same clock as below), render up to each, hit, go on */
        uint32_t pos = 0, k;
        drum_apply();
        for (k = 0; k < n; k++) drBuf[k] = d8Rev[k] = d8Dly[k] = 0.0f;
        if (E.playing) {
            uint32_t ph = E.stepPh, st = E.s, base = P.bpm * 6494u;
            if (started) drum_step(0);
            for (k = 0; k < n; k++)
                if (clk_adv(&ph, &st, base * 100u / (100u + ((st & 1u) ? 0u - P.swing : P.swing)))) {
                    if (k > pos) { dk_render(drBuf, pos, k - pos); pos = k; }
                    drum_step(st);
                }
        }
        if (n > pos) dk_render(drBuf, pos, n - pos);
        if (SG.on && SG.dhpA < 0.9999f) for (k = 0; k < n; k++) { float x = drBuf[k]; dhpY = SG.dhpA * (dhpY + x - dhpX); dhpX = x; drBuf[k] = dhpY; }   /* the drums thinned out (a build) */
    }
    for (i = 0; i < n; i++) {
        int32_t mix = 0, wet = 0, dl, dr, md, l, r, rv = 0, b2 = 0;
        uint32_t ns;
        if ((E.ctr++ & 15u) == 0u) control();
        {   /* the step clock (and, CLOCK = OUT, 24 MIDI ticks a beat) */
            uint32_t ph = E.stepPh, st = E.s, tk;
            int stepped = clk_adv(&ph, &st, E.sinc);
            E.stepPh = ph;
            tk = ((ph >> 16) * 6u) >> 16;
            if (P.mClk == 1u && (stepped || tk != E.tick)) mq_push(0xF8u, 0, 0);
            E.tick = (uint8_t)tk;
            if (stepped) { E.s = st; on_step(); control(); }
        }
        (void)ns;

        if (P.kit) {   /* the 909 / 808 (dry: not into the delay), at KICK LVL */
            int32_t dv = (int32_t)(drBuf[i] * (float)(P.kLvl * (P.kit == 2u ? D8_GAIN : 150)));
            mix += dv > 40000 ? 40000 : dv < -40000 ? -40000 : dv;
        }
        /* T3 KICK: a sine (TONE pushes it towards a filtered saw) and a click */
        E.kPh += E.kInc;
        E.kAmp += (E.kAmpT - E.kAmp) >> 5;
        if (E.kAmp > 8) {
            int32_t sine = SINT[E.kPh >> 22], saw = (int32_t)(E.kPh >> 16) - 32768, v;
            E.kLp += ((saw - E.kLp) * 9000) >> 15;
            v = (sine * (127 - P.kTone) + E.kLp * P.kTone) >> 7;
            if (E.kClk > 20) { v += (noise() * E.kClk) >> 15; E.kClk -= (E.kClk >> 7) + 1; }
            mix += (sat((v * E.kDrv) >> 4) * E.kAmp) >> 15;
        }
        /* T1 ACID: a wave and a sub -> a 4-pole ladder (the 303's kind of filter: 24 dB an octave, the resonance fed back
         * through a soft clip, the bass thinning as it rises) or the Chamberlin SVF (12 dB, brighter) */
        E.aPh += E.aInc; E.aSub += E.aInc >> 1;
        E.aAmp += (E.aAmpT - E.aAmp) >> 6;
        {
            int32_t saw = (int32_t)(E.aPh >> 16) - 32768, sq = E.aPh < 0x80000000u ? 26000 : -26000, src, hp;
            uint32_t wv = callWave < 4u ? callWave : P.aWave;
            src = wv == 0 ? saw : wv == 1 ? sq : wv == 2 ? (E.aPh < 0x40000000u ? 26000 : -26000) : (saw + sq) >> 1;
            if (P.aSub) src = (src * (127 - (P.aSub >> 1)) + (E.aSub < 0x80000000u ? 26000 : -26000) * (P.aSub >> 1)) >> 7;
            if (P.aFilt == 0u) {
                int32_t u = (src >> 1) - ((E.kq * E.y4) >> 12), u3;
                if (u > 32767) u = 32767; else if (u < -32767) u = -32767;
                u3 = (((u * u) >> 15) * u) >> 15;
                u -= u3 / 3;                                     /* soft clip: |u| <= 21845 */
                E.y1 += (E.g * (u - E.y1)) >> 15;
                E.y2 += (E.g * (E.y1 - E.y2)) >> 15;
                E.y3 += (E.g * (E.y2 - E.y3)) >> 15;
                E.y4 += (E.g * (E.y3 - E.y4)) >> 15;
                E.lp = E.y4 + ((E.y4 * E.kq) >> 13);             /* x (1 + k / 2), as the film does */
            } else {
                E.lp += (E.f * E.bp) >> 15;
                hp = (src >> 1) - E.lp - ((E.damp * E.bp) >> 14);
                if (hp > 65535) hp = 65535; else if (hp < -65535) hp = -65535;
                E.bp += (E.f * hp) >> 15;
                if (E.bp > 60000) E.bp = 60000; else if (E.bp < -60000) E.bp = -60000;
                if (E.lp > 60000) E.lp = 60000; else if (E.lp < -60000) E.lp = -60000;
            }
            if (E.aAmp > 8) { int32_t a = (sat((E.lp * E.aDrv) >> 4) * E.aAmp) >> 15; mix += a; wet += a; }
        }
        if (V2.on) {   /* the second 303 (a song's, or T2 ZOO): a saw (or square) into the same kind of ladder */
            int32_t saw = (int32_t)(V2.ph >> 16) - 32768, sq = V2.ph < 0x80000000u ? 26000 : -26000, u, u3, b;
            int32_t src = V2.wave == 1u ? sq : V2.wave == 3u ? (saw + sq) >> 1 : saw;
            V2.ph += V2.inc; V2.amp += (V2.ampT - V2.amp) >> 6;
            if (V2.amp > 8) {
                u = (src >> 1) - ((V2.kq * V2.y4) >> 12);
                if (u > 32767) u = 32767; else if (u < -32767) u = -32767;
                u3 = (((u * u) >> 15) * u) >> 15; u -= u3 / 3;
                V2.y1 += (V2.g * (u - V2.y1)) >> 15; V2.y2 += (V2.g * (V2.y1 - V2.y2)) >> 15;
                V2.y3 += (V2.g * (V2.y2 - V2.y3)) >> 15; V2.y4 += (V2.g * (V2.y3 - V2.y4)) >> 15;
                b = (sat(((V2.y4 + ((V2.y4 * V2.kq) >> 13)) * V2.drv) >> 4) * V2.amp) >> 15;
                b2 = b; wet += (b * V2.send) / 100;
            }
        }
        if (SG.on && SG.rise > 0.0f) {   /* the riser: noise through a band-pass sweeping up */
            float fc = 600.0f * f_exp2(SG.rise * 3.9069f), f = 6.2831853f * fc / 44100.0f, x;
            RZ.n = RZ.n * 1664525u + 1013904223u; x = (float)(int32_t)RZ.n * (1.0f / 2147483648.0f);
            RZ.lp += f * RZ.bp; RZ.bp += f * (x - RZ.lp - 0.45f * RZ.bp);
            mix += (int32_t)(RZ.bp * 0.16f * SG.rise * SG.rise * 30000.0f);
        }
        /* T2 BIO: the animal's wavetable, a little noise, a one-pole low-pass */
        E.bPh += E.bInc;
        E.bAmp += (E.bAmpT - E.bAmp) >> 7;
        if (E.bAmp > 8) {
            int32_t wt = BIO_WT[((P.bAnimal - 7u) & 3u) * 256u + (E.bPh >> 24)], v = (wt * (127 - P.bNoise) + noise() * P.bNoise) >> 7;
            E.bLp += ((v - E.bLp) * E.bK) >> 15;
            int32_t bo = (((E.bLp * E.bAmp) >> 15) * 9) >> 4;
            mix += bo; wet += bo;
        }
        /* T4 KEYS: into the mix and, a little, into the delay and the room; it pumps with the kick */
        {
            float kx = kv_run((float)duckEnv * (1.0f / 32767.0f)) * ((P.mutes & 8u) && !SG.on ? 0.0f : 1.0f);
            int32_t kq = (int32_t)(kx * 14000.0f * (float)E.duck * (1.0f / 32767.0f));
            kq = kq > 30000 ? 30000 : kq < -30000 ? -30000 : kq;
            mix += kq; wet += kq >> 1;
        }
        /* the delay, as in the acid film: ACID and BIO only (the drums stay dry), dotted eighth by default; the
         * echoes go left, right, left..., each one through a low-pass, so each is darker and further away */
        dl = dbufL[(E.didx - E.dlen) & DMASK]; dr = dbufR[(E.didx - E.dlen) & DMASK];
        E.dlp += rz((dr - E.dlp) * E.dk, 15);
        dbufL[E.didx] = (int16_t)clip16(rz(wet * P.dMix, 8) + rz(E.dlp * P.dFb, 7));   /* not sat(): it has gain 1.5 */
        dbufR[E.didx] = (int16_t)rz(dl * 29, 5);
        E.didx = (E.didx + 1u) & DMASK;
        md = 1 + (P.mDrive >> 5);
        if (P.rMix) rv = rz(reverb(rz(wet, 3) + rz(dl + dr, 3)) * P.rMix, 7);   /* the acid, the animal and their echoes */
        l = sat((mix + ((b2 * V2.panL) >> 5) + ((dl * 13) >> 4) + rv) * md); r = sat((mix + b2 + ((dr * 13) >> 4) + rv) * md);
        if (SG.on) {
            l = (l * SG.gain) >> 15; r = (r * SG.gain) >> 15;
            if (SG.gapNext && E.playing && (E.s & 15u) == 15u) { l >>= 3; r >>= 3; }   /* the half-beat of silence before a drop */
        }
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
    {   uint32_t t1 = fm1_ticks();
        if (fpu_sum(256)) { if (!fpuIsr) fpuIsr = 1; } else fpuIsr++;
        fpu_us = (fm1_ticks() - t1) / FM1_TICKS_PER_US;
    }
    cpu_us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
    if (cpu_us > cpu_max) cpu_max = cpu_us;
    cpu_avg8 = (uint32_t)((int32_t)cpu_avg8 + (((int32_t)(cpu_us * 256u) - (int32_t)cpu_avg8) >> 6));   /* ~0.4 s (signed: it falls too) */
}

/* ================================================================ the UI (main loop) */
#define INK RGB(31, 35, 40)
#define LIME RGB(255, 253, 247)                     /* 0.44: the light on ink (it was a lime green) */
#define PAPER RGB(246, 243, 236)
#define REDC RGB(217, 72, 59)
static const uint16_t TCOL[4] = {RGB(209, 73, 91), RGB(42, 157, 143), RGB(58, 110, 165), RGB(38, 50, 74)};
static const char *TRK[4] = {"ACID", "ZOO", "KICK", "KEYS"};   /* T3 reads "909" on the 909 kit */
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
enum { F_NUM, F_LIST, F_ST, F_PCT, F_NOTE, F_OCT, F_PLUS, F_STEP, F_CH };
typedef struct { const char *lab; uint16_t off; int16_t min, max; uint8_t sgn, fmt, part; const char *const *list; } pm_t;
#define O(f) ((uint16_t)__builtin_offsetof(params_t, f))
static const char *const L_WAVE[] = {"SAW", "SQUARE", "PULSE", "SAW+SQ"};
static const char *const L_ANIMAL[] = {"ACID B", "MEOW", "MEW", "MOO", "QUACK", "WOOF", "MEOW LONG", "FLY", "BEE", "FROG", "WHALE"};
static const char *const L_ARP[] = {"OFF", "UP", "UP/DN", "RANDOM"};
static const char *const L_HTYPE[] = {"PAD", "PLUCK", "GUITAR", "E.PIANO", "BELL", "PIANO", "DRONE"};
static const char *const L_HPAT[] = {"HOLD", "HALF", "BEATS", "OFFBEAT", "3-3-2", "ARP"};
static const char *const L_LRATE[] = {"1/16", "1/8", "1/4", "1/2", "1BAR", "2BAR", "4BAR", "8BAR"};
static const char *const L_SHAPE[] = {"SINE", "TRI", "SQUARE", "S&H"};
static const char *const L_DEST[] = {"CUTOFF", "PITCH"};
static const char *const L_DTIME[] = {"1/16", "1/8", "3/16", "1/4"};
static const char *const L_MODE7[] = {"LOCRIAN", "PHRYG", "AEOLIAN", "DORIAN", "MIXOLYD", "IONIAN", "LYDIAN"};
static const char *const L_CHORD[] = {"M7", "MAJ7", "SUS", "OCT", "5TH", "DIM"};
static const char *const L_ONOFF[] = {"OFF", "ON"};
static const char *const L_BPER[] = {"2 STEP", "4 STEP", "8 STEP", "16 STEP", "32 STEP", "64 STEP"};
static const char *const L_SRC[] = {"STEPS", "FORMULA"};
static const char *const L_FILT[] = {"303 4P", "SVF 2P"};
static const char *const L_KIT[] = {"FUNC", "909", "808"};
static const char *const L_MCLK[] = {"OFF", "OUT", "IN"};
static const char *const L_DSONG[] = {"ANY", "CHICAGO", "MINIMAL", "TWO 303S", "SCREAM", "BREAKS", "BIG ROOM", "DEEP", "RAGA"};
enum { PM_CUT, PM_RES, PM_ENV, PM_ADEC, PM_AWAVE, PM_ASUB, PM_ATUNE, PM_ALVL, PM_ADRIVE, PM_AACC, PM_AGATE, PM_AGLIDE,
       PM_BANIMAL, PM_BQUANT, PM_BARP, PM_BLVL, PM_BDRIFT, PM_BNOISE, PM_BBRIGHT, PM_BTUNE, PM_BPER, PM_BPHASE,
       PM_KTOP, PM_KTONE, PM_KCLICK, PM_KDRIVE, PM_KPDEC, PM_KBODY, PM_KBASE, PM_KDUCK,
       PM_HTYPE, PM_HTONE, PM_HPAT, PM_HLVL, PM_HDEC,
       PM_N, PM_OFF, PM_DENS, PM_SWING, PM_LRATE, PM_LDEP, PM_LSHAPE, PM_LDEST, PM_DTIME, PM_DFB, PM_DMIX, PM_MDRIVE,
       PM_SCALEC, PM_WALK, PM_ROOT, PM_ARPSET, PM_BLIPAMT, PM_BLIPN, PM_BPM, PM_AUTO, PM_BARS, PM_KLVL,
       PM_DTONE, PM_SLEN, PM_ASRC, PM_STRANS, PM_RMIX, PM_AFILT, PM_KIT, PM_MCLK, PM_MNOTE, PM_MCH, PM_BOOT, PM_DSONG, PM_ZCUT, PM_ZRES, PM_ZENV, PM_ZDEC, PM_NONE };
static const pm_t PM[] = {
    {"CUTOFF", O(cut), 0, 127, 0, F_NUM, 0, 0}, {"RESO", O(res), 0, 120, 0, F_NUM, 0, 0}, {"ENV AMT", O(env), 0, 127, 0, F_NUM, 0, 0},
    {"DECAY", O(aDec), 0, 127, 0, F_NUM, 0, 0}, {"WAVE", O(aWave), 0, 3, 0, F_LIST, 0, L_WAVE}, {"SUB", O(aSub), 0, 127, 0, F_NUM, 0, 0},
    {"OCTAVE", O(aTune), -2, 1, 1, F_OCT, 0, 0}, {"ACID LVL", O(aLvl), 0, 127, 0, F_NUM, 0, 0}, {"DRIVE", O(aDrive), 0, 127, 0, F_NUM, 0, 0},
    {"ACCENT", O(aAcc), 0, 127, 0, F_NUM, 0, 0}, {"GATE", O(aGate), 10, 100, 0, F_PCT, 0, 0}, {"GLIDE", O(aGlide), 0, 127, 0, F_NUM, 0, 0},
    {"VOICE", O(bAnimal), 0, 10, 0, F_LIST, 1, L_ANIMAL}, {"QUANTIZE", O(bQuant), 0, 127, 0, F_NUM, 1, 0}, {"ARP", O(bArp), 0, 3, 0, F_LIST, 1, L_ARP},
    {"BIO LVL", O(bLvl), 0, 127, 0, F_NUM, 1, 0}, {"CONTOUR", O(bDrift), 0, 127, 0, F_NUM, 1, 0}, {"NOISE", O(bNoise), 0, 127, 0, F_NUM, 1, 0},
    {"BRIGHT", O(bBright), 0, 127, 0, F_NUM, 1, 0}, {"TUNE", O(bTune), -24, 24, 1, F_ST, 1, 0}, {"CALL", O(bPer), 0, 5, 0, F_LIST, 1, L_BPER},
    {"PHASE", O(bPhase), 0, 15, 0, F_PLUS, 1, 0},
    {"PITCH", O(kTop), 0, 48, 0, F_ST, 2, 0}, {"TONE", O(kTone), 0, 127, 0, F_NUM, 2, 0}, {"CLICK", O(kClick), 0, 127, 0, F_NUM, 2, 0},
    {"DRIVE", O(kDrive), 0, 127, 0, F_NUM, 2, 0}, {"P.DECAY", O(kPdec), 0, 127, 0, F_NUM, 2, 0}, {"BODY", O(kBody), 0, 127, 0, F_NUM, 2, 0},
    {"TUNE", O(kBase), -12, 24, 1, F_ST, 2, 0}, {"DUCK", O(kDuck), 0, 127, 0, F_NUM, 2, 0},
    {"SOUND", O(hType), 0, 6, 0, F_LIST, 3, L_HTYPE}, {"TONE", O(hTone), 0, 127, 0, F_NUM, 3, 0}, {"RHYTHM", O(hPat), 0, 5, 0, F_LIST, 3, L_HPAT},
    {"KEYS LVL", O(hLvl), 0, 127, 0, F_NUM, 3, 0}, {"DECAY", O(hDec), 0, 127, 0, F_NUM, 3, 0},
    {"N", O(N), 3, 16, 0, F_NUM, 9, 0}, {"OFFSET", O(off), 0, 15, 0, F_NUM, 9, 0}, {"DENSITY", O(dens), 0, 127, 0, F_NUM, 9, 0},
    {"SWING", O(swing), 0, 40, 0, F_PCT, 9, 0}, {"RATE", O(lRate), 0, 7, 0, F_LIST, 9, L_LRATE}, {"DEPTH", O(lDep), 0, 127, 0, F_NUM, 9, 0},
    {"SHAPE", O(lShape), 0, 3, 0, F_LIST, 9, L_SHAPE}, {"DEST", O(lDest), 0, 1, 0, F_LIST, 9, L_DEST}, {"TIME", O(dTime), 0, 3, 0, F_LIST, 9, L_DTIME},
    {"FEEDBACK", O(dFb), 0, 110, 0, F_NUM, 9, 0}, {"MIX", O(dMix), 0, 127, 0, F_NUM, 9, 0}, {"MASTER DRV", O(mDrive), 0, 127, 0, F_NUM, 9, 0},
    {"MODE", O(scaleC), 0, 6, 0, F_LIST, 9, L_MODE7}, {"WALK", O(walk), 1, 6, 0, F_NUM, 9, 0}, {"ROOT", O(root), 21, 57, 1, F_NOTE, 9, 0},
    {"CHORD", O(arpSet), 0, 5, 0, F_LIST, 9, L_CHORD}, {"BLIP", O(blipAmt), 0, 12, 0, F_ST, 9, 0}, {"BLIP EVERY", O(blipN), 2, 8, 0, F_NUM, 9, 0},
    {"TEMPO", O(bpm), 80, 170, 0, F_NUM, 9, 0}, {"AUTO SONG", O(autoSong), 0, 1, 0, F_LIST, 9, L_ONOFF}, {"BARS/CH", O(bars), 2, 16, 0, F_NUM, 9, 0},
    {"KICK LVL", O(kLvl), 0, 127, 0, F_NUM, 2, 0},
    {"TONE", O(dTone), 0, 127, 0, F_NUM, 9, 0}, {"LENGTH", O(sLen), 1, 32, 0, F_NUM, 9, 0}, {"PLAYS", O(aSrc), 0, 1, 0, F_LIST, 9, L_SRC},
    {"TRANSPOSE", O(sTrans), -12, 12, 1, F_ST, 9, 0}, {"REVERB", O(rMix), 0, 127, 0, F_NUM, 9, 0},
    {"FILTER", O(aFilt), 0, 1, 0, F_LIST, 0, L_FILT},
    {"KIT", O(kit), 0, 2, 0, F_LIST, 2, L_KIT},
    {"CLOCK", O(mClk), 0, 2, 0, F_LIST, 9, L_MCLK}, {"NOTES OUT", O(mNote), 0, 1, 0, F_LIST, 9, L_ONOFF}, {"ACID CH", O(mCh), 0, 15, 0, F_CH, 9, 0},
    {"BOOT DEMO", O(bootDemo), 0, 1, 0, F_LIST, 9, L_ONOFF},
    {"DEMO SONG", O(demoSong), 0, 8, 0, F_LIST, 9, L_DSONG},
    {"CUTOFF", O(zCut), 0, 127, 0, F_NUM, 1, 0}, {"RESO", O(zRes), 0, 120, 0, F_NUM, 1, 0}, {"ENV AMT", O(zEnv), 0, 127, 0, F_NUM, 1, 0},
    {"DECAY", O(zDec), 0, 127, 0, F_NUM, 1, 0},
};
static int32_t pget(uint32_t k)
{
    const uint8_t *b = (const uint8_t *)&P + PM[k].off;
    if (SG.on && k <= PM_ADEC && !(SG.own >> k & 1u)) return SG.k[k] > PM[k].max ? PM[k].max : SG.k[k];   /* the song's hand on the knob */
    return PM[k].sgn ? (int32_t)(int8_t)*b : (int32_t)*b;
}
static void pset(uint32_t k, int32_t v)
{
    int32_t mx = PM[k].max;
    if (k == PM_OFF) mx = P.N - 1;
    if (k == PM_SCALEC && P.scaleM == 5) mx = 4;
    if (v < PM[k].min) v = PM[k].min;
    if (v > mx) v = mx;
    if (k == PM_SLEN && v > (int32_t)step_len()) {   /* longer: new steps start as a copy of the line, if they are empty */
        uint32_t L0 = step_len(), i, empty = 1, a = accB(), sl = sldB();
        for (i = L0; i < (uint32_t)v; i++) if (P.lane[0][i] != NONE) empty = 0;
        if (empty) for (i = L0; i < (uint32_t)v; i++) {
            P.lane[0][i] = P.lane[0][i % L0];
            a = (a & ~(1u << i)) | ((a >> (i % L0) & 1u) << i); sl = (sl & ~(1u << i)) | ((sl >> (i % L0) & 1u) << i);
        }
        setAccB(a); setSldB(sl);
    }
    *((uint8_t *)&P + PM[k].off) = (uint8_t)v;
    if (k == PM_N && P.off >= P.N) P.off = P.N - 1u;
    if (k == PM_SLEN) count_lanes();
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
    case F_CH: sint(s, v + 1); break;
    default: sint(s, v);
    }
}

/* ---- pages: 4 cells = KNOB1..4 */
enum { G_HOME, G_EDIT, G_ENV, G_LFO, G_FX, G_SEL, G_ARP, G_SEQ, G_GLO, G_DRUM, G_N };
typedef struct { const char *name; uint8_t k[4]; } page_t;
static const page_t PG_HOME = {"HOME", {PM_CUT, PM_RES, PM_ENV, PM_ADEC}};
static const page_t PG_EDIT[4][3] = {
    {{"OSC", {PM_AWAVE, PM_ASUB, PM_ATUNE, PM_ALVL}}, {"FILTER", {PM_CUT, PM_RES, PM_ENV, PM_ADRIVE}}, {"TYPE", {PM_AFILT, PM_NONE, PM_NONE, PM_NONE}}},
    {{"VOICE", {PM_BANIMAL, PM_ZCUT, PM_ZRES, PM_ZENV}}, {"PLAY", {PM_ZDEC, PM_BARP, PM_BTUNE, PM_BLVL}}, {"BIO", {PM_BDRIFT, PM_BNOISE, PM_BBRIGHT, PM_BQUANT}}},
    {{"KIT", {PM_KIT, PM_KLVL, PM_KDUCK, PM_NONE}}, {"TONE", {PM_KTOP, PM_KTONE, PM_KCLICK, PM_KDRIVE}}, {"", {0}}},
    {{"SOUND", {PM_HTYPE, PM_HTONE, PM_HPAT, PM_HLVL}}, {"CHORD", {PM_ARPSET, PM_HDEC, PM_NONE, PM_NONE}}, {"", {0}}}};
static const uint8_t NEDIT[4] = {3, 3, 2, 2};
static const page_t PG_ENV[4] = {{"ENV", {PM_ADEC, PM_AACC, PM_AGATE, PM_AGLIDE}}, {"CALL", {PM_BPER, PM_BPHASE, PM_ZDEC, PM_ZENV}},
                                 {"ENV", {PM_KPDEC, PM_KBODY, PM_KBASE, PM_KDUCK}}, {"ENV", {PM_HDEC, PM_HTONE, PM_ARPSET, PM_HLVL}}};
static const page_t PG_OTHER[] = {
    {"LFO>ACID", {PM_LRATE, PM_LDEP, PM_LSHAPE, PM_LDEST}}, {"DELAY", {PM_DTIME, PM_DFB, PM_DMIX, PM_DTONE}},
    {"SCALE", {PM_NONE, PM_SCALEC, PM_WALK, PM_ROOT}}, {"ARP BLIP", {PM_ARPSET, PM_NONE, PM_BLIPAMT, PM_BLIPN}},
    {"LINE", {PM_N, PM_OFF, PM_DENS, PM_SWING}}, {"GLOBAL", {PM_BPM, PM_NONE, PM_AUTO, PM_BARS}}};
static const page_t PG_STEPS = {"STEPS", {PM_SLEN, PM_ASRC, PM_SWING, PM_STRANS}};
static const page_t PG_SPACE = {"SPACE", {PM_RMIX, PM_MDRIVE, PM_NONE, PM_NONE}};
static const page_t PG_MIX = {"MIX", {PM_ALVL, PM_BLVL, PM_KLVL, PM_HLVL}};
static const page_t PG_DRUM = {"DRUMS", {PM_NONE, PM_NONE, PM_NONE, PM_NONE}};
static const page_t PG_MIDI = {"MIDI", {PM_MCLK, PM_MNOTE, PM_MCH, PM_NONE}};
static const page_t PG_START = {"START", {PM_BOOT, PM_DSONG, PM_NONE, PM_NONE}};

static struct {
    uint8_t group, page, part, rec, rootOct;
    int8_t held; uint32_t heldT; uint8_t used;
    int8_t touch; uint32_t touchT;
    char toast[32]; uint32_t toastT;
    uint8_t keyNote[27], keyTrk[27];
    int8_t recKey; uint8_t recTrk, recNote; uint32_t recLast;
    uint32_t seed; uint8_t dirty; uint32_t dirtyT;
    int8_t stepHeld; uint8_t stepKey, stepNew, stepEdited, lastNote, recPos; uint32_t stepT;
    int8_t slotK; uint8_t slotArm; uint32_t slotT;
} U = {.held = -1, .touch = -1, .recKey = -1, .stepHeld = -1, .slotK = -1};

static uint32_t seq_step_pages(void) { return P.sLen > 16u ? 2u : 1u; }   /* SEQ: steps 1-16 (and 17-32), then LINE */
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
    case G_SEQ: return U.page < seq_step_pages() ? &PG_STEPS : &PG_OTHER[4];
    case G_DRUM: return &PG_DRUM;
    default: return U.page % 4u == 0u ? &PG_OTHER[5] : U.page % 4u == 1u ? &PG_MIX : U.page % 4u == 2u ? &PG_MIDI : &PG_START;
    }
}
static uint32_t npages(void) { return U.group == G_EDIT ? NEDIT[U.part] : U.group == G_SEQ ? seq_step_pages() + 1u : U.group == G_GLO ? 4u : U.group == G_FX ? 2u : 1u; }
static void toast(const char *a, const char *b)
{
    U.toast[0] = 0; scat(U.toast, a); if (b) scat(U.toast, b);
    U.toastT = fm1_ms; U.touch = -1;
}
static void changed(void) { U.dirty = 1; U.dirtyT = fm1_ms; }
static void save_all(void) { ST.cur = SG.on ? PB : P; settings_save(&ST, sizeof ST); U.dirty = 0; }   /* in a song: your own set */   /* an erase: the sound stalls a moment */
static int steps_page(void) { return U.group == G_SEQ && U.page < seq_step_pages(); }
#define SG_PLAYABLE 8u                               /* 0.41: all eight (DEEP's pads and RAGA's tanpura on T4's synth) */
static const char *const SG_LAB[8] = {"CHIC", "MINI", "2X303", "SCRM", "BRKS", "BIG", "DEEP", "RAGA"};
static void sg_enter(uint32_t n, int demo)          /* play song n from its start; your set is kept in PB */
{
    const sg_song_t *g = &SONGS[n];
    uint32_t v;
    if (!SG.on) PB = P; else P = PB;
    P.bpm = g->bpm; P.swing = (uint8_t)(g->swing > 40u ? 40u : g->swing); P.autoSong = 0; P.sTrans = 0;
    P.aLvl = 100; P.aSub = 0; P.aTune = 0; P.aFilt = 0; P.aGate = 50; P.aGlide = 0; P.dTime = 2; P.dTone = 64;
    P.terms = (uint16_t)(P.terms & ~(T_BIO | T_PERC)); P.mutes = 0; P.kDuck = 40; P.kLvl = 110;
    drum_defaults(&P); P.kit = g->kit ? g->kit : 1u; drum_pots(&P); drDirty = 0x7FFu;   /* RAGA's soft kit: the 808 */
    if (g->soft) { P.drPot[1][2] = 70; P.drPot[1][3] = 30; P.drPot[7][2] = 24; P.drPot[8][2] = 14; P.drPot[9][2] = 30; }   /* soft: little snare rattle, hats and cymbal at half */
    memset(P.mot, 255, sizeof P.mot); motAny = 0;
    for (v = 0; v < 4u; v++) motTouch[v] = 0;
    SG.song = (uint8_t)n; SG.demo = (uint8_t)demo; SG.loop = 0; SG.ended = 0; SG.own = 0; SG.jump = -1; SG.off = 0;
    for (; SG.off + 1 < (int32_t)g->nb; SG.off++) {                  /* from the first bar that sounds (MINIMAL fades in from nothing) */
        const uint16_t *r = &SG_BARS[((uint32_t)g->first + (uint32_t)SG.off) * 3u];
        uint32_t j, any = 0;
        for (j = 0; j < 11u; j++) any |= SG_DRUM[r[2] * 22u + j * 2u];
        for (j = 0; j < 16u; j++) any |= !((uint8_t)SG_ACID[r[0] * 32u + j * 2u + 1u] & 4u);
        if (g->pad && SG_PADBAR[g->padOff + (uint32_t)SG.off] != 255u) any = 1;       /* DEEP opens on its pads, RAGA on its tanpura */
        if (g->tpFirst && SG.off + 1 >= g->tpFirst) any = 1;
        if (any) break;
    }
    SG.on = 1; E.startReq = 1;
    { char t[32] = ""; scat(t, g->name); if (demo) scat(t, "  PLAY = YOUR SET"); toast(t, 0); }
}
static void sg_exit(void)                            /* back to your own set, stopped */
{
    if (!SG.on) return;
    if (E.playing) E.stopReq = 1;
    SG.on = 0; SG.demo = 0;
    P = PB; P.magic = MAGIC; drDirty = 0x7FFu; count_lanes(); mot_count(); riseStart = -1;
}
static void sg_skip(int d)                           /* the next section / this section's start (or the one before) */
{
    const sg_song_t *g = sgs();
    int32_t sb = SG.curSb < 0 ? 0 : SG.curSb, sc = sg_sec_of(sb), t;
    if (d > 0) { if (sc + 1 >= g->nsec) { toast("LAST SECTION", 0); return; } t = sc + 1; }
    else t = sb - (g->secBar[sc] - 1) < 2 && sc > 0 ? sc - 1 : sc;
    SG.jump = g->secBar[t] - 1;
    toast(g->secName[t], 0);
}
static uint32_t step_base(void) { return steps_page() ? U.page * 16u : 0u; }

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
    if (t == 0u && STEPS) { setAccB(accB() & ~(1u << i)); setSldB(sldB() & ~(1u << i)); }
    count_lanes(); changed();
}
static void clear_lane(int all)
{
    uint32_t t, i;
    for (t = 0; t < 4u; t++) if (all || t == U.part) for (i = 0; i < LANE; i++) P.lane[t][i] = NONE;
    if (all || U.part == 2u) { memset(P.drRow, 0, sizeof P.drRow); memset(P.drAcc, 0, sizeof P.drAcc); }
    /* 0.43: ZOO and KEYS play on their own when they have no notes: cleared, they go quiet too (a preset or a key wakes them) */
    if (all || U.part == 1u) P.terms &= (uint16_t)~T_BIO;
    if (all || U.part == 3u) { P.terms &= (uint16_t)~T_PERC; kv_release_from(0); }
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
    if (t == 3u && kvQw - kvQr < 16u) { kvQ[kvQw & 15u] = (uint16_t)(256u | ((uint32_t)note & 127u)); kvQw++; }
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
    if (t == 3u && kvQw - kvQr < 16u) { kvQ[kvQw & 15u] = U.keyNote[k]; kvQw++; }
    if (U.recKey == (int8_t)k) U.recKey = -1;
}
static void tick_motion(void)                    /* REC on, playing: a knob being turned writes into its lane */
{
    uint32_t m;
    if (!U.rec || !E.playing) return;
    for (m = 0; m < 4u; m++)
        if (fm1_ms - motTouch[m] < 400u) {
            uint8_t v = ((const uint8_t *)&P)[MOT_OFF[m]];
            if (P.mot[m][E.s & 63u] != v) { P.mot[m][E.s & 63u] = v; motAny |= (uint8_t)(1u << m); changed(); }
        }
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
    setAccB(0); setSldB(0);
    for (i = 0; i < LANE; i++) P.lane[0][i] = NONE;
    for (i = 0; i < L; i++) {
        uint32_t r = rnd(100);
        if (i == 0u) { P.lane[0][i] = (uint8_t)P.root; setAccB(accB() | 1u); continue; }
        if (r < 20u) continue;                                            /* a rest */
        if (r < 27u && P.lane[0][i - 1u] != NONE) P.lane[0][i] = (uint8_t)(step_note(i - 1u, L) | 128);   /* a tie */
        else P.lane[0][i] = (uint8_t)(P.root + GEN_W[rnd(12)]);
        if (rnd(100) < 28u) setAccB(accB() | 1u << i);
        if (rnd(100) < 22u) setSldB(sldB() | 1u << i);
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
        case 1: setAccB(accB() ^ b); break;
        case 2: setSldB(sldB() ^ b); break;
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
        i += (int)step_base();
        if ((uint32_t)i >= step_len()) { char s[16] = "LENGTH IS "; sint(s, (int32_t)step_len()); toast(s, 0); return; }
        U.stepNew = P.lane[0][i] == NONE;
        if (U.stepNew) { step_set_note((uint32_t)i, U.lastNote ? U.lastNote : P.root); count_lanes(); changed(); }
        U.stepHeld = (int8_t)i; U.stepKey = (uint8_t)k; U.stepT = fm1_ms; U.stepEdited = 0;
        step_preview(1);
    } else if (U.stepHeld >= 0 && U.stepKey == k) {
        uint32_t j = (uint32_t)U.stepHeld;
        if (!U.stepNew && !U.stepEdited && fm1_ms - U.stepT < 400u) {          /* a tap on a step that was on: off */
            P.lane[0][j] = NONE; setAccB(accB() & ~(1u << j)); setSldB(sldB() & ~(1u << j)); count_lanes(); changed();
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
    case 2: setAccB(d > 0 ? accB() | b : accB() & ~b); break;
    default: setSldB(d > 0 ? sldB() | b : sldB() & ~b); break;
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
    case 2: scat(val, accB() >> j & 1u ? "ON" : "OFF"); break;
    default: scat(val, sldB() >> j & 1u ? "ON" : "OFF");
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
static void call_key(uint32_t i, int down)
{
    int low;
    const call_t *c;
    if (!down || i > 10u) return;
    c = call_of((int32_t)i, &low);
    toast(low ? "LOW " : "", c->name);
    if (E.playing) callReq = (int8_t)i;
    else { callT0 = (E.s << 16) + (E.stepPh >> 16); callKey = (int8_t)i; }
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

static void drum_key(uint32_t k)
{
    int w, i = key_index(k, &w);
    if (!w) { if (i < 11) { P.drSel = (uint8_t)i; drAud |= (uint16_t)(1u << i); toast(drn(i), 0); } return; }
    if (SG.on) { toast("THE SONG'S STEPS: PLAY = YOURS", 0); return; }
    {
        uint32_t v = P.drSel, b = 1u << i, on = P.drRow[v] & b, ac = P.drAcc[v] & b;
        if (!on) P.drRow[v] |= (uint16_t)b;                                          /* off -> on */
        else if (!ac) P.drAcc[v] |= (uint16_t)b;                                     /* on -> accent */
        else { P.drRow[v] &= (uint16_t)~b; P.drAcc[v] &= (uint16_t)~b; }             /* accent -> off */
        if (!E.playing) drAud |= (uint16_t)(1u << v);
        changed();
    }
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
        else if (i == 4) { memset(P.mot, 255, sizeof P.mot); motAny = 0; toast("KNOB MOTION CLEARED", 0); changed(); }
        break;
    case BTN_FX:
        if (w) fx_key((uint32_t)i, down); else call_key((uint32_t)i, down);
        break;
    case BTN_SEL:
        if (down) { P.root = (int8_t)(53 + (int32_t)k - 24); { char s[8] = ""; snote(s, P.root); toast("ROOT ", s); } changed(); }
        break;
    case BTN_HOME:
        if (down && w && i < 8) { sg_enter((uint32_t)i, 0); }
        if (down && w && i >= 8 && i < 16 && !SG.on) { load_pattern((uint32_t)(i - 8)); count_lanes(); toast("LINE: ", PATS[i - 8].name); changed(); }
        if (down && !w && i == 0 && !SG.on) { load_meow_line(); count_lanes(); toast("LINE: ", "MEOW"); changed(); }   /* acid-cat: black key 1 */
        break;
    case BTN_EDIT: case BTN_ENV:
        if (!down || !w) return;
        if (i < 4) { U.part = (uint8_t)i; U.page = 0; toast(TRK[i], 0); }
        else if (i >= 8 && (uint32_t)(i - 8) < NPRE[U.part]) { apply_preset(U.part, (uint32_t)(i - 8)); toast(pre_name(U.part, P.pre[U.part]), 0); changed(); }
        break;
    case BTN_ARP:
        if (down && w && i < 6) { P.arpSet = (uint8_t)i; P.terms |= T_ARP; toast("CHORD ", L_CHORD[i]); changed(); }
        break;
    case BTN_SAVE:                                   /* white keys 1-6: a tap loads U1-U6, a long press (0.7 s) saves into it */
        if (!w || i >= (int)NSLOT) break;
        if (SG.on) { if (down) toast("STOP THE SONG FIRST", 0); break; }
        if (down) { U.slotK = (int8_t)i; U.slotT = fm1_ms; U.slotArm = 0; break; }
        if (U.slotK != (int8_t)i) break;
        {
            char t[24] = "";
            if (fm1_ms - U.slotT >= 700u) { ST.slot[i] = P; ST.used[i] = 1; save_all(); scat(t, "SAVED U"); sint(t, i + 1); }
            else if (ST.used[i]) {
                P = ST.slot[i]; P.magic = MAGIC; count_lanes(); mot_count(); riseStart = -1;
                scat(t, "LOADED U"); sint(t, i + 1); changed();
            } else { scat(t, "U"); sint(t, i + 1); scat(t, " EMPTY: HOLD TO SAVE"); }
            toast(t, 0); U.slotK = -1;
        }
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
    case BTN_REC: if (w && i == 0) return "CLR"; if (w && i == 1) return "ALL"; if (w && i == 2) return P.laneMode[U.part] ? "EXACT" : "FOLLW"; if (w && i == 3) return "LOCK"; if (w && i == 4) return "KNOBS"; break;
    case BTN_FX: if (w && i < 16) return FX_LAB[i]; if (!w) { int low; return call_of(i, &low)->lab; } break;
    case BTN_SEL: snote(buf, 53 + (int32_t)k - 24); return buf;
    case BTN_HOME: if (w && i < 8) return SG_LAB[i]; if (w && i >= 8 && i < 16) return PATS[i - 8].lab; if (!w && i == 0) return "MEOW"; break;
    case BTN_EDIT: case BTN_ENV: if (w && i < 4) return TRK[i]; if (w && i >= 8 && (uint32_t)(i - 8) < NPRE[U.part]) { scat(buf, "P"); sint(buf, i - 7); return buf; } break;
    case BTN_ARP: if (w && i < 6) return L_CHORD[i]; break;
    case BTN_SAVE: if (w && i < (int)NSLOT) { scat(buf, "U"); sint(buf, i + 1); return buf; } break;
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
    case BTN_HOME: return w && SG.on && i == (int)SG.song;
    case BTN_FX: return w ? fx_on((uint32_t)i) : callKey == i;
    case BTN_EDIT: case BTN_ENV: return w && (i == (int)U.part || (i >= 8 && i - 8 == (int)P.pre[U.part]));
    case BTN_ARP: return w && i == (int)P.arpSet;
    case BTN_SAVE: return w && i < (int)NSLOT && ST.used[i];
    default: return 0;
    }
}
static int has_layer(int b) { return b == BTN_SAVE || b == BTN_SEQ || b == BTN_GLO || b == BTN_REC || b == BTN_FX || b == BTN_SEL || b == BTN_HOME || b == BTN_EDIT || b == BTN_ENV || b == BTN_ARP; }

static void tap(int b)
{
    static const int8_t GRP[14] = {-1, -1, G_FX, G_SEL, G_ENV, G_LFO, G_EDIT, G_GLO, G_HOME, -2, G_ARP, G_SEQ, -1, -1};
    if (SG.on && b == BTN_REC) { SG.loop ^= 1u; toast("LOOP SECTION ", SG.loop ? "ON" : "OFF"); return; }
    if (b == BTN_SEQ) {                              /* SEQ: ACID's steps (1-16, 17-32), LINE, then the drum grid; on T3: the grid first */
        int drums;
        if (U.group == G_DRUM) { U.group = G_SEQ; U.page = 0; if (!SG.on) U.part = 0; return; }
        if (U.group != G_SEQ) drums = U.part == 2u;
        else drums = SG.on || U.page + 1u >= npages();
        if (!drums) { if (U.group == G_SEQ) U.page++; else { U.group = G_SEQ; U.page = 0; } if (!SG.on) U.part = 0; return; }
        if (!P.kit && !SG.on) { P.kit = 1; drDirty = 0x7FFu; toast("T3 KIT: 909", 0); changed(); }
        U.group = G_DRUM; U.page = 0; U.part = 2; return;
    }
    if (b == BTN_REC) {
        U.rec ^= 1u; recNow = U.rec; U.recPos = 0;
        if (U.rec) {
            P.autoSong = 0;
            if (!E.playing && U.part == 0u && STEPS) toast("REC: KEYS FILL STEP 1, 2..", 0);
            else { if (!E.playing) E.startReq = 1; toast("REC ", TRK[U.part]); }
        } else { U.recKey = -1; toast("REC OFF", 0); }
        return;
    }
    if (GRP[b] == -2) { save_all(); toast("SAVED", 0); return; }   /* SAVE: keep it in flash */
    if (GRP[b] < 0) return;
    if (U.group == (uint8_t)GRP[b]) U.page = (uint8_t)((U.page + 1u) % npages());
    else { U.group = (uint8_t)GRP[b]; U.page = 0; }
    if (U.group == G_SEQ) U.part = 0;                               /* SEQ is ACID's */
}

static void turn(uint32_t enc, int32_t d)
{
    if (U.held >= 0) U.used = 1;
    if (steps_page() && U.stepHeld >= 0 && enc >= ENC_KNOB1 && enc <= ENC_KNOB4) { step_turn(enc - ENC_KNOB1, d); return; }
    if (U.group == G_DRUM && enc == ENC_SELECT) { int32_t r = (int32_t)P.drSel + d; P.drSel = (uint8_t)(r < 0 ? 0 : r > 10 ? 10 : r); drAud |= (uint16_t)(1u << P.drSel); return; }
    if (U.group == G_DRUM && enc >= ENC_KNOB1 && enc <= ENC_KNOB4) {   /* the row's knobs */
        uint32_t i = enc - ENC_KNOB1, v = P.drSel; int32_t x;
        const x0x_param_t *pp = dk_param(P.kit, v, i);
        if (!pp) return;
        x = (int32_t)P.drPot[v][i] + (pp->max > 40 ? (d > 1 || d < -1 ? d * 3 : d * 2) : d);
        P.drPot[v][i] = (uint8_t)(x < 0 ? 0 : x > pp->max ? pp->max : x);
        drDirty |= (uint16_t)(1u << v); U.touch = (int8_t)i; U.touchT = fm1_ms; changed();
        return;
    }
    if (enc == ENC_SELECT) {
        if (npages() > 1u) { int32_t p = (int32_t)U.page + d; U.page = (uint8_t)(p < 0 ? 0 : p >= (int32_t)npages() ? (int32_t)npages() - 1 : p); }
        else if (SG.on) sg_skip(d);
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
    if (enc == ENC_PRESETS && steps_page()) {                          /* on the steps: walk the patterns */
        load_pattern((uint32_t)((int32_t)patNow + d + (int32_t)NPATS) % NPATS); count_lanes();
        toast("LINE: ", PATS[patNow].name); changed(); return;
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
        if (SG.on) {
            uint32_t m;
            for (m = 0; m < 4u; m++) if (PM[k].off == MOT_OFF[m] && !(SG.own >> m & 1u)) { pset(k, pget(k)); SG.own |= (uint8_t)(1u << m); }
        }
        pset(k, pget(k) + step);
        if (k == PM_KIT) { drum_pots(&P); drDirty = 0x7FFu; }
        if (SG.on && k >= PM_MCLK && k <= PM_DSONG) ((uint8_t *)&PB)[PM[k].off] = ((const uint8_t *)&P)[PM[k].off];   /* settings, not sound: kept */
        U.touch = (int8_t)i; U.touchT = fm1_ms;
        {
            uint32_t m;
            for (m = 0; m < 4u; m++) if (PM[k].off == MOT_OFF[m]) { motTouch[m] = fm1_ms; tick_motion(); }
        }
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
/* ---------------------------------------------------------------- 0.44: the screen in the Acid films' look (一线一色):
 * paper, one ink, a colour per track. 0..70: the page and four round knobs. 70..240: the selected track's sequencer as a
 * ring (16 steps round, the pointer sweeping, higher notes further out) and, right of it, its sound: a wave over the
 * filter's spectrum. The drum grid, the steps and the key maps keep their own pages (70..180) over a strip (180..240) */
static int32_t isin(int32_t a) { return SINT[a & 1023]; }                  /* a: 1024 a turn, 0 = right, clockwise (y down) */
static int32_t icos(int32_t a) { return SINT[(a + 256) & 1023]; }
static void disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t dy, dx;
    for (dy = -r; dy <= r; dy++) {
        for (dx = 0; (dx + 1) * (dx + 1) + dy * dy <= r * r + r; dx++) { }
        cv_rect(cx - dx, cy + dy, 2 * dx + 1, 1, c);
    }
}
static void dot(int32_t cx, int32_t cy, int32_t r, uint16_t fill) { disc(cx, cy, r, INK); if (r > 1) disc(cx, cy, r - 1, fill); }
static void circ(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    int32_t i, n = r * 6 + 8;
    for (i = 0; i < n; i++) { int32_t q = i * 1024 / n; cv_pset(cx + ((r * icos(q)) >> 15), cy + ((r * isin(q)) >> 15), c); }
}
static void arcq(int32_t cx, int32_t cy, int32_t r, int32_t a0, int32_t a1, int32_t th, uint16_t c)   /* a0 < a1, th px inwards */
{
    int32_t i, t, n = (a1 - a0) * r / 120 + 3;
    for (i = 0; i <= n; i++) {
        int32_t q = a0 + (a1 - a0) * i / n;
        for (t = 0; t < th; t++) cv_pset(cx + (((r - t) * icos(q)) >> 15), cy + (((r - t) * isin(q)) >> 15), c);
    }
}
static void ray(int32_t cx, int32_t cy, int32_t r, int32_t a, uint16_t c) { cv_line(cx, cy, cx + ((r * icos(a)) >> 15), cy + ((r * isin(a)) >> 15), c); }
static float hz_of(int32_t q8) { return 440.0f * f_exp2(((float)q8 / 256.0f - 69.0f) / 12.0f); }
#define FAINT mixc(PAPER, INK, 3)
#define CREAM RGB(255, 253, 247)
static const uint16_t KNC[4] = {RGB(209, 73, 91), RGB(217, 72, 59), RGB(61, 163, 122), RGB(47, 111, 176)};

static void knob(int32_t cx, int32_t cy, int32_t r, int32_t v, int32_t lo, int32_t hi, uint16_t col)   /* a round knob: 270 degrees */
{
    int32_t f = hi > lo ? (v - lo) * 768 / (hi - lo) : 0;
    f = f < 0 ? 0 : f > 768 ? 768 : f;
    arcq(cx, cy, r, 384, 384 + 768, 3, FAINT);
    if (f) arcq(cx, cy, r, 384, 384 + f, 3, col);
    disc(cx, cy, r - 5, INK); disc(cx, cy, r - 6, CREAM);
    ray(cx, cy, r - 6, 384 + f, INK); ray(cx + 1, cy, r - 6, 384 + f, INK);
}
static void cell_text(int32_t x0, const char *lab, const char *val, int hot)   /* under a knob: its name, its value */
{
    int32_t w = (int32_t)slen(lab) * 6 - 1;
    t5(x0 + 30 - w / 2, 45, lab, hot ? REDC : INK);
    w = text_w(&FONT_S, val);
    if (w <= 58) tx(x0 + 30 - w / 2, 53, &FONT_S, val, hot ? REDC : INK, PAPER);
    else { w = (int32_t)slen(val) * 6 - 1; t5(x0 + 30 - w / 2, 58, val, hot ? REDC : INK); }
}
static void draw_top(void)                                   /* 0..70: the page, the transport, four knobs */
{
    const page_t *pg = cur_page();
    char s[32];
    uint32_t i;
    int32_t x = 4, w;
    cv_begin(240, 70, PAPER);
    /* the bar: the track's colour, the page (or the song and its section), the clock */
    cv_rect(4, 4, 9, 9, TCOL[U.part & 3u]);
    if (U.rec) { cv_rect(3, 3, 11, 1, REDC); cv_rect(3, 13, 11, 1, REDC); cv_rect(3, 3, 1, 11, REDC); cv_rect(13, 3, 1, 11, REDC); }
    x = 18; s[0] = 0;
    if (steps_page() && U.stepHeld >= 0) { scat(s, "STEP "); sint(s, U.stepHeld + 1); }
    else if (SG.on && (U.group == G_HOME || U.group == G_SEQ)) {
        const sg_song_t *g = sgs();
        if (SG.demo && (fm1_ms / 2500u) % 2u) scat(s, "PLAY = YOUR SET");
        else { scat(s, g->name); scat(s, " > "); scat(s, g->secName[SG.sec < g->nsec ? SG.sec : 0]); }
    }
    else if (U.group == G_DRUM) { scat(s, P.kit == 2u ? "808 > " : "909 > "); scat(s, drn(P.drSel)); }
    else if (U.group == G_HOME) { scat(s, TRK[U.part & 3u]); scat(s, " "); scat(s, pre_name(U.part & 3u, P.pre[U.part & 3u])); }
    else {
        if (U.group == G_EDIT || U.group == G_ENV) { scat(s, TRK[U.part & 3u]); scat(s, ">"); }
        scat(s, pg->name);
    }
    if (steps_page() && P.sLen > 16u && !SG.on) scat(s, U.page ? " 17-32" : " 1-16");
    else if (npages() > 1u && U.group != G_HOME) { scat(s, " "); sint(s, U.page % npages() + 1u); scat(s, "/"); sint(s, npages()); }
    {
        char r[16] = "";
        int32_t rw;
        if (SG.on && E.playing) { sint(r, SG.curSb + 1); scat(r, "/"); sint(r, sgs()->nb); }
        else { if (E.playing) { sint(r, (E.s >> 4) % 99u + 1u); scat(r, "."); sint(r, ((E.s & 15u) >> 2) + 1u); scat(r, " "); } sint(r, P.bpm); }
        rw = text_w(&FONT_S, r);
        tx(236 - rw, 0, &FONT_S, r, INK, PAPER);
        if (E.playing) { cv_rect(236 - rw - 9, 4, 2, 9, INK); cv_rect(236 - rw - 7, 5, 2, 7, INK); cv_rect(236 - rw - 5, 6, 2, 5, INK); cv_rect(236 - rw - 3, 7, 1, 3, INK); }
        else cv_rect(236 - rw - 9, 4, 8, 8, mixc(PAPER, INK, 7));
        if (riseStart >= 0) t5(236 - rw - 40, 5, "RISE", REDC);
        if (SG.on && SG.loop) t5(236 - rw - 40, 5, "LOOP", REDC);
        w = 236 - rw - (riseStart >= 0 || (SG.on && SG.loop) ? 44 : 14) - x;
        if (text_w(&FONT_S, s) <= w) tx(x, 0, &FONT_S, s, INK, PAPER); else t5(x, 5, s, INK);
    }
    cv_rect(0, 17, 240, 1, FAINT);
    /* the four knobs */
    for (i = 0; i < 4u; i++) {
        int32_t x0 = (int32_t)i * 60, k = pg->k[i], hot = U.touch == (int8_t)i && fm1_ms - U.touchT < 800u, cx = x0 + 30, cy = 30;
        uint16_t col = i == 0u ? TCOL[U.part & 3u] : KNC[i];
        if (k == PM_NONE && U.group == G_GLO && i == 1u) {          /* GLOBAL: how much of the 5.8 ms the sound takes */
            {   /* the average (the knob) and the worst block of the last 2 s */
                uint32_t av = cpu_avg8 * 100u / (5805u * 256u), pk = cpu_pk * 100u / 5805u;
                s[0] = 0; sint(s, (int32_t)av); scat(s, "/"); sint(s, (int32_t)pk); scat(s, "%");
                knob(cx, cy, 13, (int32_t)av, 0, 100, av > 60u ? REDC : KNC[2]);
                cell_text(x0, "CPU AVG/PK", s, fpuIsr > 1u || fpuMain > 1u || pk > 90u);
            }
            continue;
        }
        if (U.group == G_DRUM) {
            const x0x_param_t *pp = dk_param(P.kit, P.drSel, i);
            if (!pp) continue;
            knob(cx, cy, 13, P.drPot[P.drSel][i], 0, pp->max ? pp->max : 1, TCOL[2]);
            s[0] = 0; sint(s, P.drPot[P.drSel][i]);
            {
                char lab[12] = ""; uint32_t n;
                for (n = 0; pp->name[n] && n < 9u; n++) lab[n] = pp->name[n];
                lab[n] = 0; cell_text(x0, lab, s, hot);
            }
            continue;
        }
        if (steps_page() && U.stepHeld >= 0) {
            const char *lab;
            step_cell(i, &lab, s);
            cell_text(x0, lab, s, hot);
            circ(cx, cy, 13, FAINT);
            continue;
        }
        if (k == PM_NONE) continue;
        pfmt((uint32_t)k, s);
        if (k == PM_CUT) { int32_t hz = (int32_t)hz_of(24 * 256 + pget(PM_CUT) * 144); s[0] = 0; if (hz >= 1000) { sint(s, hz / 1000); scat(s, "."); sint(s, hz % 1000 / 100); scat(s, "K"); } else { sint(s, hz); scat(s, "HZ"); } }
        {   /* knob motion: the moving value, and a red mark */
            uint32_t m;
            for (m = 0; m < 4u; m++)
                if (PM[k].off == MOT_OFF[m] && (motAny >> m & 1u)) {
                    disc(cx + 13, cy - 12, 2, REDC);
                    if (E.playing) { s[0] = 0; if (k == PM_ADEC && STEPS) { sint(s, (int32_t)tau_ms((uint32_t)mv(3, E.s, E.stepPh >> 16))); scat(s, "MS"); } else sint(s, mv(m, E.s, E.stepPh >> 16)); }
                }
        }
        knob(cx, cy, 13, pget((uint32_t)k), PM[k].min, k == PM_OFF ? P.N - 1 : PM[k].max, col);
        cell_text(x0, PM[k].lab, s, hot);
    }
    cv_blit(0, 0);
}

static void draw_readout(int32_t ox, int32_t oy, int32_t ys)   /* what a knob did, or a message: a band at screen y ys */
{
    char s[40]; s[0] = 0;
    if (U.touch >= 0 && fm1_ms - U.touchT < 1000u && U.group == G_DRUM) {
        const x0x_param_t *pp = dk_param(P.kit, P.drSel, (uint32_t)U.touch);
        if (pp) { scat(s, drn(P.drSel)); scat(s, " "); scat(s, pp->name); scat(s, " "); sint(s, P.drPot[P.drSel][U.touch]); }
    } else if (U.touch >= 0 && fm1_ms - U.touchT < 1000u && steps_page() && U.stepHeld >= 0) {
        const char *lab; char v[12];
        step_cell((uint32_t)U.touch, &lab, v);
        scat(s, "STEP "); sint(s, U.stepHeld + 1); scat(s, " "); scat(s, lab); scat(s, " "); scat(s, v);
    } else if (U.touch >= 0 && fm1_ms - U.touchT < 1000u) {
        uint32_t k = cur_page()->k[U.touch];
        if (PM[k].part < 4u) { scat(s, TRK[PM[k].part]); scat(s, " "); }
        scat(s, PM[k].lab); scat(s, " "); pfmt(k, s + slen(s));
    } else if (fm1_ms - U.toastT < 1100u) scat(s, U.toast);
    if (s[0]) {
        int32_t w = text_w(&FONT_S, s) + 14, x;
        if (w > 236) w = 236;
        x = (240 - w) / 2;
        cv_rect(x - ox, ys - oy, w, 22, INK);
        tx(x + 7 - ox, ys + 3 - oy, &FONT_S, s, CREAM, INK);
    }
}

/* ---- the ring: the selected track's 16 steps of this bar (the 909 / 808: three rings, BD in, snares, hats out) */
#define RCX 66
#define RCY 76
#define RR 54
static int32_t step_ang(uint32_t i) { return (int32_t)i * 64 - 256; }
static void ring_pos(uint32_t i, int32_t r, int32_t *x, int32_t *y) { int32_t a = step_ang(i); *x = RCX + ((r * icos(a)) >> 15); *y = RCY + ((r * isin(a)) >> 15); }
static void draw_ring(void)
{
    uint32_t t = U.part & 3u, i, s0 = E.s & ~15u, now = E.s & 15u;
    uint16_t col = TCOL[t];
    char c[16] = "";
    cv_begin(134, 170, PAPER);
    if (E.playing) {                                 /* the pointer: a pale wedge and an ink hand */
        int32_t a = step_ang(now) + (int32_t)((E.stepPh >> 16) * 64u >> 16), q;
        for (q = a - 14; q <= a + 14; q += 2) ray(RCX, RCY, RR - 6, q, mixc(PAPER, KNC[2], 4));
        ray(RCX, RCY, RR - 4, a, INK);
    }
    if (t == 2u && P.kit) {
        static const uint8_t RING[11] = {0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2};        /* BD | SD LT MT HT RS CP | CH OH CR RD */
        static const int32_t RAD[3] = {24, 38, 52};
        uint16_t on[11], ac[11];
        uint32_t r;
        drum_masks(s0, on, ac);
        for (r = 0; r < 3u; r++) circ(RCX, RCY, RAD[r], FAINT);
        for (i = 0; i < 16u; i++)
            for (r = 0; r < 3u; r++) {
                uint32_t v, hit = 0, acc = 0; int32_t x, y;
                for (v = 0; v < 11u; v++) if (RING[v] == r && !(P.drMute >> v & 1u) && (on[v] >> i & 1u)) { hit = 1; acc |= ac[v] >> i & 1u; }
                ring_pos(i, RAD[r], &x, &y);
                if (hit) dot(x, y, acc ? 4 : 3, E.playing && i == now ? INK : acc ? REDC : col);
                else cv_rect(x, y, 1, 1, mixc(PAPER, INK, 8));
            }
        scat(c, P.kit == 2u ? "808" : "909");
    } else {
        int16_t nt[16]; uint8_t fl[16];
        int32_t ref = P.root + (t == 0u ? 6 : t == 3u ? 30 : 18);
        for (i = 0; i < 16u; i++) {
            uint32_t s = s0 + i;
            nt[i] = -1; fl[i] = 0;
            if (t == 0u && SG.on) {                   /* a song: its bar */
                uint32_t v = WA.v[16u + i];
                if (v != NONE) { nt[i] = (int16_t)win_note(&WA, 16u + i); fl[i] = (uint8_t)((WA.f[16u + i] & 1u) | (win_leg(&WA, 16u + i) ? 2u : 0u)); }
            } else if (t == 0u && STEPS) {            /* the steps as written (also when stopped): the half that plays */
                uint32_t L = step_len(), j = ((E.s % L) & 16u) + i, v;
                if (j < L && (v = P.lane[0][j]) != NONE) { nt[i] = (int16_t)step_note(j, L); fl[i] = (uint8_t)((accB() >> j & 1u) | (legato_into(j, L) ? 2u : 0u)); }
            } else if (t == 0u) { acid_t a; eval_acid(s, 0, &a, 1); if (a.gate && !a.rest) { nt[i] = (int16_t)((a.y + 128) >> 8); fl[i] = (uint8_t)((a.acc ? 1u : 0u) | (a.age >= 65536u ? 2u : 0u)); } }
            else if (t == 1u) { voice_t v; eval_bio(s, 8000u, &v); if (v.on) { nt[i] = (int16_t)((v.y + 128) >> 8); fl[i] = (uint8_t)(i && nt[i - 1u] >= 0 ? 2u : 0u); } }
            else if (t == 2u) { voice_t v; eval_kick(s, 0, &v); if (v.on && v.kt == 0u) nt[i] = (int16_t)P.root; }
            else if (P.terms & T_PERC) {
                uint32_t h = perc_pat(s);
                if (h) nt[i] = (int16_t)(P.root + 24);
                else if (i && (nt[i - 1u] >= 0 || (fl[i - 1u] & 2u)) && P.hPat <= 1u) fl[i] = 2u;   /* a held chord */
            }
        }
        circ(RCX, RCY, RR, FAINT);
        for (i = 1; i < 16u; i++) if (fl[i] & 2u) arcq(RCX, RCY, RR + 1, step_ang(i - 1u), step_ang(i), 3, col);   /* ties, slides, held notes */
        for (i = 0; i < 16u; i++) {
            int32_t x, y, d;
            if (nt[i] < 0 || (fl[i] & 2u && t != 0u)) {
                if (nt[i] < 0 && !(fl[i] & 2u)) { ring_pos(i, RR, &x, &y); cv_rect(x - 1, y - 1, 3, 3, INK); }
                continue;
            }
            d = (nt[i] - ref) * 2 / 3; d = d < -10 ? -10 : d > 10 ? 10 : d;
            ring_pos(i, RR + d, &x, &y);
            dot(x, y, (fl[i] & 1u) ? 5 : 4, E.playing && i == now ? INK : col);
        }
        if (t == 0u) { if (nt[now] >= 0 && E.playing) snote(c, nt[now]); else snote(c, P.root + 12); }
        else if (t == 1u) scat(c, L_ANIMAL[P.bAnimal % 11u]);
        else if (t == 2u) scat(c, "KICK");
        else scat(c, L_HTYPE[P.hType % 7u]);
    }
    { int32_t w = text_w(&FONT_S, c); if (w <= 60) tx(RCX - w / 2, RCY - 8, &FONT_S, c, INK, PAPER); else t5(RCX - (int32_t)slen(c) * 3, RCY - 3, c, INK); }
    if (SG.on) {                                     /* the song's sections: a ribbon along the bottom (its left part) */
        const sg_song_t *g = sgs();
        uint32_t k;
        for (k = 0; k < g->nsec; k++) {
            int32_t a = 4 + 232 * (g->secBar[k] - 1) / g->nb, b = 4 + 232 * (k + 1u < g->nsec ? g->secBar[k + 1u] - 1 : g->nb) / g->nb;
            cv_rect(a, 158, b - a - 2, 6, k == SG.sec ? RGB(236, 201, 112) : FAINT);
        }
        if (SG.curSb >= 0) cv_rect(4 + 232 * SG.curSb / g->nb, 154, 2, 14, REDC);
    }
    draw_readout(0, 70, 214);
    cv_blit(0, 70);
}

/* ---- right of the ring: the sound of the track: its wave, and its harmonics under the filter */
#define SX 134
static void sim303(float fcRatio, float res, int32_t sq, int32_t y0, int32_t h, uint16_t c)   /* a saw (square) through a 4-pole ladder, 2.5 periods */
{
    float y1 = 0, y2 = 0, y3 = 0, y4 = 0, ph = 0, g = 1.0f - f_exp2(-1.442695f * 6.2831853f * fcRatio / 50.0f), k = res * 3.7f, out;
    int32_t i, py0 = 0, yy;
    if (g > 0.95f) g = 0.95f;
    (void)0;
    for (i = -200; i < 100; i++) {
        float x = sq ? (ph < 0.5f ? 0.8f : -0.8f) : 1.0f - 2.0f * ph, u = x - k * y4;
        if (u > 1.0f) u = 1.0f; else if (u < -1.0f) u = -1.0f;
        u -= u * u * u / 3.0f;
        y1 += g * (u - y1); y2 += g * (y1 - y2); y3 += g * (y2 - y3); y4 += g * (y3 - y4);
        out = y4 * (1.0f + k * 0.5f);
        ph += 1.0f / 50.0f; if (ph >= 1.0f) ph -= 1.0f;
        if (i < 2) continue;
        yy = y0 + h / 2 - (int32_t)(out * (float)h * 0.36f);
        yy = yy < y0 ? y0 : yy > y0 + h ? y0 + h : yy;
        if (i > 2) cv_line(i - 1, py0, i, yy, c);
        py0 = yy;
    }
}
static int32_t fx_log(float f) { return (int32_t)(14.0f * f_log2(f / 40.0f)); }   /* 40 Hz .. 10 kHz over 104 px */
static void spectrum(float f0, float fc, float res, int32_t y0, int32_t h, uint16_t col, int lp)
{
    int32_t base = y0 + h, x, py0 = 0;
    uint32_t k;
    float K = res * 3.8f;
    cv_rect(0, base, 104, 1, INK);
    for (k = 1; k < 120u; k++) {                     /* the harmonics: 1/k, under the filter */
        float f = f0 * (float)k, r = f / fc, a, re, im, H;
        if (f > 10000.0f) break;
        re = 1.0f - 6.0f * r * r + r * r * r * r + K; im = 4.0f * r - 4.0f * r * r * r;
        H = lp ? (1.0f + K) / (1.0f + f_exp2(0.5f * f_log2(re * re + im * im + 1e-6f))) : 1.0f;
        a = H / (float)k; if (a > 1.0f) a = 1.0f;
        x = fx_log(f);
        if (x >= 0 && x < 104) cv_rect(x, base - (int32_t)(f_exp2(0.5f * f_log2(a + 1e-6f)) * (float)h * 0.9f), 1, (int32_t)(f_exp2(0.5f * f_log2(a + 1e-6f)) * (float)h * 0.9f), f > fc && lp ? FAINT : INK);
    }
    if (!lp) return;
    for (x = 0; x < 104; x++) {                      /* the filter's curve, its peak at the cutoff */
        float f = 40.0f * f_exp2((float)x / 14.0f), r = f / fc, re = 1.0f - 6.0f * r * r + r * r * r * r + K, im = 4.0f * r - 4.0f * r * r * r;
        float H = (1.0f + K) / (1.0f + f_exp2(0.5f * f_log2(re * re + im * im + 1e-6f)));
        int32_t yy = base - (int32_t)(H * (float)h * 0.42f);
        yy = yy < y0 ? y0 : yy;
        if (x) { cv_line(x - 1, py0, x, yy, col); cv_line(x - 1, py0 + 1, x, yy + 1, col); }
        py0 = yy;
    }
    x = fx_log(fc); if (x >= 0 && x < 104) cv_rect(x, y0, 1, h, col);
}
static uint8_t drLvl[11]; static uint32_t drLast = 0xFFFFFFFFu;
static void draw_scope(void)
{
    uint32_t t = U.part & 3u, k;
    cv_begin(106, 170, PAPER);
    if (t == 0u || (t == 1u && P.bAnimal == 0u)) {  /* ACID (ZOO's ACID B): the 303's wave and its filter */
        acid_t a; int32_t q8, note; float fc, res;
        if (t == 0u) {
            eval_acid(E.s, E.stepPh >> 16, &a, 0);
            q8 = cutoff_at(&a, E.s, E.stepPh >> 16); res = (float)mv(1, E.s, E.stepPh >> 16) / 127.0f; note = (a.y + 128) >> 8;
        } else {
            zoo_t z; zoo_eval(E.s, E.stepPh >> 16, &z);
            q8 = z.cq ? z.cq : 24 * 256 + P.zCut * 144; res = (float)P.zRes / 127.0f; note = z.o.gate ? (z.o.y + 128) >> 8 : P.root + 12;
        }
        fc = hz_of(q8); if (fc < 30.0f) fc = 30.0f;
        {   /* the display's own 303: past 12x the note the ringing is finer than a pixel (a grey hatch), so it stops there */
            float ratio = fc / hz_of(note << 8);
            sim303(ratio > 12.0f ? 12.0f : ratio, res, t == 0u && P.aWave == 1u, 8, 50, INK);
        }
        spectrum(hz_of(note << 8), fc, res, 66, 70, t == 0u ? REDC : TCOL[1], 1);
    } else if (t == 1u && P.bAnimal < 7u) {          /* ZOO's call: its pitch (ink) and its mouth (CUTOFF, the track's colour) */
        const call_t *c = &CALLS[P.bAnimal - 1u];
        uint32_t x, len = ((uint32_t)c->n << 16) / c->div;
        int32_t pp = 0, pc = 0;
        for (x = 0; x < 100u; x++) {
            acid_t o; int32_t cq, rs, yp, yc;
            if (!call_shape(c, len / 100u * x, 0, &o, &cq, &rs)) continue;
            yp = 40 - ((o.y >> 8) * 22) / 12; yp = yp < 6 ? 6 : yp > 58 ? 58 : yp;
            yc = 58 - ((cq - 40 * 256) * 48) / (60 * 256); yc = yc < 6 ? 6 : yc > 58 ? 58 : yc;
            if (x) { cv_line((int32_t)x - 1, pp, (int32_t)x, yp, o.gate ? INK : FAINT); cv_line((int32_t)x - 1, pc, (int32_t)x, yc, TCOL[1]); cv_line((int32_t)x - 1, pc + 1, (int32_t)x, yc + 1, TCOL[1]); }
            pp = yp; pc = yc;
        }
        {
            zoo_t z; float fc;
            int on = zoo_eval(E.s, E.stepPh >> 16, &z) && z.o.gate;
            fc = hz_of(on ? z.cq : 24 * 256 + 72 * 144);
            spectrum(hz_of(on ? (z.o.y + 128) & ~255 : (P.root + 24) << 8), fc, (float)c->res / 127.0f, 66, 70, TCOL[1], 1);
        }
    } else if (t == 1u) {                            /* the old animals: their wavetable */
        uint32_t x; int32_t pp = 0;
        for (x = 0; x < 100u; x++) {
            int32_t v = BIO_WT[((P.bAnimal - 7u) & 3u) * 256u + ((x * 256u / 40u) & 255u)], yy = 33 - (v * 22) / 32767;
            if (x) cv_line((int32_t)x - 1, pp, (int32_t)x, yy, INK);
            pp = yy;
        }
        spectrum(hz_of((P.root + 24) << 8), 3000.0f, 0.3f, 66, 70, TCOL[1], 0);
    } else if (t == 2u) {                            /* the drums: each row's last hit, fading */
        uint16_t on[11], ac[11];
        uint32_t r, st = E.s;
        if (E.playing && st != drLast) {
            drLast = st; drum_masks(st, on, ac);
            for (r = 0; r < 11u; r++) if ((on[r] >> (st & 15u) & 1u) && !(P.drMute >> r & 1u)) drLvl[r] = (ac[r] >> (st & 15u) & 1u) ? 100u : 72u;
        }
        for (r = 0; r < 11u; r++) {
            int32_t y = 8 + (int32_t)r * 12;
            t5(0, y, drn(r), r == P.drSel ? REDC : INK);
            cv_rect(16, y + 2, 84, 3, FAINT);
            if (drLvl[r]) { cv_rect(16, y + 1, (int32_t)drLvl[r] * 84 / 100, 5, TCOL[2]); drLvl[r] = (uint8_t)(drLvl[r] > 6u ? drLvl[r] - 6u : 0u); }
        }
        if (!P.kit) t5(0, 140, "KIT: FUNC", INK); else t5(0, 140, "SEQ = GRID", mixc(PAPER, INK, 9));
    } else {                                         /* KEYS: a few periods of its sound, its partials */
        uint32_t x; int32_t pp = 0;
        float b = (float)P.hTone / 127.0f;
        for (x = 0; x < 100u; x++) {
            float p = (float)x / 40.0f, v;
            switch (P.hType) {
            case KT_PAD: v = fsin(p) + 0.5f * fsin(p * 1.003f + 0.16f) + 0.18f * fsin(2.0f * p) + b * (0.32f * fsin(3.0f * p) + 0.22f * fsin(4.0f * p)); v *= 0.5f; break;
            case KT_EPIANO: v = fsin(p + 0.16f * (1.0f + 2.2f * b) * fsin(p)) * 0.8f; break;
            case KT_BELL: v = fsin(p + 0.16f * (1.5f + 4.0f * b) * fsin(3.5f * p)) * 0.8f; break;
            case KT_PIANO: v = (fsin(p + 0.16f * (0.8f + 2.0f * b) * fsin(p)) + 0.3f * fsin(2.003f * p)) * 0.6f; break;
            case KT_DRONE: { uint32_t n; v = 0; for (n = 1; n <= 7u; n++) v += fsin((float)n * p) / (float)n; v *= 0.5f; break; }
            default: v = (1.0f - 2.0f * (p - (float)(int32_t)p)) * (0.4f + 0.4f * b) + 0.2f * fsin(5.0f * p); break;   /* a plucked string, roughly */
            }
            { int32_t yy = 33 - (int32_t)(v * 24.0f); yy = yy < 6 ? 6 : yy > 60 ? 60 : yy; if (x) cv_line((int32_t)x - 1, pp, (int32_t)x, yy, INK); pp = yy; }
        }
        spectrum(hz_of((P.root + 24) << 8), 200.0f + 4000.0f * b, 0.2f, 66, 70, TCOL[3], 1);
    }
    if (SG.on) {                                     /* the ribbon's right part */
        const sg_song_t *g = sgs();
        for (k = 0; k < g->nsec; k++) {
            int32_t a = 4 + 232 * (g->secBar[k] - 1) / g->nb - SX, e = 4 + 232 * (k + 1u < g->nsec ? g->secBar[k + 1u] - 1 : g->nb) / g->nb - SX;
            cv_rect(a, 158, e - a - 2, 6, k == SG.sec ? RGB(236, 201, 112) : FAINT);
        }
        if (SG.curSb >= 0) cv_rect(4 + 232 * SG.curSb / g->nb - SX, 154, 2, 14, REDC);
    } else
        for (k = 0; k < 4u; k++) {                   /* the four tracks: which one is selected, which are muted */
            int32_t x = 4 + (int32_t)k * 25;
            if (k == U.part) { cv_rect(x - 2, 154, 22, 1, INK); cv_rect(x - 2, 165, 22, 1, INK); cv_rect(x - 2, 154, 1, 12, INK); cv_rect(x + 19, 154, 1, 12, INK); }
            cv_rect(x, 156, 18, 8, (P.mutes >> k & 1u) || (k == 1u && !(P.terms & T_BIO)) || (k == 3u && !(P.terms & T_PERC)) ? FAINT : TCOL[k]);
        }
    draw_readout(SX, 70, 214);
    cv_blit(SX, 70);
}
static void draw_strip(void)                                  /* 178..240 under the grid, the steps, a key map: the tracks */
{
    uint32_t k;
    cv_begin(240, 62, PAPER);
    cv_rect(0, 1, 240, 1, FAINT);
    for (k = 0; k < 4u; k++) {
        int32_t x = 6 + (int32_t)k * 58;
        uint16_t c = (P.mutes >> k & 1u) || (k == 1u && !(P.terms & T_BIO)) || (k == 3u && !(P.terms & T_PERC)) ? FAINT : TCOL[k];
        if (k == U.part) cv_rect(x - 3, 10, 56, 30, CREAM);
        cv_rect(x, 16, 9, 9, c);
        tx(x + 13, 13, &FONT_S, k == 2u ? (P.kit == 2u ? "808" : P.kit ? "909" : "KICK") : TRK[k], INK, k == U.part ? CREAM : PAPER);
    }
    if (SG.on) t5(6, 48, "A SONG: PLAY = YOUR SET", mixc(PAPER, INK, 9));
    cv_blit(0, 178);
}

static void draw_drums(void)                                 /* T3 on the 909: every row, each step over its white key */
{
    uint32_t r, j, now = E.playing ? E.s & 15u : 99u;
    uint16_t on[11], acs[11];
    drum_masks(E.playing ? E.s : 0u, on, acs);
    cv_begin(240, 108, PAPER);
    if (SG.on) { cv_rect(4, 2, 232, 11, TCOL[2]); t5(6, 4, "THE SONG'S 909   ROW: BLACK KEY / OCT", LIME); }
    else { cv_rect(4, 2, 232, 11, LIME); t5(6, 4, "ROW: BLACK KEY/OCT  WHITE: ON>ACC>OFF", INK); }
    for (r = 0; r < 11u; r++) {
        int32_t y = 15 + (int32_t)r * 8, sel = r == P.drSel, mute = P.drMute >> r & 1u;
        if (sel) cv_rect(0, y - 1, 240, 8, mixc(PAPER, RGB(255, 255, 232), 10));
        t5(2, y, drn(r), sel ? REDC : mixc(PAPER, INK, mute ? 5 : 13));
        for (j = 0; j < 16u; j++) {
            int32_t x = 17 + (int32_t)j * 14, hit = on[r] >> j & 1u, ac = acs[r] >> j & 1u;
            uint16_t c = hit ? (ac ? REDC : (mute ? mixc(PAPER, INK, 6) : INK)) : mixc(PAPER, INK, (j & 3u) ? 3 : 5);
            cv_rect(x, y, 12, 6, c);
            if (j == now) { cv_rect(x, y + 6, 12, 1, INK); }
        }
    }
    draw_readout(0, 70, 140);
    cv_blit(0, 70);
}
static void draw_steps(void)                                 /* SEQ > STEPS: ACID's 16 steps, each over its white key */
{
    uint32_t L = SG.on ? 16u : step_len(), i, now = E.playing && (STEPS || SG.on) ? E.s % L : 99u, base = SG.on ? 0u : step_base();
    int32_t lo = (SG.on ? 33 : P.root) - 5, prevY = -1;
    cv_begin(240, 108, PAPER);
    if (SG.on) { cv_rect(4, 3, 232, 11, TCOL[0]); t5(6, 5, "THE SONG'S 303   KEYS: TRANSPOSE", LIME); }
    else if (!STEPS) { cv_rect(4, 3, 232, 11, REDC); t5(6, 5, "THE FORMULA PLAYS: KNOB2 -> STEPS", LIME); }
    else { cv_rect(4, 3, 232, 11, LIME); t5(6, 5, "HOLD STEP+KNOBS  OCT+ NEW  OCT- MUTATE", INK); }
    cv_rect(0, 86 - 10, 240, 1, mixc(PAPER, INK, 4));                                  /* the root */
    for (i = base; i < base + 16u; i++) {
        int32_t x = 2 + (int32_t)(i - base) * 15, y, n;
        uint32_t v = SG.on ? WA.v[16u + i] : P.lane[0][i], acc = SG.on ? WA.f[16u + i] & 1u : accB() >> i & 1u, sld = SG.on ? WA.f[16u + i] >> 1 & 1u : sldB() >> i & 1u;
        char num[4] = "";
        if (i >= L) { cv_rect(x, 18, 14, 84, mixc(PAPER, INK, 2)); prevY = -1; continue; }
        if (i == now) cv_rect(x, 18, 14, 84, mixc(PAPER, INK, 4));
        if (U.stepHeld == (int8_t)i) { cv_rect(x - 1, 17, 16, 1, REDC); cv_rect(x - 1, 102, 16, 1, REDC); cv_rect(x - 1, 17, 1, 86, REDC); cv_rect(x + 14, 17, 1, 86, REDC); }
        sint(num, (int32_t)i + 1);
        t5(x + (i < 9u ? 4 : 1), 93, num, mixc(PAPER, INK, i == now ? 16 : 9));
        if (v == NONE) { cv_rect(x + 5, 84, 4, 1, mixc(PAPER, INK, 7)); prevY = -1; continue; }
        n = SG.on ? win_note(&WA, 16u + i) : step_note(i, L);
        y = 86 - (n - lo) * 2; y = y < 20 ? 20 : y > 88 ? 88 : y;
        if ((v & 128u) && prevY >= 0) cv_rect(x - 3, y - 2, 17, 5, mixc(PAPER, INK, 12));        /* a tie: one long bar */
        else cv_rect(x + 1, y - 2 - (int32_t)acc, 12, 5 + 2 * (int32_t)acc, acc ? REDC : INK);
        if (sld && i + 1u < L && i + 1u < base + 16u && (SG.on ? WA.v[17u + i] : P.lane[0][i + 1u]) != NONE) {   /* a slide: a line into the next note */
            int32_t n2 = SG.on ? win_note(&WA, 17u + i) : step_note(i + 1u, L), y2 = 86 - (n2 - lo) * 2;
            y2 = y2 < 20 ? 20 : y2 > 88 ? 88 : y2;
            cv_line(x + 12, y, x + 16, y2, INK); cv_line(x + 12, y + 1, x + 16, y2 + 1, INK);
        }
        prevY = y;
    }
    draw_readout(0, 70, 140);
    cv_blit(0, 70);
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
    if (U.held == BTN_SAVE) scat(t, ":  TAP = LOAD   HOLD = SAVE");
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
    if (U.held == BTN_SAVE) draw_readout(0, 70, 140);             /* "LET GO: SAVE TO U3" */
    cv_blit(0, 70);
}

/* ================================================================ the main loop */
#define OLDP 472u                                    /* sizeof params_t in 0.33 / 0.34 (up to sldHi) */
#define P35 ((__builtin_offsetof(params_t, mClk) + 1u) & ~1u)   /* in 0.35 (up to drPot) */
#define P36 ((__builtin_offsetof(params_t, bootDemo) + 1u) & ~1u)   /* in 0.36 - 0.38 (up to mCh) */
#define P39 ((__builtin_offsetof(params_t, zCut) + 1u) & ~1u)       /* in 0.39 / 0.40 (up to demoSong) */
_Static_assert(__builtin_offsetof(params_t, kit) == OLDP, "the 0.33 params end where kit begins");
static uint8_t oldRaw[4084];
static int load_old_store(void)                     /* 0.35 - 0.40: {cur; slot[6]; used[8]}; 0.33 / 0.34: {cur; slot[7]; used[8]} */
{
    uint32_t k, sz, ns;
    if (settings_load(oldRaw, P39 * 7u + 8u) == 0) { sz = P39; ns = 6u; }
    else if (settings_load(oldRaw, P36 * 7u + 8u) == 0) { sz = P36; ns = 6u; }
    else if (settings_load(oldRaw, P35 * 7u + 8u) == 0) { sz = P35; ns = 6u; }
    else if (settings_load(oldRaw, OLDP * 8u + 8u) == 0) { sz = OLDP; ns = 7u; }
    else return 0;
    memset(&ST, 0, sizeof ST);
    for (k = 0; k <= NSLOT; k++) {
        params_t *q = k ? &ST.slot[k - 1u] : &ST.cur;
        memcpy(q, oldRaw + sz * k, sz);
        if (sz == OLDP) drum_defaults(q);
        if (sz != P36 && sz != P39) { q->mClk = 0; q->mNote = 0; q->mCh = 0; }
        if (sz != P39) { q->bootDemo = 1; q->demoSong = 0; }
        /* 0.41: BIO's animals are ZOO's voices 7-10; PERC became KEYS (its five bytes: the first preset) */
        if (q->bAnimal < 7u) q->bAnimal = (uint8_t)(7u + (q->bAnimal & 3u));
        q->zCut = 52; q->zRes = 96; q->zEnv = 80; q->zDec = 40;
        q->hType = PRE_PERC[0].v[0]; q->hTone = PRE_PERC[0].v[1]; q->hDec = PRE_PERC[0].v[2]; q->hPat = PRE_PERC[0].v[3]; q->hLvl = PRE_PERC[0].v[4]; q->pre[3] = 0;
        if (k) ST.used[k - 1u] = oldRaw[sz * (ns + 1u) + k - 1u];
    }
    return ST.cur.magic == MAGIC;
}
static void app_main(void)
{
    uint32_t prev = 0, lastFrame = 0, lastTop = 0, k, ledT = 0;
    if (settings_load(&ST, sizeof ST) == 0 && ST.cur.magic == MAGIC) P = ST.cur;
    else if (load_old_store()) P = ST.cur;
    else {
        memset(&ST, 0, sizeof ST);
        if (settings_load(&P, __builtin_offsetof(params_t, accHi)) == 0 && P.magic == MAGIC) { P.accHi = P.sldHi = 0; drum_defaults(&P); }   /* 0.32 */
        else if (settings_load(&P, (__builtin_offsetof(params_t, aFilt) + 1u) & ~1u) == 0 && P.magic == MAGIC) {   /* 0.31 */
            P.aFilt = 0; memset(P.mot, 255, sizeof P.mot); P.accHi = P.sldHi = 0; drum_defaults(&P);
        } else defaults();
    }
    if (P.aFilt > 1u) P.aFilt = 0;
    drum909_init(&D9); d9ready = 1; drum808_init(&D8); d8ready = 1; drDirty = 0x7FFu;
    sinf_init();              /* 0.41: the 808 (its tables take a moment: before the sound needs it) */
    mot_count();
    count_lanes();
    for (k = 0; k <= 16u; k++) pal_paper[k] = mixc(PAPER, INK, k);
    lcd_fill(0, 0, 240, 240, PAPER);
    toast(APP_NAME " ", APP_VER);                                           /* the version, for a second */
    if (P.bootDemo && P.mClk != 2u) sg_enter(P.demoSong ? P.demoSong - 1u : fm1_ticks() % SG_PLAYABLE, 1);   /* a song to start with; PLAY = your own set */
    for (;;) {
        uint32_t keys, down, up, b, rel, e;
        platform_poll();
        /* keys */
        keys = fm1_in.notes;
        down = (keys & ~prev) | fm1_input_note_edges();
        up = prev & ~keys;
        prev = keys;
        for (k = 0; k < 27u; k++) {
            if (down >> k & 1u) { if (U.held >= 0 && has_layer(U.held)) layer_key(k, 1); else if (U.group == G_DRUM) drum_key(k); else if (steps_page() && !U.rec && !SG.on) step_key(k, 1); else key_down(k); }
            if (up >> k & 1u) {
                if (U.stepHeld >= 0 && U.stepKey == k) step_key(k, 0);
                else if (U.held >= 0 && has_layer(U.held)) layer_key(k, 0); else key_up(k);
            }
        }
        /* buttons: PLAY and OCT at once; the others: a tap = page, a hold = layer */
        b = fm1_input_edges(&rel);
        for (k = 0; k < 14u; k++) {
            if (b >> k & 1u) {
                if (k == BTN_PLAY && SG.on) { sg_exit(); toast("YOUR SET", 0); }
                else if (k == BTN_PLAY) { if (E.playing) E.stopReq = 1; else { E.startReq = 1; if (P.mClk == 2u) { extTarget = 0; toast("WAITING FOR MIDI CLOCK", 0); } } }
                else if ((k == BTN_OCT_UP || k == BTN_OCT_DOWN) && U.group == G_DRUM) {
                    int32_t r = (int32_t)P.drSel + (k == BTN_OCT_UP ? 1 : -1);
                    P.drSel = (uint8_t)(r < 0 ? 10 : r > 10 ? 0 : r); drAud |= (uint16_t)(1u << P.drSel); toast(drn(P.drSel), 0);
                }
                else if ((k == BTN_OCT_UP || k == BTN_OCT_DOWN) && SG.on) sg_skip(k == BTN_OCT_UP ? 1 : -1);
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
        /* MIDI: out what the audio queued; in: the clock and START / STOP / CONTINUE when CLOCK = IN */
        while (mqR != mqW) { uint32_t m = mqOut[mqR % MQN]; mqR++; midi_send(m & 255u, m >> 8 & 255u, m >> 16 & 255u); }
        {
            uint8_t st, d1, d2;
            static uint32_t lastT, avg;
            while (midi_get(&st, &d1, &d2)) {
                if (P.mClk != 2u) continue;
                if (st == 0xFAu) { extTarget = 0; lastT = 0; E.startReq = 1; toast("MIDI START", 0); }
                else if (st == 0xFBu) { if (!E.playing) { E.startReq = 1; } }
                else if (st == 0xFCu) { if (E.playing) E.stopReq = 1; }
                else if (st == 0xF8u) {
                    if (E.playing || E.startReq) extTarget++;
                    if (lastT) {   /* the tempo, from the ticks' spacing (averaged over a beat) */
                        uint32_t dt = fm1_ms - lastT;
                        avg = avg ? avg + ((int32_t)(dt * 256u) - (int32_t)avg) / 24 : dt * 256u;
                        if (avg) { uint32_t b = 60000u * 256u / (avg * 24u); P.bpm = (uint8_t)(b < 80u ? 80u : b > 170u ? 170u : b); }
                    }
                    lastT = fm1_ms;
                }
            }
        }
        tick_rec(); tick_motion();
        TRK[2] = P.kit == 2u ? "808" : P.kit ? "909" : "KICK";
        if (fpu_sum(64)) { if (!fpuMain) fpuMain = 1; } else fpuMain++;   /* 0.34: floats here, interrupted by the audio */
        /* LEDs: the page's button, PLAY, REC */
        if (fm1_ms - ledT > 50u) {
            static const int8_t GBTN[G_N] = {BTN_HOME, BTN_EDIT, BTN_ENV, BTN_LFO, BTN_FX, BTN_SEL, BTN_ARP, BTN_SEQ, BTN_GLO, BTN_SEQ};
            ledT = fm1_ms;
            for (k = 2; k < 12u; k++) fm1_led_key(k, (int)k == GBTN[U.group]);
            fm1_led_key(BTN_PLAY, E.playing && ((E.s & 3u) < 2u));
            fm1_led_key(BTN_REC, U.rec);
        }
        /* the screen: the graph about 30 times a second, the rest when it changes */
        if (fm1_ms - lastFrame >= 33u) {
            lastFrame = fm1_ms;
            int fxShow = U.held == BTN_FX && (fxBufKey >= 0 || X.flags || fxFn || riseStart >= 0 || fxThrow);
            if (U.held >= 0 && has_layer(U.held) && fm1_ms - U.heldT > 160u && !fxShow) { draw_keymap(); draw_strip(); }
            else if (U.group == G_DRUM && !fxShow) { draw_drums(); draw_strip(); }
            else if (steps_page() && !fxShow) { draw_steps(); draw_strip(); }
            else { draw_ring(); draw_scope(); }
            if (fm1_ms - lastTop >= 100u || (U.touch >= 0 && fm1_ms - U.touchT < 900u)) { lastTop = fm1_ms; draw_top(); }
        }
        if (SG.on && SG.ended) {
            if (SG.demo) sg_enter((SG.song + 1u) % SG_PLAYABLE, 1);
            else { sg_exit(); toast("THE END: YOUR SET", 0); }
        }
        { static uint32_t pkT; if (fm1_ms - pkT >= 2000u) { pkT = fm1_ms; cpu_pk = cpu_max; cpu_max = 0; } }
        /* keep the settings when nothing has changed for 20 s and the music is stopped (an erase stalls the sound) */
        if (U.dirty && !E.playing && fm1_ms - U.dirtyT > 20000u) save_all();
        /* SAVE held, a white key held long: say that letting go saves */
        if (U.held == BTN_SAVE && U.slotK >= 0 && !U.slotArm && fm1_ms - U.slotT > 700u) {
            char t[24] = "LET GO: SAVE TO U"; sint(t, U.slotK + 1); toast(t, 0); U.slotArm = 1;
        }
    }
}
