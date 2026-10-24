// Function Acid: the eight acid songs (tools/songs/songs8.js, the same data as acid-8-tracks.netlify.app) → firmware/gen/songs8.h
//   node tools/export_songs.mjs
// Each song is run bar by bar here (its arrange() and step() are JavaScript); the firmware gets plain tables:
// unique 303 bars (16 steps: note offset + flags), unique drum bars (11 rows: on / accent masks), each song's bar list,
// its sections, and its knob lanes as keyframes. Half-step drum hits (inside rolls) are dropped.
import fs from 'node:fs';
import vm from 'node:vm';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const ctx = {}; vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(ROOT, 'tools/songs/songs8.js'), 'utf8') + '\nthis.S8 = ACID_SONGS8();', ctx);

// the order on the FM-1: HOME held + white keys 1-8
const ORDER = ['raw', 'hypno', 'layer', 'scream', 'breaks', 'big', 'deep', 'raga'];
const NAME = { raw: 'CHICAGO', hypno: 'MINIMAL', layer: 'TWO 303S', scream: 'SCREAM', breaks: 'BREAKS', big: 'BIG ROOM', deep: 'DEEP', raga: 'RAGA' };
// the sections' names for the 5x7 font (capitals, ASCII)
const SEC = { 'Intro': 'INTRO', 'Outro': 'OUTRO', 'Break': 'BREAK', 'Breakdown': 'BREAKDOWN', 'Ride': 'RIDE', 'Drop': 'DROP', 'Groove': 'GROOVE',
  '拧开': 'OPEN UP', '拍手': 'CLAPS', '坦普拉': 'TANPURA', '起句': 'PHRASE 1', '迪斯科': 'DISCO', '第二句': 'PHRASE 2', '只剩持续音': 'DRONE ONLY',
  '第三句': 'PHRASE 3', '第四句': 'PHRASE 4', '积蓄 1': 'BUILD 1', '积蓄 2': 'BUILD 2', '第一个高点': 'PEAK 1', '再积蓄': 'BUILD AGAIN',
  '最高点': 'PEAK', '303 × 2': '303 X 2', '换一组': 'NEW PAIR', '最满': 'FULL', '拧到尖叫': 'SCREAM', '再尖叫一次': 'SCREAM AGAIN',
  '碎拍': 'BREAKBEAT', '方波': 'SQUARE', '换一句': 'NEW LINE', '半拍': 'HALF TIME', '回来': 'BACK', '最后一句': 'LAST LINE',
  '和弦': 'CHORDS', '第二台 303': 'SECOND 303', '铺垫': 'BUILD', '底鼓进来': 'KICK IN', '闭镲': 'HATS', '漂着': 'FLOATING' };
const ROWS = ['bd', 'sd', 'lt', 'mt', 'ht', 'rs', 'cp', 'ch', 'oh', 'cr', 'rd'];

const acidBars = [], acidKey = new Map(), drumBars = [], drumKey = new Map(), kf = [], padCh = [], padKey = new Map(), padBar = [];
const intern = (list, map, rec) => { const k = JSON.stringify(rec); if (!map.has(k)) { map.set(k, list.length); list.push(rec); } return map.get(k); };
const songs = [], barList = [];
const lane = (K, conv) => { const off = kf.length; for (const [x, v] of K) kf.push([Math.max(0, Math.round((x - 1) * 64)), conv(v)]); return [off, K.length]; };

