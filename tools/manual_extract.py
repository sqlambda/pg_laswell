"""Read the intent kinds, their options and their worked examples out of the
source, so the manual cannot drift from the code.

Nothing here is a list of kinds kept by hand. `intent_kinds()` names them, the
parse dispatch says which parser validates each, `reject_unknown_keys` is the
complete option list for that parser, the `require_*` calls say which options
are mandatory, the `fail()` messages carry the allowed values and the prose, and
conformance.inc supplies an example that is executed against a real database on
every test run. Same principle as cpp/test/coverage/audit.py.
"""
import json, os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'cpp', 'src')


def _read(name):
    with open(os.path.join(SRC, name)) as f:
        return f.read()


def kinds(spec):
    """Every intent kind, in the order intent_kinds() declares it."""
    block = spec[spec.index('intent_kinds()'):]
    block = block[:block.index('return kKinds;')]
    return [(m.group(1), m.group(2))
            for m in re.finditer(r'\{"([a-z_]+)",\s*IntentKind::(\w+)\}', block)]


def dispatch(spec):
    """enum name -> (parse function, extra argument).

    The argument matters: parse_rename and parse_schema_like build their key set
    from it, so rename_column accepts "column" where rename_table does not, and
    create_schema accepts "comment" where drop_schema does not. Reading the
    switch is the only way to know which.
    """
    out, pending = {}, []
    for line in spec.split('\n'):
        m = re.match(r'\s*case IntentKind::(\w+):\s*$', line)
        if m:
            pending.append(m.group(1))
            continue
        m = re.match(r'\s*case IntentKind::(\w+):\s*(parse_\w+)\((.*?)\);', line)
        if m:
            enum, fn, args = m.group(1), m.group(2), m.group(3)
            for e in pending + [enum]:
                out[e] = (fn, _extra_arg(args))
            pending = []
            continue
        m = re.match(r'\s*(parse_\w+)\((.*?)\);', line)
        if m and pending:
            for e in pending:
                out[e] = (m.group(1), _extra_arg(m.group(2)))
            pending = []
    return out


def _extra_arg(args):
    """The second argument to a parse function, as a literal, or None."""
    parts = [a.strip() for a in args.split(',')]
    if len(parts) < 2:
        return None
    a = parts[1]
    if a in ('{}', '""'):
        return ''
    if a in ('true', 'false'):
        return a == 'true'
    m = re.match(r'"([a-z_0-9]*)"', a)
    return m.group(1) if m else None


def _join_literals(text):
    """Collapse adjacent C++ string literals, so a message split across lines
    reads as one string. Without this an enum list written as
    "must be \"a\", " "\"b\" or \"c\"" is only half seen."""
    return re.sub(r'"\s*\n\s*"', '', text)


def _body(spec, fn):
    """The source text of one parse function."""
    start = spec.find('inline void %s(' % fn)
    if start < 0:
        return ''
    i, depth, seen = spec.index('{', start), 0, False
    while i < len(spec):
        if spec[i] == '{':
            depth += 1; seen = True
        elif spec[i] == '}':
            depth -= 1
            if seen and depth == 0:
                return spec[start:i + 1]
        i += 1
    return spec[start:]


def _string_list(text):
    return re.findall(r'"([a-z_0-9]+)"', text)


def options(spec, fn, arg=None):
    """Accepted keys, which are required, and any enumerated values."""
    body = _join_literals(_body(spec, fn))
    if not body:
        return {'accepted': [], 'required': [], 'enums': {}, 'hints': {}, 'source': fn}

    accepted = []
    # ONLY the key set checked against in.body. A parse function may call
    # reject_unknown_keys again on a nested object -- backfill does, for each
    # assert_invariants entry -- and those keys belong to the sub-object, not to
    # the intent. Taking them all is how "name" and "query" ended up documented
    # as top-level backfill options.
    for m in re.finditer(r'reject_unknown_keys\(\s*in\.body\s*,', body):
        i, depth = m.end(), 1
        while i < len(body) and depth:
            if body[i] == '(': depth += 1
            elif body[i] == ')': depth -= 1
            i += 1
        call = body[m.end():i]
        brace = call.find('{')
        if brace >= 0:
            accepted += _string_list(call[brace:call.find('}', brace)])

    # The set-building form: `allowed = {...}` then conditional inserts.
    m = re.search(r'allowed\s*=\s*\{(.*?)\}', body, re.S)
    if m:
        accepted += _string_list(m.group(1))
        for ins in re.finditer(r'allowed\.insert\((.*?)\)', body):
            a = ins.group(1).strip()
            lit = re.match(r'"([a-z_0-9]+)"', a)
            if lit:
                # Guarded by `if (creating)` or similar; the dispatch argument
                # says whether this kind takes that branch.
                if arg is not False:
                    accepted.append(lit.group(1))
            elif isinstance(arg, str) and arg:
                accepted.append(arg)   # allowed.insert(what)

    required = []
    for m in re.finditer(r'require_(?:string|identifier)\([^;]*?in\.body,\s*"([a-z_0-9]+)"', body):
        required.append(m.group(1))
    for m in re.finditer(r'require_string\(in\.body,\s*"([a-z_0-9]+)"', body):
        required.append(m.group(1))
    if isinstance(arg, str) and arg and re.search(r'in\.body,\s*what\b', body):
        required.append(arg)
    if arg is False:
        # drop_schema: the comment is only required when creating.
        required = [r for r in required if r != 'comment']

    enums = {}
    for m in re.finditer(r'\.([a-z_0-9]+) must be ([^\n]*)', body):
        vals = re.findall(r'\\"([a-z_ ]+)\\"', m.group(2))
        if vals:
            cur = enums.setdefault(m.group(1), [])
            cur += [v for v in vals if v not in cur]

    # The hint an author sees when they get this option wrong. It is the closest
    # thing the source has to per-option documentation, it was written to be read
    # under pressure, and it is kept honest by the tests that assert on it -- so
    # it belongs on the option rather than only in an error nobody has triggered
    # yet.
    hints = {}
    for m in re.finditer(r'fail\(', body):
        i, depth = m.end(), 1
        while i < len(body) and depth:
            if body[i] == '(': depth += 1
            elif body[i] == ')': depth -= 1
            i += 1
        call = body[m.end():i - 1]
        parts = _split_args(call)
        if len(parts) < 2:
            continue
        msg, hint = _cstr_join(parts[0]), _cstr_join(parts[1])
        if not hint:
            continue
        km = re.search(r'\.([a-z_0-9]+)\b', msg) or re.search(r'"([a-z_0-9]+)"', msg)
        if km and km.group(1) not in hints:
            hints[km.group(1)] = hint

    seen, acc = set(), []
    for k in accepted:
        if k not in seen:
            seen.add(k); acc.append(k)
    return {'accepted': acc, 'required': sorted(set(required)), 'enums': enums,
            'hints': hints, 'source': fn}


