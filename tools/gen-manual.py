#!/usr/bin/env python3
"""Generate an HTML reference for every intent kind, in the shape of the
PostgreSQL manual's per-command pages.

    python3 tools/gen-manual.py [outdir]     # default: docs/

Everything on every page is READ OUT OF THE SOURCE. The kinds come from
intent_kinds(), the options from the reject_unknown_keys set the parser actually
enforces, the required ones from its require_* calls, the enumerated values and
their explanations from the fail() messages an author would see, and the
examples from conformance.inc -- which means every example on every page is
executed against a real PostgreSQL on every test run.

That is the same rule cpp/test/coverage/audit.py follows, and for the same
reason: a hand-written reference is a second description of the code that
starts drifting the day it is written. This one cannot drift; it can only be
out of date, and re-running it fixes that.
"""
import html
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import manual_extract as X

ROOT = X.ROOT
E = html.escape

CSS = """
:root{--fg:#333;--bg:#fff;--muted:#666;--rule:#ddd;--link:#0066a4;
      --code-bg:#f7f7f7;--code-rule:#e0e0e0;--req:#a33;--accent:#0e6ba8;
      --dd:#444;--req-bg:#fbeaea;--req-rule:#f0cccc;--opt-bg:#eef4f8;
      --opt-rule:#d3e3ee;--note-bg:#f6f9fb;--warn-bg:#fdf7f1;--warn-rule:#c8853a}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);
     font:15px/1.6 "Helvetica Neue",Helvetica,Arial,sans-serif}
.wrap{max-width:52rem;margin:0 auto;padding:1.5rem 1.25rem 5rem}
a{color:var(--link);text-decoration:none}
a:hover{text-decoration:underline}
a:focus-visible{outline:2px solid var(--accent);outline-offset:2px;border-radius:2px}
nav.crumb{display:flex;justify-content:space-between;gap:1rem;
          border-bottom:1px solid var(--rule);padding-bottom:.6rem;
          margin-bottom:1.5rem;font-size:.85rem;color:var(--muted)}
nav.crumb .l,nav.crumb .r{display:flex;gap:1rem;flex-wrap:wrap}
h1{font-size:1.9rem;margin:.2rem 0 .1rem;font-weight:600;letter-spacing:-.01em}
h1 code{font-size:inherit;background:none;padding:0;border:0}
.purpose{color:var(--muted);margin:0 0 1.8rem;font-size:1.02rem}
h2{font-size:1.15rem;margin:2.2rem 0 .7rem;padding-bottom:.3rem;
   border-bottom:1px solid var(--rule);font-weight:600}
pre{background:var(--code-bg);border:1px solid var(--code-rule);border-radius:3px;
    padding:.85rem 1rem;overflow-x:auto;font-size:.86rem;line-height:1.5;margin:0 0 1rem}
code,pre{font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}
p code,li code,dd code,dt code,td code{background:var(--code-bg);
    border:1px solid var(--code-rule);border-radius:3px;padding:.05rem .3rem;font-size:.86em}
dl.params{margin:0}
dl.params>dt{margin:1.1rem 0 .35rem;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
             font-size:.92rem;font-weight:600}
dl.params>dd{margin:0 0 0 1.6rem;color:var(--dd)}
.tag{display:inline-block;margin-left:.5rem;padding:.02rem .4rem;border-radius:2px;
     font:600 .68rem/1.5 "Helvetica Neue",Helvetica,Arial,sans-serif;
     text-transform:uppercase;letter-spacing:.04em;vertical-align:.08em}
.tag.req{background:var(--req-bg);color:var(--req);border:1px solid var(--req-rule)}
.tag.opt{background:var(--opt-bg);color:var(--accent);border:1px solid var(--opt-rule)}
.note{background:var(--note-bg);border-left:3px solid var(--accent);padding:.7rem .9rem;
      margin:0 0 1rem;font-size:.92rem}
.note.warn{background:var(--warn-bg);border-left-color:var(--warn-rule)}
table{border-collapse:collapse;width:100%;font-size:.9rem;margin:0 0 1rem}
th,td{text-align:left;padding:.45rem .6rem;border-bottom:1px solid var(--rule);
      vertical-align:top;font-variant-numeric:tabular-nums}
th{font-weight:600;color:var(--muted);font-size:.78rem;text-transform:uppercase;letter-spacing:.04em}
td.k{white-space:nowrap;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.86rem}
.idx{columns:2;column-gap:2.5rem}
@media(max-width:640px){.idx{columns:1}}
.idx section{break-inside:avoid;margin:0 0 1.4rem}
.idx h3{font-size:.78rem;text-transform:uppercase;letter-spacing:.05em;color:var(--muted);
        margin:0 0 .4rem;font-weight:600}
.idx ul{list-style:none;margin:0;padding:0}
.idx li{margin:.16rem 0;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;font-size:.86rem}
footer{margin-top:3rem;padding-top:1rem;border-top:1px solid var(--rule);
       color:var(--muted);font-size:.82rem}
/* Three states, not two: an explicit choice stamps data-theme on the root, and
   the default "system" setting stamps nothing at all -- so the media query has
   to carry the un-stamped case while losing to an explicit light choice, and
   the [data-theme="dark"] block has to win in the other direction. */
@media(prefers-color-scheme:dark){
 :root:not([data-theme="light"]){
  --fg:#d6d6d6;--bg:#161719;--muted:#9a9a9a;--rule:#333;--link:#63b3e8;
  --code-bg:#1e2023;--code-rule:#2e3136;--req:#e8918f;--accent:#63b3e8;
  --dd:#c2c2c2;--req-bg:#3a2526;--req-rule:#573537;--opt-bg:#1d2a33;
  --opt-rule:#2b3f4d;--note-bg:#1a2126;--warn-bg:#2a2119}
}
:root[data-theme="dark"]{
 --fg:#d6d6d6;--bg:#161719;--muted:#9a9a9a;--rule:#333;--link:#63b3e8;
 --code-bg:#1e2023;--code-rule:#2e3136;--req:#e8918f;--accent:#63b3e8;
 --dd:#c2c2c2;--req-bg:#3a2526;--req-rule:#573537;--opt-bg:#1d2a33;
 --opt-rule:#2b3f4d;--note-bg:#1a2126;--warn-bg:#2a2119}
"""

