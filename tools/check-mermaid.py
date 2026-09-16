#!/usr/bin/env python3
"""Parse every mermaid diagram in an HTML page with the real mermaid grammar.

    python3 tools/check-mermaid.py site/index.html

The diagrams are the point of the landing page, so a syntax error in one is a
build failure rather than an empty box on a live site -- which is how a broken
diagram normally ships, since nothing renders it until a visitor does.

Checked against the SAME version the page loads from its CDN, read out of the
page's own import rather than pinned here twice: a validator testing a
different version than the browser runs is a validator that can pass while the
page is broken.

Needs node, with mermaid and jsdom resolvable. jsdom because mermaid sanitises
labels through DOMPurify, which wants a DOM even when only parsing; a stub is
not enough, measured.
"""
import json
import os
import re
import subprocess
import sys
import tempfile

CHECKER = r'''
import fs from 'fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
import { createRequire } from 'node:module';

// Resolved by absolute path, not by name: ESM ignores NODE_PATH, so a checker
// that relied on it passed nothing and failed with ERR_MODULE_NOT_FOUND --
// which reads exactly like a broken diagram and is not one.
const modules = process.argv[3];
const require = createRequire(pathToFileURL(path.join(modules, 'noop.js')));
const { JSDOM } = require('jsdom');
const dom = new JSDOM('<!doctype html><body></body>', { pretendToBeVisual: true });
global.window = dom.window;
global.document = dom.window.document;
global.navigator = dom.window.navigator;
const mermaid = (await import(
  pathToFileURL(path.join(modules, 'mermaid', 'dist', 'mermaid.esm.min.mjs')).href
)).default;
mermaid.initialize({ startOnLoad: false, securityLevel: 'loose' });
const blocks = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
let bad = 0;
for (let i = 0; i < blocks.length; i++) {
  try {
    await mermaid.parse(blocks[i]);
    console.log('  ok   diagram ' + (i + 1));
  } catch (e) {
    bad++;
    const msg = String((e && e.message) || e).split('\n').slice(0, 6).join('\n         ');
    console.log('  FAIL diagram ' + (i + 1) + ': ' + msg);
  }
}
process.exit(bad ? 1 : 0);
'''


def main():
    if len(sys.argv) < 2:
        sys.exit('usage: check-mermaid.py <html file>')
    page = open(sys.argv[1]).read()

    blocks = [b.strip() for b in
              re.findall(r'<pre class="mermaid">(.*?)</pre>', page, re.S)]
    if not blocks:
        sys.exit('check-mermaid: no <pre class="mermaid"> blocks in %s -- the '
                 'page lost its diagrams, or this script lost track of how they '
                 'are written' % sys.argv[1])

    m = re.search(r'mermaid@([0-9][0-9.]*)/', page)
    if not m:
        sys.exit('check-mermaid: the page does not pin a mermaid version, so '
                 'there is no version to check against')
    version = m.group(1)

    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'blocks.json')
        with open(src, 'w') as f:
            json.dump(blocks, f)
        script = os.path.join(tmp, 'check.mjs')
        with open(script, 'w') as f:
            f.write(CHECKER)
        modules = os.environ.get('MERMAID_MODULES') or os.path.join(os.getcwd(), 'node_modules')
        for needed in ('mermaid', 'jsdom'):
            if not os.path.isdir(os.path.join(modules, needed)):
                sys.exit('check-mermaid: %s is not installed under %s. The site '
                         'job installs mermaid@%s and jsdom before calling this; '
                         'locally, npm install them or set MERMAID_MODULES.'
                         % (needed, modules, version))
        print('check-mermaid: %d diagram(s) against mermaid %s' % (len(blocks), version))
        r = subprocess.run(['node', script, src, modules])
        if r.returncode != 0:
            sys.exit('check-mermaid: a diagram does not parse')


if __name__ == '__main__':
    main()
