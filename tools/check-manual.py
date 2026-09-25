#!/usr/bin/env python3
"""Assert the man pages name every intent kind the source implements.

    python3 tools/check-manual.py        # exits non-zero and says what is missing

The man pages are hand-written mdoc rather than generated, which is a deliberate
choice -- prose about lock levels and failure modes is not extractable from a
parser. The cost of that choice is drift, and drift is exactly what this checks.

It exists because the pages HAD drifted: the core page's kind list claimed to
enumerate what the build implements and named 46 of 83, and no page mentioned the
Citus module at all. Nothing caught it, because nothing was looking. The
governing invariant is written three lines above that very list -- "the set of
kinds a binary knows is exactly the set it implements" -- so a list that is a
subset makes the page contradict itself.

Two rules, and the second is the one that keeps modules honest:

  1. The core page's kind list == intent_kinds() in spec.h, exactly. Not a
     subset either way: a kind missing is undocumented, a kind listed that does
     not exist sends a reader to write a specification that will be refused.

  2. Each module's kinds appear on THAT MODULE'S page, and NOT in the core
     page's list. A module is a build-time choice, so the shipped core binary
     refuses those kinds -- documenting them as things "core implements" would
     have a reader write a specification the stock build rejects outright.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAN = os.path.join(ROOT, 'cpp', 'man')
SRC = os.path.join(ROOT, 'cpp', 'src')


def read(*parts):
    with open(os.path.join(*parts)) as f:
        return f.read()


def core_kinds():
    """The kinds core implements, from the map the parser dispatches on.

    Only literal entries: the module X-macro include in the same block expands
    to nothing here, which is the point -- module kinds are not core's.
    """
    s = read(SRC, 'spec.h')
    block = s[s.index('intent_kinds()'):]
    block = block[:block.index('return kKinds;')]
    return set(re.findall(r'\{"([a-z_]+)"', block))


def module_kinds():
    """{module: {kind, ...}} from each module's own X-macro list."""
    out = {}
    mods = os.path.join(SRC, 'modules')
    for name in sorted(os.listdir(mods)):
        inc = os.path.join(mods, name, 'kinds.inc')
        if not os.path.isfile(inc):
            continue
        out[name] = set(re.findall(r'^PGLASWELL_KIND\(\s*(\w+)', read(inc), re.M))
    return out


def marked_block(text, what, where):
    """The text between BEGIN/END marker comments, which the pages carry so the
    extent being checked is stated rather than guessed at."""
    begin = f'.\\" BEGIN {what}'
    end = f'.\\" END {what}'
    if begin not in text or end not in text:
        sys.exit(f"{where}: no '{what}' marker block. The check needs one to "
                 f"know which part of the page is the list.")
    return text[text.index(begin):text.index(end)]


def main():
    problems = []

    core = core_kinds()
    mods = module_kinds()
    core_page = read(MAN, 'pg_laswell_mcp.1')

    listed = set(re.findall(r'^\.Cm ([a-z_]+)',
                            marked_block(core_page, 'CORE KIND LIST',
                                         'pg_laswell_mcp.1'), re.M))

    for k in sorted(core - listed):
        problems.append(f'pg_laswell_mcp.1: implements {k}, does not list it')
    for k in sorted(listed - core):
        problems.append(f'pg_laswell_mcp.1: lists {k}, which no longer exists')

    for mod, kinds in mods.items():
        page_name = f'pg_laswell_{mod}.7'
        path = os.path.join(MAN, page_name)
        if not os.path.isfile(path):
            problems.append(
                f'module {mod!r} has kinds but no {page_name}. A vendor gets its '
                f'own page, because its kinds are absent from a stock build.')
            continue
        page = read(path)
        for k in sorted(kinds):
            if not re.search(r'\b' + re.escape(k) + r'\b', page):
                problems.append(f'{page_name}: module adds {k}, page never names it')
            if k in listed:
                problems.append(
                    f'pg_laswell_mcp.1: lists {k}, which is module {mod!r}, not '
                    f'core. A stock build refuses it.')
        if f'.Xr {page_name[:-2]} 7' not in core_page:
            problems.append(
                f'pg_laswell_mcp.1: never cross-references {page_name}, so a '
                f'reader has no way to find it.')

    if problems:
        print('man pages disagree with the source:\n', file=sys.stderr)
        for p in problems:
            print('  ' + p, file=sys.stderr)
        print(f'\n{len(problems)} problem(s).', file=sys.stderr)
        return 1

    total = len(core) + sum(len(v) for v in mods.values())
    print(f'man pages name all {total} kinds '
          f'({len(core)} core, ' +
          ', '.join(f'{len(v)} {m}' for m, v in mods.items()) + ')')
    return 0


if __name__ == '__main__':
    sys.exit(main())
