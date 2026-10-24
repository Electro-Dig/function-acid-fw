# acid-cat test: measure the animal calls in the host build's WAV (tests/scripts/acid-cat.txt).
# For each call: the pitch (autocorrelation, 10 ms frames) against the designed curve (steps + 22 ms glides),
# and the loudest harmonic (after undoing the saw's 1/n) against the designed CUTOFF curve.
#   python tests/acid-cat-measure.py build/host/cat.wav
import sys, wave, math
import numpy as np

BPM = 120; ROOT = 33; STEP = 15 / BPM
def q8hz(q): return 440 * 2 ** ((q / 256 - 69) / 12)
# name, n, div, oct, lowOct, st, sld, rest, ct (1/16 steps), cq (Q8)  -- as CALLS[] in dinghy.c
CALLS = {
 'MEOW': (6, 1, 3, 1, [3, 7, 7, 5, 3, 0], 0x1F, 0, [0, 24, 51, 77, 96], [18404, 25364, 24375, 20836, 19039]),
 'MEW': (3, 1, 3, 1, [3, 7, 3], 0x3, 0, [0, 12, 36, 48], [19039, 25183, 20836, 19039]),
 'MOO': (12, 1, 1, 0, [3, 5, 5, 5, 5, 5, 5, 5, 3, 0, 0, 0], 0x7FF, 0, [0, 38, 102, 154, 192], [13703, 16389, 20836, 19393, 14978]),
 'QUACK': (8, 1, 2, 1, [5, 0, 3, 0, 2, 0, 0, 0], 0, 0xAA, [0, 128], [19722, 19722]),
 'WOOF': (16, 2, 2, 1, [5, 2, 0, 0, 5, 2, 0, 0, 0, 0, 0, 0, 5, 2, 0, 0], 0x1011, 0xCFCC, [0, 128], [17764, 17014]),
 'LONG': (16, 1, 3, 1, [3, 7, 7, 7, 7, 7, 7, 5, 5, 5, 3, 3, 0, 0, 0, 0], 0x7FFF, 0, [0, 45, 122, 173, 218, 256], [18404, 25364, 25089, 23100, 20314, 18404]),
}
ENV = {'QUACK': (19, 0.100), 'WOOF': (26, 0.080)}   # semitones, tau
# key-down times (ms) in the script, the call, low?
RUNS = [(1100, 'MEOW', 0), (3600, 'MEW', 0), (6100, 'MOO', 0), (8600, 'QUACK', 0), (11100, 'WOOF', 0), (13600, 'LONG', 0),
        (17100, 'MEOW', 1), (19600, 'MEW', 1), (22100, 'MOO', 1), (24600, 'QUACK', 1), (27100, 'WOOF', 1)]

def design(name, low, t):
    n, div, octv, lo, st, sld, rest, ct, cq = CALLS[name]
    unit = STEP / div
    j = int(t / unit)
    if j >= n: return None, None, False
    ph = t / unit - j
    base = ROOT + 12 * (lo if low else octv)
    a = j
    while a > 0 and rest >> a & 1: a -= 1
    p = base + st[a]
    gate = False
    if not rest >> j & 1:
        if j > 0 and sld >> (j - 1) & 1 and not rest >> (j - 1) & 1:
            p += (st[j - 1] - st[j]) * math.exp(-ph * unit / 0.022)
        gate = (sld >> j & 1 and j + 1 < n and not rest >> (j + 1) & 1) or ph < 40000 / 65536
    tq = t / STEP * 16
    c = float(np.interp(tq, ct, cq))
    if name in ENV and gate:   # the envelope from the attack
        a = j
        while a > 0 and sld >> (a - 1) & 1 and not rest >> (a - 1) & 1: a -= 1
        semis, tau = ENV[name]
        c += 256 * semis * math.exp(-((j - a + ph) * unit) / tau)
    return p, q8hz(c), gate

def f0_of(x, fs, lo, hi):
    x = x - x.mean()
    r = np.correlate(x, x, 'full')[len(x) - 1:]
    a, b = int(fs / hi), int(fs / lo)
    if b >= len(r) or r[0] <= 0: return None
    k = a + int(np.argmax(r[a:b]))
    if r[k] < 0.3 * r[0]: return None
    if 0 < k < len(r) - 1:   # parabolic
        d = (r[k - 1] - r[k + 1]) / (2 * (r[k - 1] - 2 * r[k] + r[k + 1]) + 1e-12); k += d
    return fs / k

