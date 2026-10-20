"""Function Acid: the install site. Our own page (web/site/install.html, web/site/fn.css) around SLOOP 2.7.1's web
installer code (GPL-3.0, Felucca's): its <script> is taken from web/installer/sloop-installer-2.7.1.orig.html and kept
as it is (the Web MIDI update protocol, the package checks, the progress); only the package (meta), the words it
shows (TEXT, Chinese / English instead of Japanese / English) and the service worker line change.
   python3 tools/make_installer.py          (after ./build.sh)  -> web/installer/ (local test: :8814) and build/site/
   netlify deploy --prod --dir build/site --site f9e120e7-2d77-473a-a97a-cb1fe11a48a8 --no-build   -> fn-acid.netlify.app
build/site: index.html (install), function-acid.fwsc, guide.html (TEST-ZH.md), editor/ (web/editor, the offline editor).
The version comes from APP_VER in firmware/src/dinghy.c (steps of 0.01: 0.31, 0.32 ...); add a line to CHANGES below
for each new version (the page shows the newest first)."""
import base64, hashlib, html, json, pathlib, re, shutil, sys, urllib.parse
ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from md_page import md_to_html

PRODUCT = 'FM-1_956'
REPO = 'https://github.com/Electro-Dig/function-acid-fw'
# the changelog on the page: (version, date, [Chinese lines], [English lines]); newest first
CHANGES = [
    ('0.36', '2026-10-20',
     ['MIDI（USB）：按三下 GLO 进 MIDI 页。CLOCK：OUT = 发时钟和开始 / 停止，IN = 跟外部时钟走；NOTES OUT 打开后把音符发出去',
      '通道：ACID = ACID CH，BIO = 下一个通道，第 3 轨（909 用通用鼓号）和 PERC = 10 通道'],
     ['USB-MIDI: GLO three times = MIDI. CLOCK OUT sends clock and start / stop, IN follows an external clock; NOTES OUT sends the notes',
      'Channels: ACID on ACID CH, BIO on the next, track 3 (909 as GM drum notes) and PERC on 10']),
    ('0.35', '2026-10-20',
     ['909 鼓机（来自 X0X）：EDIT 选第 3 轨，KIT 选 909；再按 SEQ = 鼓的编辑页，11 个鼓一行一行排开',
      '鼓页：黑键选哪一行（并试听），白键是 16 步（点一下开，再点重音，再点关），KNOB1–4 = 这一行的音高 / 衰减 / 音量 / 另一项',
      '存档格改成 6 个（U1–U6）；0.33/0.34 的存档自动转过来（U7 舍去）'],
     ['909 drum machine (from X0X): EDIT on track 3, KIT = 909; then SEQ = the drum page, all 11 voices as rows',
      'Drum page: black keys pick a row (and play it), white keys are the 16 steps (on, accent, off), KNOB1-4 = the row tune / decay / level / one more',
      'Slots are now 6 (U1-U6); 0.33/0.34 saves are converted (U7 dropped)']),
    ('0.34', '2026-10-20',
     ['测试版：打开芯片的硬件浮点（FPU），GLO 页第 2 格显示 FPU OK / ERR，为 808/909 鼓做准备；其它和 0.33 一样'],
     ["Test: the chip's FPU on; GLO page cell 2 shows FPU OK / ERR (for the 808/909 drums); otherwise as 0.33"]),
    ('0.33', '2026-10-20',
     ['存档：按住 SAVE，点白键 1–7 = 读出 U1–U7，长按（0.7 秒）= 存进去',
      'ACID 最多 32 步：LENGTH 拧过 16，新的步先复制原来的线；再按一次 SEQ 翻到 17–32 步',
      '混音：按两下 GLO 进 MIX 页，四个旋钮 = ACID / BIO / KICK / PERC 的音量'],
     ['Slots: hold SAVE, tap white key 1-7 = load U1-U7, hold it 0.7 s = save',
      'ACID up to 32 steps: past 16 the new steps start as a copy; press SEQ again for steps 17-32',
      'Mixer: GLO twice = MIX, the four knobs = ACID / BIO / KICK / PERC levels']),
    ('0.32', '2026-10-19',
     ['滤波器换成 303 那种 4 层梯形（声音更圆、共振更集中）；原来的 2 层 SVF 在 EDIT › ACID › TYPE',
      '8 条预设旋律连音色一起载入：按住 HOME + 白键 9–16，或 STEPS 页拧 PRESETS',
      '录旋钮动作（X0X 的做法）：播放时按 REC，拧 CUTOFF / RESO / ENV / DECAY，录进 4 小节',
      '0.31 存过的状态会保留'],
     ['A 303-style 4-pole ladder filter (rounder, a more focused resonance); the 2-pole SVF stays on EDIT › ACID › TYPE',
      '8 preset lines, each with its sound: HOME held + white keys 9–16, or the PRESETS knob on STEPS',
      'Knob motion (as X0X records it): REC while playing, turn CUTOFF / RESO / ENV / DECAY, 4 bars',
      'A 0.31 save still loads']),
    ('0.31', '2026-10-19',
     ['到处只有一个版本号（安装页 = 开机画面）；网站上线 fn-acid.netlify.app',
      '安装页提醒：写入时让标签页留在前台'],
     ['One version number everywhere (this page = the start screen); the site on fn-acid.netlify.app',
      'The installer asks to keep its tab in front while it writes']),
    ('0.3', '2026-10-19',
     ['DECAY 改成 303 的范围：按步模式 200 毫秒到 2 秒，屏幕显示毫秒',
      '加了一点小混响（FX 按两下 = SPACE 页）',
      'HOME + 白键 10（FILM）连视频里的音色一起载入',
      '修了延时反馈过大时停不下来的问题'],
     ["DECAY over a 303's range on the steps: 200 ms to 2 s, shown in ms",
      'A small room after the delay (FX twice = SPACE)',
      "HOME + white key 10 (FILM) loads the film's sound too",
      'The delay no longer runs away at high FEEDBACK']),
    ('0.2', '2026-10-19',
     ['开机是一张空白页；ACID 改成按步写（音 / 连音 / 休止，重音、滑音），公式还在',
      '延时换成视频里的附点八分乒乓回声',
      'FX 层：按住 FX 再按住白键 = 效果一直在，松手恢复；10–14 直接对曲线做运算'],
     ["A blank start; ACID plays 16 steps you write (note / tie / rest, accent, slide), the formula one knob away",
      "The acid film's dotted-eighth ping-pong delay",
      'The FX layer: hold FX + a white key; keys 10–14 are operations on the curve (y + 12, mirror, x / 2, 2x)']),
    ('0.1', '2026-10-19',
     ['第一版：函数 Acid 的四轨引擎移植到 Dinghy 平台'],
     ["The first build: Function Acid's four-track engine on the Dinghy platform"]),
]