for (const id of ORDER) {
  const S = ctx.S8[id](), NB = S.NB;
  // drums: per bar, 11 rows of on / accent bits
  const D = Array.from({ length: NB + 1 }, () => ROWS.map(() => [0, 0]));
  S.arrange((n, j, v, o) => {
    const r = ROWS.indexOf(v); if (r < 0 || j % 1 || n < 1 || n > NB) return;
    const a = o && o.a != null ? o.a : 1; if (a < 0.2) return;
    D[n][r][0] |= 1 << j; if ((o && o.acc) || a >= 0.95) D[n][r][1] |= 1 << j;
  });
  // the tanpura (RAGA): its plucks on the quarters (the same four strings every bar it sounds)
  const tp = { notes: null, first: 0, last: 0 };
  S.arrange((n, j, v, o) => {
    if (v !== 'tp' || !o || o.m == null) return;
    if (!tp.first) tp.first = n; tp.last = n;
    if (!tp.notes) tp.notes = [];
    if (n === tp.first) tp.notes[j / 4 | 0] = o.m;
  });
  // the pads (DEEP): a chord per bar (up to 5 notes), interned
  const padOff = padBar.length;
  if (S.PADS) for (let n = 1; n <= NB; n++) { const c = S.PADS(n); padBar.push(c ? intern(padCh, padKey, c.slice(0, 5)) : 255); }
  // the 303s: per bar, 16 steps [note offset, flags]; flags 1 accent, 2 slide, 4 rest, 8 tie, 16 two hits
  const voices = S.voices.slice(0, 2).map(vd => {
    const out = [];
    for (let n = 1; n <= NB; n++) {
      const bar = [];
      for (let j = 0; j < 16; j++) {
        let p;
        if (n === NB) p = vd.endHit && j === 0 ? [0, 1, 0, 0] : null;
        else p = vd.step ? vd.step(n, j) : (vd.pat(n) || [])[j];
        if (!p || p[3] === 1) bar.push([0, 4]);
        else bar.push([p[0] | 0, (p[1] ? 1 : 0) | (p[2] ? 2 : 0) | (p[3] === 2 ? 8 : 0) | (p[4] ? 16 : 0)]);
      }
      out.push(intern(acidBars, acidKey, bar));
    }
    return out;
  });
  const first = barList.length;
  for (let n = 1; n <= NB; n++) barList.push([voices[0][n - 1], voices[1] ? voices[1][n - 1] : 0xFFFF, intern(drumBars, drumKey, D[n])]);
  const L = vd => vd.lanes;
  const v0 = S.voices[0], v1 = S.voices[1];
  const lanes = {
    cut: lane(L(v0).cut, v => Math.round(v)), res: lane(L(v0).res, v => Math.round(v * 1000)), env: lane(L(v0).env, v => Math.round(v * 1000)),
    dec: lane(L(v0).dec || [[1, 0.3]], v => Math.round(v * 1000)), acc: lane(L(v0).acc || [[1, 0.5]], v => Math.round(v * 1000)),
    drv: lane(v0.drv || [[1, 1.1]], v => Math.round(v * 100)), wave: lane(v0.wave || [[1, 0]], v => Math.round(v * 1000)),
    bcut: lane(v1 ? L(v1).cut : [[1, 1000]], v => Math.round(v)), bres: lane(v1 ? L(v1).res : [[1, 0.8]], v => Math.round(v * 1000)),
    benv: lane(v1 ? L(v1).env : [[1, 0.5]], v => Math.round(v * 1000)), bdec: lane(v1 && L(v1).dec ? L(v1).dec : [[1, 0.25]], v => Math.round(v * 1000)),
    dly: lane(S.DLY || [[1, 0.15]], v => Math.round(v * 1000)), fb: lane(S.FB || [[1, 0.42]], v => Math.round(v * 1000)),
    verb: lane(S.VERB || [[1, 0.15]], v => Math.round(v * 1000)), lvl: lane(S.LVL || [[1, 0]], v => Math.round(v * 10)),
    dhp: lane(S.DHP || [[1, 20]], v => Math.round(v)),
    plvl: lane(S.PADLVL || [[1, 0]], v => Math.round(v * 1000)), pbri: lane(S.PADBRI || [[1, 0]], v => Math.round(v * 1000)),
    ppump: lane(S.PADPUMP || [[1, 0]], v => Math.round(v * 1000)), tpl: lane(S.TPLVL || [[1, 1]], v => Math.round(v * 1000)),
  };
  const risers = (S.RISERS || []).concat((S.ROLLS || []).map(n => [n, 1]));
  songs.push({ id, name: NAME[id], bpm: S.BPM, nb: NB, swing: Math.round((S.SWING || 0) * 100), soft: S.KIT === 'soft' ? 1 : 0, two: v1 ? 1 : 0,
    bdly: v1 ? Math.round((v1.dly || 1) * 100) : 100, first, lanes, kit: S.KIT === 'soft' ? 2 : 1,
    pad: S.PADS ? 1 : 0, padOff: S.PADS ? padOff : 0, tp, gaps: S.GAPS || [], risers,
    secs: (S.SECS || [[1, 'Intro']]).map(([b, nm]) => [b, SEC[nm] || nm.toUpperCase()]) });
}