# Which family a kind belongs to, for the index and for "See also". Derived from
# the name, because the names were chosen to be read this way.
FAMILIES = [
    ('Tables',        r'^(create_table|drop_table|rename_table|create_table_as)$'),
    ('Columns',       r'column|_not_null$|^set_identity$|^drop_expression$'),
    ('Rows',          r'^(insert_rows|update_rows|delete_rows|merge_rows|copy_rows|backfill)$'),
    ('Indexes',       r'index'),
    ('Constraints',   r'constraint|^add_foreign_key$|^add_primary_key$|^add_unique_constraint$'),
    ('Partitions',    r'partition'),
    ('Views',         r'view'),
    ('Row security',  r'polic|row_security'),
    ('Privileges',    r'^(grant|revoke|alter_default_privileges|set_owner|security_label)$'),
    ('Routines',      r'function|trigger|^create_rule$|^drop_rule$'),
    ('Types',         r'type|enum|domain'),
    ('Schemas',       r'schema'),
    ('Sequences',     r'sequence'),
    ('Extensions',    r'extension'),
    ('Replication',   r'publication|subscription|foreign'),
    ('Physical',      r'^(set_logged|set_tablespace|set_access_method|set_replica_identity|cluster_on|set_table_options|set_column_options)$'),
    ('Statistics',    r'statistics'),
    ('Other objects', r'object'),
]


def family(kind):
    for name, pat in FAMILIES:
        if re.search(pat, kind):
            return name
    return 'Other'