VER = re.search(r'#define APP_VER "([^"]+)"', (ROOT / 'firmware' / 'src' / 'dinghy.c').read_text(encoding='utf-8'))[1]
pkg = ROOT / 'build' / 'function-acid.fwsc'
out = ROOT / 'web' / 'installer'
web = ROOT / 'web' / 'site'
data = pkg.read_bytes()
sha = hashlib.sha256(data).hexdigest()
shutil.copy2(pkg, out / 'function-acid.fwsc')
meta = {"version": VER + " test", "product": PRODUCT, "pkg": "function-acid.fwsc", "sha256": sha}

# ---- SLOOP's installer script, as it is: only meta, the words and the service worker change
orig = (out / 'sloop-installer-2.7.1.orig.html').read_text(encoding='utf-8')
script = re.search(r'<script type="module">.*?</script>', orig, re.S)[0]
def sub1(pat, rep, s, flags=0):
    s2, n = re.subn(pat, rep, s, count=1, flags=flags)
    assert n == 1, 'installer script changed: ' + pat
    return s2
def rep1(old, new, s):
    assert s.count(old) == 1, 'installer script changed: ' + old
    return s.replace(old, new)

TEXT = {
  'zh': {
    'intro': '用 USB 把 FM-1 直接接到电脑上（Chrome 或 Edge）。', 'warn': '写入时不要拔 USB 线。',
    'install': f'安装 FN ACID {VER}', 'other': 'English',
    'editor': '编辑器', 'edOffline': '下载编辑器', 'appInstall': '作为应用安装（离线也能用安装页）',
    'direct': 'USB 线直接插电脑，不需要 Probe 或 Transporter。',
    'brick': '万一安装失败、机器起不来，先试 USB 救援模式（按住 OCT− 开机）；再不行需要 Transporter：',
    'beta': '这是测试版，可能不稳定。自制固件造成的损坏或问题，作者不承担责任。',
    'back': '也可以用 M-VAVE 自己的升级工具 M-UPGRADE（Downloads → PC Software）。找不到 FM-1 时，关掉其它用 MIDI 的软件和浏览器标签页。',
    'nomidi': '这个浏览器做不了：请用电脑上的 Chrome 或 Edge 打开本页。', 'loadfail': '读不到安装包：',
    'start': '开始', 'verify': '校验中', 'loader': '切换到升级模式', 'write': '写入中（标签页留在前台）',
    'reboot': '重启中', 'done': '完成。FM-1 现在是 FN ACID 了', 'stopped': '中途停了。重新插一下 USB 线，再按一次安装。',
    'error': '出错：',
    'denied': '没有得到 MIDI（SysEx）权限。在地址栏的网站设置里允许 MIDI，再按一次安装。',
    'notfound': '找不到 FM-1。检查 USB 线（要能传数据），关掉其它用 MIDI 的软件，再按一次。',
    'model': '接上的设备不是 FM-1。',
    'lost': 'USB 连接断了。重新插上再按一次。',
    'noloader': '没找到处在升级模式的 FM-1。重新插一下 USB 线再按一次（写完之前它一直停在升级模式）。',
    'noreturn': 'FM-1 没有回来：关机再开机。',
    'mismatch': '写完了，但 FM-1 报告的是另一个版本：',
    'foreign': 'FM-1 正处在别的固件的升级模式里。先用那个固件自己的工具（M-UPGRADE）写完，再来这里安装：',
    'badpkg': '要安装的文件坏了，请刷新页面。',
    'leave': '正在写入，别关这个页面。',
    'license': 'GPL-3.0，不提供任何担保。源代码：',
    'stockTitle': '回到官方固件（V15）',
    'stockHelp': '从 M-VAVE 网站下载官方 FM-1 V15，选里面没改过的 FM-1.fwsc。文件只在这个浏览器里校验，不会上传到任何地方。',
    'stockBackup': '官方固件读不了 SLOOP 的工程。要回 SLOOP 的话，先备份（SLOOP 编辑器 → Projects → Backup）。FN ACID 自己的状态只有一份设置，回官方后会丢。',
    'stockEditor': '打开编辑器备份', 'stockSaved': '我已经把需要的备份存到电脑上了',
    'stockFile': '官方的 FM-1.fwsc', 'stockGo': '安装官方 V15',
    'stockValid': '已确认是官方 V15', 'stockBad': '不是官方 V15：',
    'stockConfirm': '要给 FM-1 装官方 V15 吗？FN ACID 的状态和 SLOOP 的工程在官方固件里都读不了。备份存好了吗？',
    'stockFinish': 'FM-1 正停在 FN ACID / SLOOP 的升级模式里。先按“安装”把这次写完，再回官方 V15。',
  },
  'en': {
    'intro': 'Connect the FM-1 straight to the computer by USB (Chrome or Edge).', 'warn': 'Do not unplug the USB cable while writing.',
    'install': f'Install FN ACID {VER}', 'other': '中文',
    'editor': 'Editor', 'edOffline': 'Download the editor', 'appInstall': 'Install as an app (the installer offline too)',
    'direct': 'Connect the USB cable directly to the computer. No probe or Transporter is needed.',
    'brick': 'If an install fails and the FM-1 no longer starts, try the USB rescue first (hold OCT- at power-on); beyond that it needs a Transporter:',
    'beta': 'This is a test build and may be unstable. The authors accept no responsibility if your FM-1 is damaged or misbehaves.',
    'back': "M-VAVE's own updater, M-UPGRADE (Downloads → PC Software), works too. If it does not find the FM-1, close every other app and browser tab that uses MIDI.",
    'nomidi': 'This browser cannot do it. Open the page in Chrome or Edge on a computer.', 'loadfail': 'Could not load: ',
    'start': 'Starting', 'verify': 'Verifying', 'loader': 'Switching to update mode', 'write': 'Writing (keep this tab in front)',
    'reboot': 'Restarting', 'done': 'Done: the FM-1 runs FN ACID now', 'stopped': 'Stopped. Replug the USB cable and press Install again.',
    'error': 'Error: ',
    'denied': 'MIDI (SysEx) access was not allowed. Allow MIDI in the site settings (address bar) and press Install again.',
    'notfound': 'FM-1 not found. Check the USB cable (a data cable), close other apps that use MIDI, and press Install again.',
    'model': 'The connected device is not an FM-1.',
    'lost': 'The USB connection was lost. Replug it and press Install again.',
    'noloader': 'The FM-1 in update mode was not found. Replug the USB cable and press Install again (it stays in update mode until it is finished).',
    'noreturn': 'The FM-1 did not come back. Power-cycle it.',
    'mismatch': 'Written, but the FM-1 reports another version: ',
    'foreign': 'The FM-1 is in the update mode of another firmware. Finish that update with its own updater (M-UPGRADE), then press Install again: ',
    'badpkg': 'The file to install is damaged. Reload the page.',
    'leave': 'Writing. Do not close the page.',
    'license': 'GPL-3.0, no warranty. Source:',
    'stockTitle': 'Return to the official firmware (V15)',
    'stockHelp': 'Download the official FM-1 V15 from M-VAVE and select the FM-1.fwsc inside it, unmodified. The file is checked in this browser; it is not sent anywhere.',
    'stockBackup': "The official firmware cannot open SLOOP's projects: back them up first if you will go back to SLOOP (SLOOP editor → Projects → Backup). FN ACID keeps one settings record; it is lost on the official firmware.",
    'stockEditor': 'Open the editor to back up', 'stockSaved': 'I saved the backups I need on this computer',
    'stockFile': 'The official FM-1.fwsc', 'stockGo': 'Install the official V15',
    'stockValid': 'Official V15 verified', 'stockBad': 'Not the official V15: ',
    'stockConfirm': "Install the official V15 on the FM-1? Neither FN ACID's state nor SLOOP's projects open there. Did you save your backups?",
    'stockFinish': "The FM-1 is in FN ACID / SLOOP's update mode. Press Install to finish that first, then return to the official V15.",
  },
}
old_keys = set(re.findall(r'^\s{4}(\w+):', re.search(r'const TEXT = \{.*?\n\};', script, re.S)[0], re.M))
assert old_keys <= set(TEXT['zh']) and old_keys <= set(TEXT['en']), 'words missing: ' + str(old_keys - set(TEXT['zh']))
s = sub1(r'const TEXT = \{.*?\n\};', lambda m: 'const TEXT = ' + json.dumps(TEXT, ensure_ascii=False, indent=1) + ';', script, re.S)
s = rep1('let lang = (navigator.language || "en").toLowerCase().startsWith("ja") ? "ja" : "en";',
         'let lang = (navigator.language || "zh").toLowerCase().startsWith("en") ? "en" : "zh";', s)