const C = [];
const arr = (name, type, vals, per = 16) => { const rows = []; for (let i = 0; i < vals.length; i += per) rows.push(vals.slice(i, i + per).join(', ')); return `static const ${type} ${name}[${vals.length}] = {\n    ${rows.join(',\n    ')}\n};\n`; };
C.push('/* SPDX-License-Identifier: GPL-3.0-only */', '/* The eight acid songs (tools/songs/songs8.js): generated by tools/export_songs.mjs, do not edit. */', '');
C.push(`#define SG_N ${songs.length}`, `#define SG_NA ${acidBars.length}`, `#define SG_ND ${drumBars.length}`, '');
C.push(arr('SG_ACID', 'int8_t', acidBars.flatMap(b => b.flatMap(([n, f]) => [n, f])), 32));
C.push(arr('SG_DRUM', 'uint16_t', drumBars.flatMap(b => b.flatMap(([on, ac]) => [on, ac])), 22));
C.push(arr('SG_BARS', 'uint16_t', barList.flat(), 24));
C.push('/* SG_KF: pos (1/64 bar from bar 1) << 16 | value (int16, two\'s complement) */');
C.push(arr('SG_KF', 'uint32_t', kf.map(([p, v]) => (p * 65536 + (v & 0xFFFF)) >>> 0), 12));
C.push('/* the pads (DEEP): chords of up to 5 MIDI notes (0 = none), and the chord of each bar (255 = none) */');
C.push(arr('SG_PADC', 'uint8_t', padCh.flatMap(c => [0, 1, 2, 3, 4].map(i => c[i] || 0)), 20));
C.push(arr('SG_PADBAR', 'uint8_t', padBar.length ? padBar : [255], 32));
C.push('typedef struct { uint16_t off, n; } sg_lane_t;');
C.push('enum { SL_CUT, SL_RES, SL_ENV, SL_DEC, SL_ACC, SL_DRV, SL_WAVE, SL_BCUT, SL_BRES, SL_BENV, SL_BDEC, SL_DLY, SL_FB, SL_VERB, SL_LVL, SL_DHP, SL_PLVL, SL_PBRI, SL_PPUMP, SL_TPL, SL_N };');
C.push('typedef struct { const char *name; uint8_t bpm, swing, soft, two, nsec, ngap, nris; uint16_t nb, first, bdly; sg_lane_t lane[SL_N];');
C.push('                 uint8_t secBar[10]; const char *secName[10]; uint8_t gap[4]; uint8_t ris[6][2];');
C.push('                 uint8_t kit, pad; uint16_t padOff; uint8_t tpNotes[4], tpFirst, tpLast; } sg_song_t;   /* kit 1 = 909, 2 = 808 (the soft kit) */');
C.push('static const sg_song_t SONGS[SG_N] = {');
const LN = ['cut', 'res', 'env', 'dec', 'acc', 'drv', 'wave', 'bcut', 'bres', 'benv', 'bdec', 'dly', 'fb', 'verb', 'lvl', 'dhp', 'plvl', 'pbri', 'ppump', 'tpl'];
for (const s of songs) {
  if (s.secs.length > 10 || s.gaps.length > 4 || s.risers.length > 6) throw new Error('too many sections / gaps / risers in ' + s.id);
  C.push(`    {"${s.name}", ${s.bpm}, ${s.swing}, ${s.soft}, ${s.two}, ${s.secs.length}, ${s.gaps.length}, ${s.risers.length}, ${s.nb}, ${s.first}, ${s.bdly},`);
  C.push(`     {${LN.map(k => `{${s.lanes[k][0]}, ${s.lanes[k][1]}}`).join(', ')}},`);
  C.push(`     {${s.secs.map(x => x[0]).join(', ')}}, {${s.secs.map(x => JSON.stringify(x[1])).join(', ')}}, {${s.gaps.join(', ') || 0}}, {${s.risers.map(r => `{${r[0]}, ${r[1]}}`).join(', ') || '{0, 0}'}},`);
  C.push(`     ${s.kit}, ${s.pad}, ${s.padOff}, {${(s.tp.notes || [0, 0, 0, 0]).map(x => x || 0).join(', ')}}, ${s.tp.first}, ${s.tp.last}},`);
}
C.push('};', '');
fs.writeFileSync(path.join(ROOT, 'firmware/gen/songs8.h'), C.join('\n'));
const bytes = acidBars.length * 32 + drumBars.length * 44 + barList.length * 6 + kf.length * 4;
console.log(`songs8.h: ${songs.length} songs, ${barList.length} bars, ${acidBars.length} unique 303 bars, ${drumBars.length} unique drum bars, ${kf.length} keyframes, ~${(bytes / 1024).toFixed(1)} KB`);
for (const s of songs) console.log(' ', s.id, s.name, s.bpm, 'BPM', s.nb, 'bars', s.secs.map(x => x[1]).join(' / '));