w = wave.open(sys.argv[1] if len(sys.argv) > 1 else 'build/host/cat.wav')
fs = w.getframerate(); ch = w.getnchannels()
d = np.frombuffer(w.readframes(w.getnframes()), np.int16).astype(float).reshape(-1, ch).mean(1) / 32768
print(f'{len(d)/fs:.1f} s, {fs} Hz, peak {np.abs(d).max():.2f}, NaN-free')
def measure(on, model, dur=2.2):
    """frames from sample `on`; model(t) -> (pitch MIDI, cutoff Hz, gate) or (None, ..) at the end"""
    errs, hp, cf = [], [], []
    win = int(0.04 * fs); hop = int(0.01 * fs)
    for k in range(0, int(dur * fs), hop):
        p, cut, gate = model(k / fs + 0.02)
        if p is None: break
        x = d[on + k:on + k + win]
        if not gate or len(x) < win or np.sqrt((x ** 2).mean()) < 0.01: continue
        f0e = 440 * 2 ** ((p - 69) / 12)
        f0 = f0_of(x, fs, f0e / 1.6, f0e * 1.6)
        if f0 is None: continue
        errs.append(1200 * math.log2(f0 / f0e))
        # the loudest harmonic, the saw's 1/n undone
        X = np.abs(np.fft.rfft(x * np.hanning(len(x)), 8192)); fr = np.fft.rfftfreq(8192, 1 / fs)
        hs = [(h * f0, X[int(round(h * f0 / fr[1]))] * h) for h in range(1, int(5000 / f0)) if h * f0 < 5000]
        if len(hs) >= 3:
            hp.append(max(hs, key=lambda z: z[1])[0]); cf.append(cut)
    errs = np.array(errs)
    if len(hp) > 3:
        lh, lc = np.log2(hp), np.log2(cf)
        corr = np.corrcoef(lh, lc)[0, 1] if np.std(lc) > 1e-6 and np.std(lh) > 1e-6 else float('nan')
        dif = np.median(np.abs(lh - lc))
    else: corr = dif = float('nan')
    return (f'{len(errs):6d}  {np.median(np.abs(errs)) if len(errs) else float("nan"):8.1f}  {100*np.mean(np.abs(errs)<=50) if len(errs) else 0:5.0f}%'
            f'   {corr:8.2f}  {dif:8.2f}   {min(cf) if cf else 0:5.0f}-{max(cf) if cf else 0:5.0f}')
def onset(t_s, thr=0.01):
    i0 = int(t_s * fs); return i0 + int(np.argmax(np.abs(d[i0:i0 + fs]) > thr))
HEAD = 'what        start  frames  pitch err (cents) median  <=50c   peak harm vs cutoff: corr  median |diff| (oct)  cutoff range (Hz)'

if len(sys.argv) > 2 and sys.argv[2] == 'live':      # tests/scripts/acid-cat-live.txt
    print(HEAD)
    s0 = onset(2.0) / fs                             # PLAY: step 0
    CUT7 = [85, 117, 127, 127, 114, 100, 89]; ST6 = [3, 7, 7, 5, 3, 0]
    def line(t):                                     # the MEOW line from step 8: 6 steps, slid, the lanes' CUTOFF
        j = int(t / STEP); ph = t / STEP - j
        if j >= 6: return None, None, False
        p = ROOT + 12 + ST6[j]
        if j: p += (ST6[j - 1] - ST6[j]) * math.exp(-ph * STEP / 0.022)
        v = CUT7[j] + (CUT7[j + 1] - CUT7[j]) * ph
        return p, 440 * 2 ** ((24 + v * 72 / 128 - 69) / 12), j < 5 or ph < 0.5
    for b in range(4):
        print(f'line bar {b + 1}{"*" if b == 3 else " "} {s0 + b * 2 + 1:6.2f} ' + measure(int((s0 + b * 2 + 1) * fs), line, 0.8))
    for t_key, name in ((9.03, 'MEOW'), (10.51, 'LONG')):
        # the call's own pitch (C5 and up) shows where it starts: the first frame from the key near 523 Hz
        hop = int(0.002 * fs); st = None
        for k in range(int(t_key * fs), int((t_key + 0.4) * fs), hop):
            f = f0_of(d[k:k + int(0.03 * fs)], fs, 400, 700)
            if f and f > 0 and abs(1200 * math.log2(f / 523.25)) < 60: st = k / fs; break
        grid = s0 + math.ceil((t_key - s0) / STEP) * STEP
        print(f'FX+{name} key {t_key:.3f} s: next 16th {grid:.3f} s, call heard from {st:.3f} s (frames: 30 ms windows)')
        print(f'{name:11s} {grid:6.2f} ' + measure(int(grid * fs), lambda t, n=name: design(n, 0, t), 2.2))
    print('* bar 4: FX + MEOW comes in on the line at 9.125 s (it takes ACID over), so this bar is not the line')
    sys.exit()
print(HEAD)
for t0, name, low in RUNS:
    on = onset(t0 / 1000)
    print(f"{('low ' if low else '') + name:11s} {(on/fs):6.2f} " + measure(on, lambda t: design(name, low, t)))