s = rep1('lang = lang === "ja" ? "en" : "ja";', 'lang = lang === "zh" ? "en" : "zh";', s)
s = sub1(r'const meta = \{.*?\};', lambda m: 'const meta = ' + json.dumps(meta) + ';', s)
s = rep1('say(`SLOOP ${meta.version}`);', 'say(`FN ACID ${meta.version} · ${meta.product} · ready`);', s)
s = rep1('navigator.serviceWorker.register("../sw.js").catch(() => {});', '{ /* FN ACID: no service worker */ }', s)
# (the rest of the script is SLOOP's, byte for byte)

# ---- our page
def logo(size=28):
    return (f'<svg viewBox="0 0 32 32" width="{size}" height="{size}" aria-hidden="true"><rect x="1" y="1" width="30" height="30" rx="6" '
            'fill="#c4e45a" stroke="#1d1d1b" stroke-width="2"/><path d="M6 25V9c1.6 8 3.6 12.6 6.5 14.5V12c1.6 6.2 3.4 9.8 6.5 11.2V15'
            'c1.5 4 3.3 6.6 7 7.6" fill="none" stroke="#1d1d1b" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"/>'
            '<circle cx="26" cy="9" r="3" fill="#d8204a"/></svg>')
def changes():
    rows = []
    for v, d, zh, en in CHANGES:
        li = lambda xs: ''.join(f'<li>{html.escape(x)}</li>' for x in xs)
        now = ' class="now"' if v == VER else ''
        rows.append(f'        <li{now}><div class="v">{v}<small>{d}</small></div><div>'
                    f'<ul data-l="zh">{li(zh)}</ul><ul data-l="en">{li(en)}</ul></div></li>')
    return '\n'.join(rows)
