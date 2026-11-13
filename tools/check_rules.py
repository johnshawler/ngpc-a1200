#!/usr/bin/env python3
"""check_rules.py - the brief's rules that a script can check, before and beside the bench gates.

usage: check_rules.py [port.conf]      (run by tools/port.sh and by CI; exit code 0 = no rule broken)

1. A plan. The brief's milestone 1 is a feasibility note: FEASIBILITY.md (Step 1's class and route, the DispCfg,
   colours, objects and sizes, the budget sum, what is cut). tools/port.sh does not build without it. (The user
   dropped the approval of the plan: no "plan <sha256>" line in APPROVALS is needed.)
2. The class. CLASS=perpixel softens the gates (no late=0, no worst-frame limit); the brief keeps it for per-pixel
   renderers. It counts only with the line "class perpixel <sha256 of FEASIBILITY.md>" in APPROVALS; otherwise the
   port is judged as planar.
3. "Never do these" (BRIEF.md), as far as the sources show them: the original CPU interpreted at run time
   (cpu_step, STEP(), asm_step, asm_interp, a Z80 core in the Amiga program), float or double, a changed kit.
   A user may exempt one with "exempt <rule> <reason>" in APPROVALS (rules: interpreter, float).

Only the user writes APPROVALS (tools/approve.sh). A model never does.
"""
import hashlib, os, re, subprocess, sys


def sources(conf):
    """the Amiga program's source files, as port.conf lists them"""
    out = subprocess.run(['bash', '-c', '. "$1" >/dev/null 2>&1; echo "$SOURCES"; echo "${CLASS:-planar}"; echo "$CFLAGS"', 'x', conf],
                         capture_output=True, text=True).stdout.split('\n')
    defined = set(re.findall(r'-D(\w+)', out[2]))
    return out[0].split(), (out[1].strip() or 'planar'), defined


def conditional(text, defined):
    """drop what #ifdef / #ifndef / #else leave out for the Amiga build (macros: CFLAGS -D); #if keeps everything"""
    out, stack = [], []                         # stack of (kind, keep this branch)
    for line in text.split('\n'):
        m = re.match(r'\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b\s*(\w*)', line)
        if m:
            k, name = m.group(1), m.group(2)
            if k in ('ifdef', 'ifndef'): stack.append([k, (name in defined) == (k == 'ifdef')])
            elif k == 'if': stack.append(['if', True])
            elif k in ('else', 'elif') and stack: stack[-1][1] = True if stack[-1][0] == 'if' else not stack[-1][1]
            elif k == 'endif' and stack: stack.pop()
            continue
        if all(keep for _, keep in stack): out.append(line)
    return '\n'.join(out)


def strip_comments(text, asm):
    if asm: return re.sub(r';.*', '', text)
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//.*', '', text)


def main():
    conf = sys.argv[1] if len(sys.argv) > 1 else 'port.conf'
    root = os.path.dirname(os.path.abspath(conf))
    os.chdir(root)
    srcs, cls, defined = sources(conf)
    approvals = open('APPROVALS').read().split('\n') if os.path.exists('APPROVALS') else []
    approvals = [l.split() for l in approvals if l.strip() and not l.strip().startswith('#')]
    problems, notes = [], []

    plan_sha = hashlib.sha256(open('FEASIBILITY.md', 'rb').read()).hexdigest() if os.path.exists('FEASIBILITY.md') else None
    if plan_sha is None: problems.append('plan: no FEASIBILITY.md (the brief\'s milestone 1). Write it first')

    eff = cls
    if cls == 'perpixel':
        if not (plan_sha and ['class', 'perpixel', plan_sha] in [a[:3] for a in approvals]):
            problems.append('class: CLASS=perpixel is not approved by the user: the port is judged as planar (late=0, worst_pct <= 80)')
            eff = 'planar'
    elif cls != 'planar':
        problems.append('class: unknown CLASS=%s' % cls); eff = 'planar'

    exempt = {a[1] for a in approvals if a[0] == 'exempt' and len(a) > 1}
    interp = re.compile(r'\bcpu_step\s*\(|\bSTEP\s*\(|\b_?asm_step\b|\b_?asm_interp\b|\bz80_step\s*\(')
    flt = re.compile(r'\b(float|double)\b')
    hits = {'interpreter': [], 'float': []}
    for f in srcs:
        if f.startswith('kit/') or not os.path.exists(f): continue
        asm = f.endswith(('.s', '.asm'))
        text = strip_comments(open(f, errors='replace').read(), asm)
        if not asm: text = conditional(text, defined)
        if interp.search(text): hits['interpreter'].append(f)
        if not asm and flt.search(text): hits['float'].append(f)
    if hits['interpreter']:
        msg = 'never do these: "Emulate another CPU ... at run time": the Amiga program interprets the original CPU (%s)' % ', '.join(hits['interpreter'])
        (notes if 'interpreter' in exempt else problems).append(msg + (' (exempted by the user)' if 'interpreter' in exempt else ''))
    if hits['float']:
        msg = 'never do these: "Use float or double": %s' % ', '.join(hits['float'])
        (notes if 'float' in exempt else problems).append(msg + (' (exempted by the user)' if 'float' in exempt else ''))
    missing = [f for f in srcs if not os.path.exists(f)]
    if missing: notes.append('not checked (not made yet): %s' % ', '.join(missing))

    if os.path.exists('tools/check_kit.sh'):
        r = subprocess.run(['bash', 'tools/check_kit.sh'], capture_output=True, text=True)
        if r.returncode != 0: problems.append('kit: ' + (r.stdout + r.stderr).strip().split('\n')[-1])

    print('rules: class %s%s' % (eff, ' (port.conf says %s)' % cls if eff != cls else ''))
    for n in notes: print('rules: note:', n)
    for p in problems: print('FAIL:', p)
    if not problems: print('rules: no rule broken')
    open(os.environ.get('RULES_CLASS_FILE', os.devnull), 'w').write(eff)
    return 1 if problems else 0


sys.exit(main())
