"""把案例里的 Markdown 笔记渲染成简单的 HTML 页（公开版用）。只支持笔记里用到的语法：标题、表格、列表、代码块、粗体、行内代码、链接、分隔线。"""
import re
import html as H

def _inline(t):
    t = H.escape(t, quote=False)
    t = re.sub(r'`([^`]+)`', r'<code>\1</code>', t)
    t = re.sub(r'\*\*([^*]+)\*\*', r'<b>\1</b>', t)
    t = re.sub(r'\[([^\]]+)\]\(([^)]+)\)',
               lambda m: '<a href="%s">%s</a>' % (re.sub(r'\.md(#|$)', r'.html\1', m.group(2)), m.group(1)), t)
    return t

LIST = re.compile(r'\s*([-*]|\d+\.) ')

def md_to_html(src, title, extra_head='', css=None):
    """css: the page's style instead of the plain one below."""
    out, lines, i = [], src.splitlines(), 0
    while i < len(lines):
        l = lines[i]
        if l.startswith('```'):
            j, buf = i + 1, []
            while j < len(lines) and not lines[j].startswith('```'):
                buf.append(H.escape(lines[j])); j += 1
            out.append('<pre>' + '\n'.join(buf) + '</pre>'); i = j + 1; continue
        m = re.match(r'(#{1,4}) (.*)', l)
        if m:
            n = len(m.group(1)); out.append('<h%d>%s</h%d>' % (n, _inline(m.group(2)), n)); i += 1; continue
        if l.startswith('|'):
            rows = []
            while i < len(lines) and lines[i].startswith('|'):
                cells = [c.strip() for c in lines[i].strip().strip('|').split('|')]
                if not all(re.fullmatch(r':?-+:?', c) for c in cells if c):
                    rows.append(cells)
                i += 1
            t = '<table><tr>' + ''.join('<th>%s</th>' % _inline(c) for c in rows[0]) + '</tr>'
            t += ''.join('<tr>' + ''.join('<td>%s</td>' % _inline(c) for c in r) + '</tr>' for r in rows[1:]) + '</table>'
            out.append('<div class="tw">' + t + '</div>'); continue
        if LIST.match(l):
            tag = 'ol' if re.match(r'\s*\d+\.', l) else 'ul'; items = []
            while i < len(lines) and LIST.match(lines[i]):
                items.append('<li>' + _inline(LIST.sub('', lines[i], 1)) + '</li>'); i += 1
            out.append('<%s>%s</%s>' % (tag, ''.join(items), tag)); continue
        if l.strip() == '---':
            out.append('<hr>'); i += 1; continue
        if l.strip():
            out.append('<p>' + _inline(l) + '</p>')
        i += 1
    css = css or ('body{margin:0;background:#f5f2ec;color:#1d1c1a;font:15px/1.7 system-ui,"PingFang SC","Microsoft YaHei",sans-serif}'
           'main{max-width:960px;margin:0 auto;padding:24px 16px 60px}a{color:#2b47e6}'
           'table{border-collapse:collapse;font-size:14px;min-width:560px}td,th{border:1px solid #e2dcd1;padding:6px 8px;vertical-align:top;text-align:left}'
           'th{background:#fffdf9}.tw{overflow-x:auto}pre{background:#fffdf9;border:1px solid #e2dcd1;padding:10px;overflow-x:auto;font-size:12.5px;line-height:1.35}'
           'code{background:#efe9de;padding:0 3px;border-radius:3px}'
           '@media (prefers-color-scheme:dark){body{background:#17171a;color:#ecebe6}a{color:#8fa2ff}th,pre{background:#222}td,th,pre{border-color:#34343a}code{background:#2a2a30}}')
    return ('<!doctype html><html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">'
            '<title>%s</title><style>%s</style>%s</head><body><main>%s</main></body></html>'
            % (H.escape(title), css, extra_head, '\n'.join(out)))