def leading_comment(text, signature):
    """The comment block immediately above a definition, as prose."""
    i = text.find(signature)
    if i < 0:
        return ''
    lines, j = [], text.rfind('\n', 0, i)
    while j > 0:
        start = text.rfind('\n', 0, j) + 1
        line = text[start:j]
        if line.strip().startswith('//'):
            lines.append(line.strip()[2:].strip())
            j = start - 1
        else:
            break
    if not lines:
        return ''
    lines.reverse()
    paras, cur = [], []
    for l in lines:
        if not l:
            if cur:
                paras.append(' '.join(cur)); cur = []
        else:
            cur.append(l)
    if cur:
        paras.append(' '.join(cur))
    return paras


def section_prose(text):
    """kind -> prose, from the `// --- name ---` dividers the planner uses.

    A divider may name several kinds that share a rule ("create_table /
    drop_table"), and the paragraphs under it describe all of them. This is
    where most of the real explanation lives, because it is written once for the
    family rather than repeated per function.
    """
    out = {}
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        m = re.match(r'^// --- ([a-z_][a-z_ /]*?) -+\s*$', lines[i])
        if not m:
            i += 1
            continue
        names = [n.strip() for n in m.group(1).split('/') if n.strip()]
        body, j = [], i + 1
        while j < len(lines) and lines[j].startswith('//'):
            body.append(lines[j][2:].strip())
            j += 1
        paras, cur = [], []
        for l in body:
            if not l:
                if cur:
                    paras.append(' '.join(cur)); cur = []
            else:
                cur.append(l)
        if cur:
            paras.append(' '.join(cur))
        for n in names:
            if paras:
                out.setdefault(n, paras)
        i = j
    return out


def describe(kind, spec, planner, planner_dml, sections):
    """Prose for a kind, from whichever source actually has it.

    The planner's own comment first -- it explains what the kind DECIDES, which
    is the thing worth knowing. Then the family divider it sits under, then the
    parser's comment, which tends to be about validation rather than intent.
    """
    for text in (planner_dml, planner):
        p = leading_comment(text, 'inline void plan_%s(' % kind)
        if p:
            return p
    if kind in sections:
        return sections[kind]
    p = leading_comment(spec, 'inline void parse_%s(' % kind)
    return p or []


def md(text):
    """The little markup the comments use: `code` and -- as an em dash."""
    text = E(text).replace('--', '—')
    return re.sub(r'`([^`]+)`', r'<code>\1</code>', text)


def synopsis(d):
    """The JSON shape, required keys first, in the order the parser lists them."""
    req = [k for k in d['accepted'] if k in d['required'] and k != 'kind']
    opt = [k for k in d['accepted'] if k not in d['required'] and k != 'kind']
    lines = ['{']
    lines.append('  "kind": "%s",' % d['kind'])
    for k in req:
        lines.append('  "%s": <required>,' % k)
    for k in opt:
        vals = d['enums'].get(k)
        v = ' | '.join('"%s"' % x for x in vals) if vals else '<optional>'
        lines.append('  "%s": %s,' % (k, v))
    if len(lines) > 1:
        lines[-1] = lines[-1].rstrip(',')
    lines.append('}')
    return '\n'.join(lines)


def cxx_json_to_text(raw):
    """The conformance body, as the JSON an author would actually write.

    A real parse rather than a chain of regexes, because the C++ initialiser has
    all three things regexes get wrong here: commas inside string literals (a
    view definition is "SELECT id, amount FROM t"), nested objects (backfill's
    `set`, create_table's `columns`), and ordinary // comments sitting between
    entries. The output is fed through json.loads by a test, so a mistake here
    is caught rather than published.
    """
    val, _ = _cxx_value(_strip_comments(raw), 0)
    return json.dumps(val, indent=1, ensure_ascii=False)


def _strip_comments(text):
    out, i = '', 0
    while i < len(text):
        if text[i] == '"':
            j = i + 1
            while j < len(text):
                if text[j] == '\\':
                    j += 2
                    continue
                if text[j] == '"':
                    break
                j += 1
            out += text[i:j + 1]
            i = j + 1
            continue
        if text.startswith('//', i):
            i = text.find('\n', i)
            if i < 0:
                break
            continue
        out += text[i]
        i += 1
    return out


def _ws(t, i):
    while i < len(t) and t[i] in ' \t\r\n':
        i += 1
    return i