if VER not in [c[0] for c in CHANGES]:
    print(f'warning: no CHANGES line for {VER} in tools/make_installer.py')
css = (web / 'fn.css').read_text(encoding='utf-8')
b64 = lambda p: base64.b64encode((web / 'img' / p).read_bytes()).decode()
fill = {'VER': VER, 'CSS': css, 'LOGO': logo(), 'ICON': urllib.parse.quote(logo(64)), 'CHANGES': changes(),
        'IMG_HOME': b64('screen-home.png'), 'IMG_STEPS': b64('screen-steps.png'), 'SHA': sha, 'SIZE': f'{len(data):,}',
        'SCRIPT': s}
page = re.sub(r'\{\{(\w+)\}\}', lambda m: fill[m[1]], (web / 'install.html').read_text(encoding='utf-8'))
# what the installer code needs on the page
for i in ('lang', 'go', 'bar', 'status', 'log', 'app', 'stock-saved', 'stock-file', 'stock-go'):
    assert f'id="{i}"' in page, 'the page lacks #' + i
assert f'"product": "{PRODUCT}"' in page and f'"sha256": "{sha}"' in page and '{{' not in page
assert script.split('const BLOCKS')[1].split('const $ =')[0] in page, 'the update code must stay as SLOOP wrote it'
(out / 'index.html').write_text(page, encoding='utf-8', newline='\n')

