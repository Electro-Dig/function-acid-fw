"""Function Acid: put the built package into a web installer (SLOOP 2.7.1's installer, GPL-3.0, pointed at our
package), and the site around it (the installer, the package, the Chinese guide) for Netlify.
   python3 tools/make_installer.py          (after ./build.sh)  -> web/installer/ (local test: :8814) and build/site/
   netlify deploy --prod --dir build/site --site f9e120e7-2d77-473a-a97a-cb1fe11a48a8 --no-build   -> fn-acid.netlify.app
The version comes from APP_VER in firmware/src/dinghy.c (steps of 0.01: 0.31, 0.32 ...)."""
import hashlib, json, pathlib, re, shutil, sys
ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
from md_page import md_to_html

VER = re.search(r'#define APP_VER "([^"]+)"', (ROOT / 'firmware' / 'src' / 'dinghy.c').read_text(encoding='utf-8'))[1]
pkg = ROOT / 'build' / 'function-acid.fwsc'
out = ROOT / 'web' / 'installer'
src = (out / 'sloop-installer-2.7.1.orig.html').read_text(encoding='utf-8')
data = pkg.read_bytes()
shutil.copy2(pkg, out / 'function-acid.fwsc')
meta = {"version": VER + " test", "product": "FM-1_956", "pkg": "function-acid.fwsc", "sha256": hashlib.sha256(data).hexdigest()}
s = re.sub(r'const meta = \{.*?\};', 'const meta = ' + json.dumps(meta) + ';', src, count=1)
s = s.replace('<title>SLOOP installer</title>', f'<title>FN ACID {VER} installer (test)</title>'
              '<style>#nl-badge-frame{display:none!important}</style>')
s = s.replace('say(`SLOOP ${meta.version}`);', 'say(`FN ACID ${meta.version}  (FM-1_956)`);')
s = s.replace('navigator.serviceWorker.register("../sw.js").catch(() => {});', '/* no service worker */')
s = s.replace('<!--PWA--><link rel="manifest" href="../sloop.webmanifest"><meta name="theme-color" content="#000000"><link rel="icon" href="../icon-192.png"><!--/PWA-->', '')
banner = ('<div style="background:#c4e45a;color:#1d1d1b;padding:12px 14px;border-radius:10px;margin:12px 0;font:15px/1.6 system-ui,sans-serif">'
          f'<b>FN ACID {VER} test</b> (Function Acid, built on Dinghy / SLOOP / Felucca, GPL-3.0). This page is SLOOP 2.7.1&#39;s installer '
          'pointed at the FN ACID package. Back up SLOOP first (SLOOP editor: Projects &rarr; Backup). To go back: '
          '<a href="https://isod89.github.io/sloop-fm1/webapp/installer/" target="_blank" rel="noopener">SLOOP&#39;s installer</a> or '
          '<a href="https://thefirmwarehub.com/" target="_blank" rel="noopener">the FM-1 Firmware Hub</a>. '
          'Stuck: hold OCT&minus; while switching on (USB rescue), then install again.<br>'
          '<b><a href="guide.html">中文说明：怎么刷、怎么玩</a></b> &middot; '
          '<a href="https://github.com/Electro-Dig/function-acid-fw" target="_blank" rel="noopener">source (GitHub)</a><br>'
          '<small>Keep this tab in front while it writes: a tab in the background is slowed down by the browser.</small></div>')
s = re.sub(r'<h1>Firmware for the M-VAVE FM-1</h1>.*?<div class="tiles">.*?</div>\s*</div>',
           lambda m: f'<h1>FN ACID {VER} test</h1>' + banner, s, count=1, flags=re.S)
s = s.replace('<a class="ed" href="../editor/" data-t="editor"></a>', '')
s = re.sub(r'<p class="small"><a href="\.\./editor/sloop-editor\.html"[^<]*</a></p>', '', s)
s = s.replace('href="../editor/"', 'href="https://isod89.github.io/sloop-fm1/webapp/editor/" target="_blank" rel="noopener"')
assert '"FM-1_956"' in s and f'FN ACID {VER} test</h1>' in s
(out / 'index.html').write_text(s, encoding='utf-8')

site = ROOT / 'build' / 'site'
shutil.rmtree(site, ignore_errors=True)
site.mkdir(parents=True)
shutil.copy2(out / 'index.html', site / 'index.html')
shutil.copy2(out / 'function-acid.fwsc', site / 'function-acid.fwsc')
css = ('<style>body{max-width:760px;margin:0 auto;padding:16px;font:16px/1.7 system-ui,"PingFang SC","Microsoft YaHei",sans-serif;'
       'background:#ceea70;color:#1d1d1b}table{border-collapse:collapse;width:100%;font-size:14px}td,th{border:1px solid #1d1d1b55;'
       'padding:4px 6px;vertical-align:top}code{background:#1d1d1b14;padding:0 3px}a{color:#b0103a}#nl-badge-frame{display:none!important}'
       '.top{display:inline-block;margin-bottom:8px}</style>')
guide = md_to_html((ROOT / 'TEST-ZH.md').read_text(encoding='utf-8'), f'FN ACID {VER} 上机说明', css)
guide = guide.replace('<body>', '<body><a class="top" href="./">&larr; 安装页</a>', 1)
(site / 'guide.html').write_text(guide, encoding='utf-8')
print('installer ->', out / 'index.html', meta)
print('site      ->', site)
