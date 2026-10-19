"""Function Acid: put the built package into a local web installer (SLOOP 2.7.1's installer, GPL-3.0, pointed at
our package). Chrome / Edge, Web MIDI: serve web/installer on localhost and press Install.
   python3 tools/make_installer.py   (after ./build.sh)"""
import hashlib, json, pathlib, re, shutil
ROOT = pathlib.Path(__file__).resolve().parent.parent
pkg = ROOT / 'build' / 'function-acid.fwsc'
out = ROOT / 'web' / 'installer'
src = (out / 'sloop-installer-2.7.1.orig.html').read_text(encoding='utf-8')
data = pkg.read_bytes()
shutil.copy2(pkg, out / 'function-acid.fwsc')
meta = {"version": "0.1 test", "product": "FM-1_956", "pkg": "function-acid.fwsc", "sha256": hashlib.sha256(data).hexdigest()}
s = re.sub(r'const meta = \{.*?\};', 'const meta = ' + json.dumps(meta) + ';', src, count=1)
s = s.replace('<title>SLOOP installer</title>', '<title>FN ACID installer (test)</title>')
s = s.replace('say(`SLOOP ${meta.version}`);', 'say(`FN ACID ${meta.version}  (FM-1_956)`);')
s = s.replace('navigator.serviceWorker.register("../sw.js").catch(() => {});', '/* no service worker: a local test page */')
s = s.replace('<!--PWA--><link rel="manifest" href="../sloop.webmanifest"><meta name="theme-color" content="#000000"><link rel="icon" href="../icon-192.png"><!--/PWA-->', '')
banner = ('<div style="background:#c4e45a;color:#1d1d1b;padding:12px 14px;border-radius:10px;margin:12px 0;font:15px/1.6 system-ui,sans-serif">'
          '<b>FN ACID 0.1 test</b> (Function Acid, built on Dinghy / SLOOP / Felucca, GPL-3.0). This page is SLOOP 2.7.1&#39;s installer '
          'pointed at the FN ACID package. Back up SLOOP first (SLOOP editor: Projects &rarr; Backup). To go back: '
          '<a href="https://isod89.github.io/sloop-fm1/webapp/installer/" target="_blank" rel="noopener">SLOOP&#39;s installer</a> or '
          '<a href="https://thefirmwarehub.com/" target="_blank" rel="noopener">the FM-1 Firmware Hub</a>. '
          'Stuck: hold OCT&minus; while switching on (USB rescue), then install again.</div>')
# SLOOP's own headline, blurb and tiles become ours; its editor links point to SLOOP's site (for the backup)
s = re.sub(r'<h1>Firmware for the M-VAVE FM-1</h1>.*?<div class="tiles">.*?</div>\s*</div>',
           lambda m: '<h1>FN ACID 0.1 test</h1>' + banner, s, count=1, flags=re.S)
s = s.replace('<a class="ed" href="../editor/" data-t="editor"></a>', '')
s = re.sub(r'<p class="small"><a href="\.\./editor/sloop-editor\.html"[^<]*</a></p>', '', s)
s = s.replace('href="../editor/"', 'href="https://isod89.github.io/sloop-fm1/webapp/editor/" target="_blank" rel="noopener"')
assert '"FM-1_956"' in s and 'FN ACID 0.1 test</h1>' in s
(out / 'index.html').write_text(s, encoding='utf-8')
print('installer ->', out / 'index.html', meta)