def _cxx_string(t, i):
    """One string literal, plus any adjacent literals concatenated to it."""
    parts = []
    while True:
        i = _ws(t, i)
        if i >= len(t) or t[i] != '"':
            break
        j, buf = i + 1, ''
        while j < len(t):
            if t[j] == '\\':
                buf += {'n': '\n', 't': '\t', '"': '"', '\\': '\\'}.get(t[j + 1], t[j + 1])
                j += 2
                continue
            if t[j] == '"':
                break
            buf += t[j]
            j += 1
        parts.append(buf)
        i = j + 1
    return ''.join(parts), i


def _cxx_value(t, i):
    i = _ws(t, i)
    if t.startswith('json::array()', i):
        return [], i + len('json::array()')
    if t.startswith('json::array({', i):
        i += len('json::array({')
        out = []
        while True:
            i = _ws(t, i)
            if i < len(t) and t[i] == '}':
                break
            v, i = _cxx_value(t, i)
            out.append(v)
            i = _ws(t, i)
            if i < len(t) and t[i] == ',':
                i += 1
        i = _ws(t, i)
        if t.startswith('})', i):
            i += 2
        return out, i
    if t.startswith('json', i) and _ws(t, i + 4) < len(t) and t[_ws(t, i + 4)] == '{':
        i = _ws(t, i + 4)
    if i < len(t) and t[i] == '{':
        # Either an object written as {{"k", v}, ...} or a single pair {"k", v}.
        save = i
        i = _ws(t, i + 1)
        if i < len(t) and t[i] == '"':
            key, i = _cxx_string(t, i)
            i = _ws(t, i)
            if i < len(t) and t[i] == ',':
                v, i = _cxx_value(t, i + 1)
                i = _ws(t, i)
                if i < len(t) and t[i] == '}':
                    i += 1
                return {key: v}, i
            i = save
        out, i = {}, _ws(t, save + 1)
        while i < len(t) and t[i] != '}':
            pair, i = _cxx_value(t, i)
            if isinstance(pair, dict):
                out.update(pair)
            i = _ws(t, i)
            if i < len(t) and t[i] == ',':
                i = _ws(t, i + 1)
        return out, i + 1
    if i < len(t) and t[i] == '"':
        return _cxx_string(t, i)
    m = re.match(r'(-?\d+\.?\d*)', t[i:])
    if m:
        txt = m.group(1)
        return (float(txt) if '.' in txt else int(txt)), i + len(txt)
    if t.startswith('true', i):
        return True, i + 4
    if t.startswith('false', i):
        return False, i + 5
    if t.startswith('nullptr', i):
        return None, i + 7
    return None, i + 1