def examples(conf):
    """kind -> the worked example the conformance suite EXECUTES for it.

    Keyed off the body's own "kind" rather than the case label, because a kind
    may have more than one case and the extra ones carry descriptive labels.
    The first case for a kind wins: it is the plain one.
    """
    out = {}
    for m in re.finditer(r'json\{\{"kind"\s*,\s*"([a-z_0-9]+)"\}', conf):
        kind = m.group(1)
        if kind in out:
            continue
        i = conf.index('{', m.start() + len('json'))
        depth, start = 0, i
        while i < len(conf):
            if conf[i] == '{': depth += 1
            elif conf[i] == '}':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        raw = conf[start:i + 1]
        # The verify query and its expected value follow the body.
        tail = conf[i + 1:i + 900]
        vm = re.search(r'^\s*,\s*\n?\s*("(?:[^"\\]|\\.)*"(?:\s*\n\s*"(?:[^"\\]|\\.)*")*)\s*,\s*"([^"]*)"\s*\}', tail, re.S)
        out[kind] = {
            'raw': raw,
            'verify': _cstr(vm.group(1)) if vm else None,
            'expect': vm.group(2) if vm else None,
        }
    return out


def _split_args(call):
    """Top-level comma split of a C++ argument list, skipping string literals.

    The literals matter: a message like `.on_equivalent_index must be "rename",
    "adopt" or "refuse"` carries commas of its own, and splitting on those cuts
    the message in half and loses the hint that follows it.
    """
    out, depth, cur, i = [], 0, '', 0
    while i < len(call):
        ch = call[i]
        if ch == '"':
            j = i + 1
            while j < len(call):
                if call[j] == '\\':
                    j += 2
                    continue
                if call[j] == '"':
                    break
                j += 1
            cur += call[i:j + 1]
            i = j + 1
            continue
        if ch in '([{': depth += 1
        elif ch in ')]}': depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur); cur = ''
        else:
            cur += ch
        i += 1
    out.append(cur)
    return out


def _cstr_join(expr):
    """Every string literal in an expression, concatenated. `at + ".x is bad"`
    yields ".x is bad", which is what the key-matching below needs."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', expr)
    return ''.join(parts).replace('\\"', '"').strip()


def _cstr(text):
    """Adjacent C++ literals -> one Python string."""
    return ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', text)).replace('\\"', '"')


def deferred(conf):
    """Kinds with no single-database example, and the stated reason."""
    block = conf[conf.index('deferred_to_two_clusters()'):]
    block = block[:block.index('return kDeferred;')]
    out = {}
    for m in re.finditer(r'\{"([a-z_0-9]+)",\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\}', block):
        out[m.group(1)] = _cstr(m.group(2))
    return out


def collect():
    spec, conf = _read('spec.h'), _read('conformance.inc')
    disp, ex, defer = dispatch(spec), examples(conf), deferred(conf)
    out = []
    for name, enum in kinds(spec):
        fn, arg = disp.get(enum, (None, None))
        o = (options(spec, fn, arg) if fn
             else {'accepted': [], 'required': [], 'enums': {}, 'hints': {},
                   'source': None})
        o.update(kind=name, enum=enum, example=ex.get(name), deferred=defer.get(name))
        out.append(o)
    return out


if __name__ == '__main__':
    data = collect()
    missing_fn = [d['kind'] for d in data if not d['source']]
    no_opts = [d['kind'] for d in data if not d['accepted']]
    no_ex = [d['kind'] for d in data if not d['example']]
    print("kinds:            %d" % len(data))
    print("no parse fn:      %d %s" % (len(missing_fn), missing_fn[:8]))
    print("no option list:   %d %s" % (len(no_opts), no_opts[:8]))
    print("no example:       %d %s" % (len(no_ex), no_ex[:10]))
    print()
    for d in data[:3]:
        print("%-22s required=%s" % (d['kind'], d['required']))
        print("   accepted: %s" % ", ".join(d['accepted']))
        if d['enums']: print("   enums:    %s" % d['enums'])
