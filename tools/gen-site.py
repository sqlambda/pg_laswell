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
    """How many tests the suite has, counted from the source and CHECKED.

    The count was "380+" on the published page, from a fallback that fired
    because the site job builds no test binary. Vague, and needlessly: every
    test here is a plain TEST or TEST_F macro, one macro one test, so the
    source says it exactly.

    That is only true while no macro GENERATES tests -- TEST_P, TYPED_TEST and
    friends turn one line into many -- so their absence is asserted rather than
    assumed, and when a binary happens to be lying around the two numbers are
    compared. Disagreement is fatal: a page that quietly reports the wrong
    number is worse than one that admits it does not know.
    """
    src = _read('cpp', 'src', 'test_main.cpp')
    generators = re.findall(r'^(TEST_P|TYPED_TEST|TYPED_TEST_P|INSTANTIATE_\w+)\(',
                            src, re.M)
    counted = len(re.findall(r'^TEST(?:_F)?\(', src, re.M))
    if generators:
        counted = None  # one macro is no longer one test

    measured = None
    listed = set()
    for candidate in ('cpp/build/pg_laswell_mcp_test', 'build/pg_laswell_mcp_test'):
        path = os.path.join(ROOT, candidate)
        if not os.path.exists(path):
            continue
        try:
            out = subprocess.run([path, '--gtest_list_tests'], capture_output=True,
                                 text=True, timeout=120).stdout
            suite = ''
            for line in out.splitlines():
                if not line.startswith(' ') and line.strip().endswith('.'):
                    suite = line.strip()
                elif line.startswith('  ') and not line.strip().startswith('#'):
                    listed.add(suite + line.split()[0])
            if listed:
                measured = len(listed)
                break
        except Exception:
            pass

    # A module's tests live in modules/<name>/tests*.inc and are compiled in only
    # when the build enabled that module. Counted when -- and only when -- the
    # binary actually carries them, so the check stays exact for a PostgreSQL-only
    # build (what the release ships, and what the page describes) and for a
    # module build alike. Without this, any module build on the machine that runs
    # the site script made the source and the binary disagree by the module's
    # test count, and the page refused to build.
    if counted is not None and measured is not None:
        mods = os.path.join(ROOT, 'cpp', 'src', 'modules')
        for mod in sorted(os.listdir(mods)) if os.path.isdir(mods) else []:
            if not os.path.isdir(os.path.join(mods, mod)):
                continue  # README.md sits beside the module directories
            names = set()
            for f in sorted(os.listdir(os.path.join(mods, mod))):
                if f.startswith('tests') and f.endswith('.inc'):
                    text = _read('cpp', 'src', 'modules', mod, f)
                    names |= {'%s.%s' % m for m in
                              re.findall(r'^TEST(?:_F)?\(\s*(\w+)\s*,\s*(\w+)\s*\)',
                                         text, re.M)}
            if names and names <= listed:
                counted += len(names)

    if counted is not None and measured is not None and counted != measured:
        sys.exit('gen-site: the source says %d tests and the binary says %d. One '
                 'macro is no longer one test, so the count on the page cannot '
                 'be trusted until this is understood.' % (counted, measured))
    return measured or counted


def module_kinds(module):
    """A module's intent kinds, from its own kinds.inc: the one place a module
    declares them."""
    path = os.path.join(ROOT, 'cpp', 'src', 'modules', module, 'kinds.inc')
    return re.findall(r'^PGLASWELL_KIND\(\s*([a-z_0-9]+)\s*,', _read(path), re.M)


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
        'CITUS_KINDS': str(len(module_kinds('citus'))),
        'CASES': str(conformance_cases()),
        'VERSION': version(),
        'TARGETS': str(release_targets()),
        'TESTS': str(n_tests) if n_tests else '380+',  # fallback: generators present
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
