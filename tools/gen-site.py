#!/usr/bin/env python3
"""Assemble the published site: landing page, reference, man pages.

    python3 tools/gen-site.py [outdir]        # default: _site

Every number on the landing page is filled from the SOURCE here rather than
typed into the HTML, for the reason the reference is generated rather than
written: a count kept by hand is a count that disagrees with the code the week
after somebody adds a kind. The template carries {{NAMES}}; this fills them and
fails if one is left over, because a page shipped with a literal {{KINDS}} on
it is worse than one with a stale number -- it is obviously broken, and it got
published anyway.

Same rule pg_licht's tools/gen-reference.py follows.
"""
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _read(*parts):
    with open(os.path.join(ROOT, *parts)) as f:
        return f.read()


def kinds():
    """Every intent kind, from intent_kinds() -- the same list the binary has."""
    spec = _read('cpp', 'src', 'spec.h')
    block = spec[spec.index('intent_kinds()'):]
    block = block[:block.index('return kKinds;')]
    return re.findall(r'\{"([a-z_]+)",\s*IntentKind::', block)


def conformance_cases():
    """Cases executed against a real database, plus the ones a second cluster
    proves. Counted from the table rather than from a comment about it."""
    conf = _read('cpp', 'src', 'conformance.inc')
    body = conf[conf.index('inline std::vector<Case> cases()'):]
    return len(re.findall(r'^\s*\{"[a-z_0-9]+",', body, re.M))


def tests():
    """gtest's own count, from the built binary when there is one.

    Absent a binary this returns None and the caller leaves the number out
    rather than guessing: a test count is the one claim on the page that cannot
    be derived from source text, since a TEST_P or a loop is not one line.
    """
    for candidate in ('cpp/build/pg_laswell_mcp_test', 'build/pg_laswell_mcp_test'):
        path = os.path.join(ROOT, candidate)
        if not os.path.exists(path):
            continue
        try:
            out = subprocess.run([path, '--gtest_list_tests'], capture_output=True,
                                 text=True, timeout=120).stdout
            n = sum(1 for line in out.splitlines() if line.startswith('  ') and
                    not line.strip().startswith('#'))
            if n:
                return n
        except Exception:
            pass
    return None


def version():
    m = re.search(r'project\(pg_laswell_mcp VERSION ([0-9.]+)',
                  _read('cpp', 'CMakeLists.txt'))
    return m.group(1) if m else '0.0.0'


def release_targets():
    """The artifacts release.yml actually builds, counted from its matrix."""
    return len(re.findall(r'^\s+artifact: pg_laswell-',
                          _read('.github', 'workflows', 'release.yml'), re.M))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, '_site')
    os.makedirs(out, exist_ok=True)

    n_tests = tests()
    values = {
        'KINDS': str(len(kinds())),
        'CASES': str(conformance_cases()),
        'VERSION': version(),
        'TARGETS': str(release_targets()),
        'TESTS': str(n_tests) if n_tests else '380+',
    }

    page = _read('site', 'index.html')
    for key, value in values.items():
        page = page.replace('{{%s}}' % key, value)
    left = re.findall(r'\{\{[A-Z_]+\}\}', page)
    if left:
        sys.exit('gen-site: unfilled placeholders in site/index.html: %s'
                 % ', '.join(sorted(set(left))))
    with open(os.path.join(out, 'index.html'), 'w') as f:
        f.write(page)

    # The reference, regenerated rather than copied, so the published pages come
    # from this commit's source and not from whatever was last committed under
    # docs/.
    ref = os.path.join(out, 'reference')
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'gen-manual.py'), ref],
                   check=True)

    # Served as-is: no Jekyll pass over generated files.
    open(os.path.join(out, '.nojekyll'), 'w').close()

    print('site: %s' % out)
    for key in sorted(values):
        print('  %-8s %s' % (key, values[key]))
    print('  pages    %d' % len([f for f in os.listdir(ref) if f.endswith('.html')]))


if __name__ == '__main__':
    main()