# ---- the site
site = ROOT / 'build' / 'site'
shutil.rmtree(site, ignore_errors=True)
site.mkdir(parents=True)
shutil.copy2(out / 'index.html', site / 'index.html')
shutil.copy2(out / 'function-acid.fwsc', site / 'function-acid.fwsc')
shutil.copytree(ROOT / 'web' / 'editor', site / 'editor')

GUIDE_CSS = css + '''
main{max-width:860px}
.doc{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:8px 22px 26px}
@media (max-width:600px){.doc{padding:4px 14px 20px;border-radius:10px}}
.doc h1{font:800 26px/1.3 var(--sans);margin:18px 0 6px}
.doc h2{font-size:19px;margin:30px 0 10px;padding-top:14px;border-top:1px solid var(--line);display:block}
.doc h2::before{content:"";display:inline-block;width:10px;height:10px;background:var(--lime);border:2px solid var(--lcdink);border-radius:2px;margin-right:9px;vertical-align:1px}
.doc p,.doc li{font-size:15.5px}
.doc li{margin:0 0 6px}
.doc b{background:linear-gradient(transparent 64%,color-mix(in srgb,var(--lime) 70%,transparent) 64%)}
.doc .tw{overflow-x:auto;margin:10px 0 16px;border:1px solid var(--line);border-radius:10px}
.doc table{border-collapse:collapse;width:100%;min-width:520px;font-size:14px}
.doc th{background:var(--lcd);color:var(--lcdink);text-align:left;font:700 13px/1.4 var(--mono)}
.doc td,.doc th{border-bottom:1px solid var(--line);padding:7px 9px;vertical-align:top}
.doc tr:last-child td{border-bottom:0}
.doc td:first-child{font-family:var(--mono);font-weight:600;white-space:nowrap}
.doc pre{background:var(--lcd);color:var(--lcdink);border-radius:8px;padding:10px;overflow-x:auto;font-size:13px}
.doc hr{border:0;border-top:1px solid var(--line)}
'''
nav = ('<header class="bar"><div class="in"><a class="brand" href="./">' + logo() + '<span>FN ACID<br><small>函数 Acid · FM-1</small></span></a>'
       '<nav class="nav"><a href="./">安装</a><a class="on" href="guide.html">上机说明</a><a href="editor/">编辑器原型</a>'
       f'<a href="{REPO}" target="_blank" rel="noopener">GitHub</a></nav></div></header>')
head = (f'<link rel="icon" href="data:image/svg+xml,{urllib.parse.quote(logo(64))}"><meta name="theme-color" content="#c4e45a">')
guide = md_to_html((ROOT / 'TEST-ZH.md').read_text(encoding='utf-8'), f'FN ACID {VER} 上机说明', head, css=GUIDE_CSS)
guide = guide.replace('<body><main>', '<body>' + nav + '<main><div class="doc">', 1).replace('</main></body>', '</div></main></body>', 1)
assert nav in guide and '</div></main>' in guide
(site / 'guide.html').write_text(guide, encoding='utf-8', newline='\n')
print('installer ->', out / 'index.html', meta)
print('site      ->', site, sorted(p.relative_to(site).as_posix() for p in site.rglob('*') if p.is_file()))