def page(d, all_kinds, prose, prev_k, next_k):
    k = d['kind']
    fam = family(k)
    req = [x for x in d['accepted'] if x in d['required'] and x != 'kind']
    opt = [x for x in d['accepted'] if x not in d['required'] and x != 'kind']

    purpose = ''
    if prose:
        purpose = prose[0]
        if len(purpose) > 190:
            cut = purpose[:190].rsplit('. ', 1)[0]
            purpose = (cut + '.') if cut else purpose[:190] + '…'

    h = ['<!doctype html><html lang="en"><head><meta charset="utf-8">',
         '<meta name="viewport" content="width=device-width,initial-scale=1">',
         '<title>%s — pg_laswell</title>' % E(k),
         '<style>%s</style></head><body><div class="wrap">' % CSS]

    h.append('<nav class="crumb"><div class="l">'
             '<a href="index.html">Intent kinds</a><span>%s</span></div>'
             '<div class="r">%s%s</div></nav>' % (
                 E(fam),
                 '<a href="%s.html">← %s</a>' % (prev_k, prev_k) if prev_k else '',
                 '<a href="%s.html">%s →</a>' % (next_k, next_k) if next_k else ''))

    h.append('<h1><code>%s</code></h1>' % E(k))
    if purpose:
        h.append('<p class="purpose">%s</p>' % md(purpose))

    h.append('<h2>Synopsis</h2><pre>%s</pre>' % E(synopsis(d)))

    if prose:
        h.append('<h2>Description</h2>')
        for p in prose:
            h.append('<p>%s</p>' % md(p))

    h.append('<h2>Parameters</h2><dl class="params">')
    for name in req + opt:
        tag = ('<span class="tag req">required</span>' if name in req
               else '<span class="tag opt">optional</span>')
        h.append('<dt>%s%s</dt>' % (E(name), tag))
        vals = d['enums'].get(name)
        hint = d.get('hints', {}).get(name)
        parts = []
        if vals:
            parts.append('One of %s.' %
                         ', '.join('<code>%s</code>' % E(v) for v in vals))
        if hint:
            parts.append(md(hint))
        h.append('<dd>%s</dd>' % (' '.join(parts) if parts else '&nbsp;'))
    h.append('</dl>')
    h.append('<div class="note">Any key not listed here is <strong>refused</strong>, '
             'not ignored: a key this binary skips and a newer one honours would be a '
             'silent difference between what was reviewed and what ran.</div>')

    h.append('<h2>Examples</h2>')
    if d['example']:
        h.append('<p>From the conformance suite, which plans this intent and '
                 '<em>applies</em> it to a real PostgreSQL on every test run:</p>')
        h.append('<pre>%s</pre>' % E(cxx_json_to_text(d['example']['raw'])))
        if d['example']['verify']:
            h.append('<p>Afterwards, <code>%s</code> returns <code>%s</code>.</p>' % (
                E(d['example']['verify']), E(d['example']['expect'] or '')))
    elif d['deferred']:
        h.append('<div class="note warn">No single-database example: %s. '
                 'This kind is covered by <code>cpp/test/replication-tests.sh</code>, '
                 'which builds two clusters.</div>' % md(d['deferred']))
    else:
        h.append('<p class="purpose">No worked example yet.</p>')

    siblings = [x for x in all_kinds if family(x) == fam and x != k]
    if siblings:
        h.append('<h2>See also</h2><p>%s</p>' % ', '.join(
            '<a href="%s.html"><code>%s</code></a>' % (s, E(s)) for s in siblings))

    h.append('<footer>Generated from <code>cpp/src/spec.h</code> and '
             '<code>cpp/src/conformance.inc</code> by <code>tools/gen-manual.py</code>. '
             'The manual page <code>pg_laswell_mcp(1)</code> remains the authoritative '
             'reference for everything that is not a per-kind option.</footer>')
    h.append('</div></body></html>')
    return '\n'.join(h)


def index_page(data):
    by_fam = {}
    for d in data:
        by_fam.setdefault(family(d['kind']), []).append(d['kind'])
    order = [n for n, _ in FAMILIES if n in by_fam] + \
            [f for f in sorted(by_fam) if f not in [n for n, _ in FAMILIES]]

    h = ['<!doctype html><html lang="en"><head><meta charset="utf-8">',
         '<meta name="viewport" content="width=device-width,initial-scale=1">',
         '<title>pg_laswell Intent Kinds</title>',
         '<style>%s</style></head><body><div class="wrap">' % CSS]
    h.append('<h1>Intent kinds</h1>')
    h.append('<p class="purpose">The %d change intents a pg_laswell specification '
             'may contain. The set a binary knows is exactly the set it implements: '
             'an unknown kind refuses the whole specification rather than skipping a '
             'step.</p>' % len(data))
    h.append('<div class="idx">')
    for fam in order:
        h.append('<section><h3>%s</h3><ul>' % E(fam))
        for k in sorted(by_fam[fam]):
            h.append('<li><a href="%s.html">%s</a></li>' % (k, E(k)))
        h.append('</ul></section>')
    h.append('</div>')

    h.append('<h2>Coverage at a glance</h2><table><tr><th>Kind</th><th>Required</th>'
             '<th>Optional</th><th>Example</th></tr>')
    for d in sorted(data, key=lambda x: x['kind']):
        req = [x for x in d['accepted'] if x in d['required'] and x != 'kind']
        opt = [x for x in d['accepted'] if x not in d['required'] and x != 'kind']
        ex = 'tested' if d['example'] else ('two clusters' if d['deferred'] else '—')
        h.append('<tr><td class="k"><a href="%s.html">%s</a></td><td>%d</td>'
                 '<td>%d</td><td>%s</td></tr>' % (d['kind'], E(d['kind']),
                                                  len(req), len(opt), ex))
    h.append('</table>')
    h.append('<footer>Generated by <code>tools/gen-manual.py</code>, which reads the '
             'kinds, their options and their examples out of the source rather than '
             'from a list kept by hand.</footer>')
    h.append('</div></body></html>')
    return '\n'.join(h)


def single_file(data, spec, planner, planner_dml, sections):
    """The whole reference as one page, the way PostgreSQL also ships a
    single-page HTML manual beside the per-command one. Cross-references become
    anchors, so the file stands alone."""
    names = [d['kind'] for d in data]
    body = []
    for i, d in enumerate(data):
        prose = describe(d['kind'], spec, planner, planner_dml, sections)
        p = page(d, names, prose, None, None)
        inner = p[p.index('<h1>'):p.index('<footer>')]
        inner = inner.replace('href="index.html"', 'href="#top"')
        inner = re.sub(r'href="([a-z_0-9]+)\.html"', r'href="#\1"', inner)
        body.append('<section id="%s" class="kindpage">%s</section>' % (d['kind'], inner))

    idx = index_page(data)
    idx_inner = idx[idx.index('<h1>'):idx.index('<footer>')]
    idx_inner = re.sub(r'href="([a-z_0-9]+)\.html"', r'href="#\1"', idx_inner)

    return ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1">'
            '<title>pg_laswell Intent Kinds</title><style>%s\n'
            '.kindpage{border-top:2px solid var(--rule);margin-top:3.5rem;padding-top:2rem}'
            '.kindpage h1{scroll-margin-top:1rem}</style></head><body>'
            '<div class="wrap" id="top">%s%s<footer>Generated by '
            '<code>tools/gen-manual.py --single</code> from <code>cpp/src/spec.h</code> '
            'and <code>cpp/src/conformance.inc</code>.</footer></div></body></html>'
            % (CSS, idx_inner, '\n'.join(body)))


def main():
    args = [a for a in sys.argv[1:] if a != '--single']
    single = '--single' in sys.argv
    out = args[0] if args else os.path.join(ROOT, 'docs')
    data = X.collect()
    spec = X._read('spec.h')
    planner = X._read('planner.h')
    planner_dml = X._read('planner_dml.h')
    names = [d['kind'] for d in data]
    sections = section_prose(planner)
    sections.update(section_prose(planner_dml))
    if not single:
        os.makedirs(out, exist_ok=True)

    if single:
        path = out if out.endswith('.html') else os.path.join(out, 'manual.html')
        os.makedirs(os.path.dirname(path) or '.', exist_ok=True)
        with open(path, 'w') as f:
            f.write(single_file(data, spec, planner, planner_dml, sections))
        print("wrote %s (%d kinds, one page)" % (path, len(data)))
        return

    for i, d in enumerate(data):
        prose = describe(d['kind'], spec, planner, planner_dml, sections)
        p = page(d, names, prose,
                 names[i - 1] if i else None,
                 names[i + 1] if i + 1 < len(names) else None)
        with open(os.path.join(out, d['kind'] + '.html'), 'w') as f:
            f.write(p)
    with open(os.path.join(out, 'index.html'), 'w') as f:
        f.write(index_page(data))

    described = sum(1 for d in data
                    if describe(d['kind'], spec, planner, planner_dml, sections))
    print("wrote %d pages + index to %s" % (len(data), out))
    print("  with a description: %d / %d" % (described, len(data)))
    print("  with a tested example: %d / %d" % (
        sum(1 for d in data if d['example']), len(data)))
    print("  options documented:    %d / %d" % (
        sum(len(d.get('hints', {})) for d in data),
        sum(len([k for k in d['accepted'] if k != 'kind']) for d in data)))


if __name__ == '__main__':
    main()
