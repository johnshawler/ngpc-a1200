#!/usr/bin/env python3
"""recomp900.py - translate a Neo Geo Pocket game's TLCS-900/H code to C, once, on the host.

usage: recomp900.py [--game name] rom.ngc outdir flow1.txt [flow2.txt ...]   (game knowledge: tools/games/<name>.py)

Flow files come from tools/ngpref (option -c): executed instructions (E), targets of jumps and calls through a
register or memory with the address of the jump or call (I, C), interrupt handlers (V). They seed the entry
points; everything reachable from them is decoded statically.

Output (all generated, never edited by hand):
  outdir/gen_code.h        declarations
  outdir/gen_NN.c          the functions, one C function per entry point, in files of about 2,500 lines
  outdir/gen_dispatch.c    ngp_dispatch(): entry address -> function
  outdir/gen_waits.h       the loops where the game waits, for the interpreter that checks the translation
  outdir/gen_game.h        what tools/games/<name>.py says the run time must know

Model. The TLCS-900/H stack stays in the game's RAM. CALL pushes its return address and calls the target's C
function, which returns the address where execution goes on: what its RET popped. If that is not the caller's
own continuation the stack was manipulated, and the caller returns it upward until a caller recognises it (or
the top level dispatches it). A jump to another function is a C tail call; a jump through a register becomes a
switch over the targets the traces saw. A loop that only re-reads memory is a wait: it ends the frame
(ngp_wait) or, when it counts down meanwhile, gives the sound processor time (ngp_poll).

Instructions. The registers and flags live in the one CPU state of src/ngpc/cpu900.h, and the generated code
is built from that file's primitives (cpu_alu, cpu_rd, cpu_setr, ...), so a flag is computed in one place
only. Instruction forms this translator has no native code for yet are executed by cpu_step() on the spot
(STEP): always correct, slower. The report at the end says how much of the traced code that is.
"""
import sys, os, collections, importlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tlcs900 as T

args = sys.argv[1:]
gamename = None
if args and args[0] == '--game': gamename, args = args[1], args[2:]
if len(args) < 2: print(__doc__); sys.exit(2)
class _NoGame: pass
game = _NoGame()
if gamename:
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'games'))
    game = importlib.import_module(gamename)
EXTRA_ENTRIES = set(getattr(game, 'EXTRA_ENTRIES', ())) # functions no trace reached
NOT_WAITS = set(getattr(game, 'NOT_WAITS', ()))         # branches that look like a wait loop and are not
NATIVE = getattr(game, 'NATIVE', True)                  # False: every instruction through cpu_step() (to find a translator bug)

rom = open(args[0], 'rb').read()
outdir = args[1]

def in_rom(a): return 0x200000 <= a < 0x200000 + min(len(rom), 0x200000) or 0x800000 <= a < 0x600000 + len(rom)
def rd(a):
    a &= 0xffffff
    if 0x200000 <= a < 0x400000: return rom[a - 0x200000] if a - 0x200000 < len(rom) else 0xff
    if 0x800000 <= a < 0xa00000: return rom[a - 0x600000] if a - 0x600000 < len(rom) else 0xff
    return 0xff

# ---------------------------------------------------------------------------------------------------- inputs
executed = set(); ind_jump = collections.defaultdict(set); ind_call = set(); vectors = set()
ind_call_from = collections.defaultdict(set)    # target of a call through a register -> the calls that reached it
dyn_targets = collections.defaultdict(set)      # a call through a register -> the targets it reached
start = (rom[0x1c] | rom[0x1d] << 8 | rom[0x1e] << 16) & 0xffffff
for fn in args[2:]:
    for line in open(fn):
        p = line.split()
        if not p: continue
        if p[0] == 'E': executed.add(int(p[1], 16))
        elif p[0] == 'I': ind_jump[int(p[2], 16)].add(int(p[1], 16))
        elif p[0] == 'C': ind_call.add(int(p[1], 16)); ind_call_from[int(p[1], 16)].add(int(p[2], 16)); dyn_targets[int(p[2], 16)].add(int(p[1], 16))
        elif p[0] == 'V': vectors.add(int(p[2], 16))
ram_code = sorted(a for a in executed if not in_rom(a) and a < 0xff0000)

insn_cache = {}
def insn(a):
    if a not in insn_cache: insn_cache[a] = T.decode(rd, a) if in_rom(a) else None
    return insn_cache[a]

def successors(a):
    """(falls through?, static targets inside the function, static call target or None)"""
    d = insn(a)
    if not d: return False, [], None
    kind, cc, target = T.flow(d)
    if kind == 'ret': return cc is not None, [], None
    if kind == 'jump': return False, ([target] if target is not None else sorted(ind_jump.get(a, ()))), None
    if kind == 'branch': return True, ([target] if target is not None else sorted(ind_jump.get(a, ()))), None
    if kind == 'call': return True, [], target
    if kind == 'swi': return d.ops[0][1] == 1, [], None
    return True, [], None

# every instruction reachable from the entry points, and every call target among them
entries = {start} | {v for v in vectors if in_rom(v)} | {c for c in ind_call if in_rom(c)} | {e for e in EXTRA_ENTRIES if in_rom(e)}
reach = set(); work = list(entries)
while work:
    a = work.pop()
    if a in reach or not in_rom(a): continue
    reach.add(a)
    d = insn(a)
    if not d: continue
    ft, targets, call = successors(a)
    if ft: work.append((a + d.length) & 0xffffff)
    work += targets
    if call is not None and in_rom(call) and call not in entries: entries.add(call); work.append(call)

def body(entry):
    """addresses of the instructions of the function at entry: a jump to another function's entry is a tail call"""
    seen = set(); work = [entry]
    while work:
        a = work.pop()
        if a in seen or not in_rom(a) or (a in entries and a != entry): continue
        seen.add(a)
        d = insn(a)
        if not d: continue
        ft, targets, _ = successors(a)
        if ft: work.append((a + d.length) & 0xffffff)
        work += targets
    return sorted(seen)

# ---------------------------------------------------------------------------------------------------- wait loops
READS_ONLY = ('cp', 'bit')
def wait_kind(aset, lo, hi):
    """The loop lo..hi (hi = a conditional branch back to lo). 1 = it only re-reads memory: it waits for an
    interrupt handler, the frame ends there. 2 = it re-reads memory and counts down a register meanwhile: it
    waits for the sound processor (or gives up). None = an ordinary loop."""
    if hi in NOT_WAITS or hi - lo > 40: return None
    a, reads, written, addr_regs, pure = lo, 0, set(), set(), True
    while a <= hi:
        d = insn(a)
        if not d or a not in aset: return None
        kind = T.flow(d)[0]
        if kind in ('call', 'ret', 'swi', 'halt') or (kind in ('jump', 'branch') and T.flow(d)[2] is None): return None
        if kind is None:
            if d.mnem in T.BLOCK or d.mnem in T.COMPARE or d.mnem in ('push', 'pop', 'ex', 'ldc', 'link', 'unlk', 'mula', 'rld', 'rrd'): return None
            ops = d.ops
            for i, o in enumerate(ops):
                if o[0] == 'm':
                    if o[1] in ('inc', 'dec'): return None
                    if o[1] in ('ind', 'disp', 'idx'): addr_regs.add(o[2] & 0xfc)
                    if o[1] == 'idx': addr_regs.add(o[3] & 0xfc)
                    is_dest = i == 0 and d.mnem not in READS_ONLY
                    if is_dest or d.mnem in ('tset', 'set', 'res', 'chg', 'stcf'): return None      # a memory write
                    reads += 1
            if ops and ops[0][0] == 'r' and d.mnem not in READS_ONLY:
                written.add(ops[0][2] & 0xfc)
                if not (d.mnem == 'ld' and len(ops) == 2 and ops[1][0] in ('m', 'i')): pure = False     # not a plain reload
        elif d.mnem == 'djnz': written.add(d.ops[0][2] & 0xfc); pure = False
        a += d.length
    if not reads or addr_regs & written: return None
    return 1 if pure else 2

# ---------------------------------------------------------------------------------------------------- C generation
def fname(a): return 'F_%06x' % a
stats = collections.Counter()

ALU = {'add': 0, 'adc': 1, 'sub': 2, 'sbc': 3, 'and': 4, 'xor': 5, 'or': 6, 'cp': 7}
SHIFT = {'rlc': 0, 'rrc': 1, 'rl': 2, 'rr': 3, 'sla': 4, 'sra': 5, 'sll': 6, 'srl': 7}

# ---- flags: which an instruction reads and which it sets (exactly as src/ngpc/cpu900.h does) ----
FS, FZ, FH, FV, FN, FC = 0x80, 0x40, 0x10, 0x04, 0x02, 0x01
ALLF = FS | FZ | FH | FV | FN | FC
FLAGS = ((FS, 'fS', 7), (FZ, 'fZ', 6), (FH, 'fH', 4), (FV, 'fV', 2), (FN, 'fN', 1), (FC, 'fC', 0))
CC_USES = {0: 0, 8: 0, 1: FS | FV, 9: FS | FV, 2: FZ | FS | FV, 10: FZ | FS | FV, 3: FC | FZ, 11: FC | FZ,
           4: FV, 12: FV, 5: FS, 13: FS, 6: FZ, 14: FZ, 7: FC, 15: FC}
CC_EXPR = {0: '0', 8: '1', 1: 'CC_LT', 9: '!CC_LT', 2: 'CC_LE', 10: '!CC_LE', 3: 'CC_ULE', 11: '!CC_ULE',
           4: 'fV', 12: '!fV', 5: 'fS', 13: '!fS', 6: 'fZ', 14: '!fZ', 7: 'fC', 15: '!fC'}
NOFLAGS = ('ld', 'lda', 'ldar', 'push', 'pop', 'ex', 'extz', 'exts', 'paa', 'mirr', 'ldc', 'link', 'unlk', 'ldx', 'nop',
           'ei', 'ldf', 'incf', 'decf', 'minc1', 'minc2', 'minc4', 'mdec1', 'mdec2', 'mdec4', 'mul', 'muls', 'res', 'set',
           'chg', 'swi', 'djnz')

def flags_ud(d):
    """(read, set) masks of the flags for a non-flow instruction"""
    m, ops = d.mnem, d.ops
    kinds = tuple(o[0] for o in ops)
    if m in ('push', 'pop') and kinds and kinds[0] in ('sr', 'f'):
        return (ALLF, 0) if m == 'push' else (0, ALLF)
    if m == 'ex' and kinds == ('f', 'f2'): return ALLF, ALLF
    if m in NOFLAGS: return 0, 0
    if m in ALU: return (FC if m in ('adc', 'sbc') else 0), ALLF
    if m == 'neg': return 0, ALLF
    if m == 'cpl': return 0, FH | FN
    if m in ('inc', 'dec'):
        if ops[1][0] == 'r' and ops[1][1] in (2, 4): return 0, 0
        return 0, ALLF & ~FC
    if m in SHIFT: return (FC if m in ('rl', 'rr') else 0), ALLF
    if m == 'daa': return FH | FN | FC, FS | FZ | FH | FV | FC
    if m in ('rld', 'rrd'): return 0, FS | FZ | FH | FV | FN
    if m in ('div', 'divs', 'bs1f', 'bs1b'): return 0, FV
    if m in ('bit', 'tset'): return 0, FZ | FH | FN
    if m in ('andcf', 'orcf', 'xorcf', 'ldcf'): return FC, FC        # a bit number past the operand leaves C as it is
    if m == 'stcf': return FC, 0
    if m in ('scf', 'rcf'): return 0, FH | FN | FC
    if m == 'ccf': return FC, FN | FC
    if m == 'zcf': return FZ, FN | FC
    if m in T.BLOCK: return 0, FH | FV | FN
    if m in T.COMPARE: return 0, FS | FZ | FH | FV | FN
    if m == 'mula': return 0, FS | FZ | FV
    if m == 'scc': return CC_USES[ops[0][1]], 0
    if m == 'reti': return 0, ALLF
    return ALLF, 0                                          # not known: everything must be in cpu.sr before it

def savef(mask): return 'SAVEF(0x%02x);' % mask if mask else ''
def loadf(mask): return ' '.join('LOADF_%s();' % n[1] for f, n, _ in FLAGS if mask & f)

# ---- operands ----
def basep(code):
    i = (code >> 2) & 3
    if code >= 0xf0: return '(&cpu.gpr[%d])' % i
    if code >= 0xe0: return '(R + %d)' % i
    if code < 0x40: return '(&cpu.bank[%d][%d])' % (code >> 4, i)
    return '(PREV() + %d)' % i
def reg(size, code):
    """a register as a C lvalue"""
    b = basep(code)
    if size == 4: return '(*%s)' % b
    if size == 2: return 'W16(%s, %d)' % (b, (code >> 1) & 1)
    return 'B8(%s, %d)' % (b, code & 3)

def ea_code(o):
    """(statements first, C expression of the address) of a memory operand. (-r) and (r+) move the register
    before the operation, as the interpreter does."""
    m = o[1]
    if m in ('abs', 'pcrel'): return '', '0x%06xu' % o[2]
    if m == 'ind': return '', reg(4, o[2])
    if m == 'disp': return '', '(%s + (uint32_t)(%d))' % (reg(4, o[2]), o[3])
    if m == 'idx': return '', '(%s + (uint32_t)(int%d_t)%s)' % (reg(4, o[2]), o[4] * 8, reg(o[4], o[3]))
    if m == 'dec': return 'xe = (%s -= %d);' % (reg(4, o[2]), o[3]), 'xe'
    if m == 'inc': return 'xe = %s; %s += %d;' % (reg(4, o[2]), reg(4, o[2]), o[3]), 'xe'
    raise ValueError(m)

def rd_mem(size, addr):
    """read of size bytes at a C address expression; constant addresses in RAM or the cartridge read directly"""
    if addr.startswith('0x') and addr.endswith('u'):
        a = int(addr[:-1], 16)
        if 0x4000 <= a and a + size <= 0x7000: p = 'ngp_ram + 0x%04x' % (a - 0x4000)
        elif 0x200000 <= a and a + size <= 0x400000: p = 'ngp_rom + 0x%06x' % (a - 0x200000)
        else: p = None
        if p: return '(uint32_t)(%s)[0]' % p if size == 1 else 'LE%d(%s)' % (size * 8, p)
    return 'RD%d(%s)' % (size * 8, addr)

def wr_mem(size, addr, v):
    if addr.startswith('0x') and addr.endswith('u'):
        a = int(addr[:-1], 16)
        if 0x4000 <= a and a + size <= 0x7000:
            o = a - 0x4000
            return ' '.join('ngp_ram[0x%04x] = (uint8_t)(%s);' % (o + i, v if i == 0 else '(%s) >> %d' % (v, 8 * i)) for i in range(size))
    return 'WR%d(%s, %s);' % (size * 8, addr, v)

MASK = {1: '0xffu', 2: '0xffffu', 4: '0xffffffffu'}
SEXT = {1: 'int8_t', 2: 'int16_t', 4: 'int32_t'}

def alu_code(op, size, live):
    """xa op xb -> xr, and the flags in live; op as cpu_alu: 0 ADD 1 ADC 2 SUB 3 SBC 4 AND 5 XOR 6 OR 7 CP"""
    n = size * 8; M = MASK[size]; L = []
    cin = op in (1, 3)
    if cin: L.append('xc = fC;')
    if op in (0, 1):
        L.append('xr = %s;' % ('(xa + xb%s) & %s' % (' + xc' if cin else '', M) if size < 4 else 'xa + xb%s' % (' + xc' if cin else '')))
        if live & FC:
            if size < 4: L.append('fC = (xa + xb%s) >> %d;' % (' + xc' if cin else '', n))
            else: L.append('fC = xr < xa%s;' % (' || (xc && xr == xa)' if cin else ''))
        if live & FV: L.append('fV = ((~(xa ^ xb) & (xa ^ xr)) >> %d) & 1;' % (n - 1))
        if live & FH: L.append('fH = %s;' % ('((xa ^ xb ^ xr) >> 4) & 1' if size < 4 else '0'))
        if live & FN: L.append('fN = 0;')
    elif op in (2, 3, 7):
        L.append('xr = %s;' % ('(xa - xb%s) & %s' % (' - xc' if cin else '', M) if size < 4 else 'xa - xb%s' % (' - xc' if cin else '')))
        if live & FC:
            if size < 4: L.append('fC = xa < xb%s;' % (' + xc' if cin else ''))
            else: L.append('fC = %s;' % ('xc ? xa <= xb : xa < xb' if cin else 'xa < xb'))
        if live & FV: L.append('fV = (((xa ^ xb) & (xa ^ xr)) >> %d) & 1;' % (n - 1))
        if live & FH: L.append('fH = %s;' % ('((xa ^ xb ^ xr) >> 4) & 1' if size < 4 else '0'))
        if live & FN: L.append('fN = 1;')
    else:
        L.append('xr = xa %s xb;' % {4: '&', 5: '^', 6: '|'}[op])
        if live & FC: L.append('fC = 0;')
        if live & FV: L.append('fV = %s;' % ('EVEN8(xr)' if size == 1 else 'EVEN16(xr)' if size == 2 else '0'))
        if live & FH: L.append('fH = %d;' % (1 if op == 4 else 0))
        if live & FN: L.append('fN = 0;')
    if live & FS: L.append('fS = (xr >> %d) & 1;' % (n - 1))
    if live & FZ: L.append('fZ = xr == 0;')
    return L

def shift_code(op, size, n, live):
    """xa shifted n times (1..16) -> xr, flags as cpu_shift: 0 RLC 1 RRC 4 SLA 5 SRA 6 SLL 7 SRL (RL, RR: None)"""
    bits = size * 8; M = MASK[size]; L = []
    if op in (4, 6):
        L.append('xr = %s;' % ('0' if n >= bits else '(xa << %d) & %s' % (n, M)))
        c = '(xa >> %d) & 1' % (bits - n) if n <= bits else '0'
    elif op == 7:
        L.append('xr = %s;' % ('0' if n >= bits else 'xa >> %d' % n))
        c = '(xa >> %d) & 1' % (n - 1) if n <= bits else '0'
    elif op == 5:
        if n >= bits: L.append('xr = (xa >> %d) & 1 ? %s : 0;' % (bits - 1, M)); c = '(xa >> %d) & 1' % (bits - 1)
        else: L.append('xr = (uint32_t)((%s)xa >> %d) & %s;' % (SEXT[size], n, M)); c = '(xa >> %d) & 1' % (n - 1)
    elif op in (0, 1):
        k = n % bits
        if k == 0: L.append('xr = xa;')
        elif op == 0: L.append('xr = ((xa << %d) | (xa >> %d)) & %s;' % (k, bits - k, M))
        else: L.append('xr = ((xa >> %d) | (xa << %d)) & %s;' % (k, bits - k, M))
        c = 'xr & 1' if op == 0 else '(xr >> %d) & 1' % (bits - 1)
    else: return None
    if live & FC: L.append('fC = %s;' % c)
    if live & FS: L.append('fS = (xr >> %d) & 1;' % (bits - 1))
    if live & FZ: L.append('fZ = xr == 0;')
    if live & FV: L.append('fV = %s;' % ('EVEN8(xr)' if size == 1 else 'EVEN16(xr)' if size == 2 else '0'))
    if live & FH: L.append('fH = 0;')
    if live & FN: L.append('fN = 0;')
    return L

def native(d, live):
    """C statements for a non-flow instruction (live: the flags read after it), or None: then the interpreter's
    step runs it on the spot. Every form mirrors the case of the same instruction in src/ngpc/cpu900.h."""
    if not NATIVE: return None
    m, ops, s = d.mnem, d.ops, d.size
    kinds = tuple(o[0] for o in ops)
    if m in ('ei', 'di'):
        return 'cpu.sr = (uint16_t)((cpu.sr & ~0x7000) | 0x%04x);' % ((7 if m == 'di' else ops[0][1] & 7) << 12)
    if m in ('scf', 'rcf'): return ' '.join(x for x, f in (('fH = 0;', FH), ('fN = 0;', FN), ('fC = %d;' % (m == 'scf'), FC)) if live & f) or ';'
    if m == 'ccf': return ' '.join(x for x, f in (('fN = 0;', FN), ('fC ^= 1;', FC)) if live & f) or ';'
    if m == 'zcf': return ' '.join(x for x, f in (('fN = 0;', FN), ('fC = !fZ;', FC)) if live & f) or ';'
    if m == 'nop': return ';'
    if m in ('ldf', 'incf', 'decf'):
        rfp = '%d' % (ops[0][1] & 3) if m == 'ldf' else '((cpu.sr >> 8) %s 1) & 3' % ('+' if m == 'incf' else '-')
        return 'cpu.sr = (uint16_t)((cpu.sr & ~0x0700) | ((%s) << 8)); CPU_RFP_SET(); R = cpu.cur;' % rfp
    if any(k in ('sr', 'f', 'f2', 'cr', 'cc', 'a') for k in kinds) and m != 'scc': return None
    pre = []
    def src(o, size=None):
        if o[0] == 'r': return '(uint32_t)%s' % reg(o[1], o[2])
        if o[0] == 'i': return '0x%xu' % (o[1] & 0xffffffff)
        st, e = ea_code(o)
        if st: pre.append(st)
        return rd_mem(size or s, e)
    def addr(o):
        st, e = ea_code(o)
        if st: pre.append(st); return e
        if e.startswith('0x'): return e
        pre.append('xe = %s;' % e); return 'xe'
    def done(*text): return ' '.join(pre + list(text))
    if m == 'ld' and kinds in (('r', 'r'), ('r', 'i'), ('r', 'm')):
        v = src(ops[1]); return done('%s = %s;' % (reg(ops[0][1], ops[0][2]), v))
    if m == 'ld' and kinds in (('m', 'r'), ('m', 'i')):
        a = addr(ops[0]); return done(wr_mem(s, a, src(ops[1])))
    if m == 'ld' and kinds == ('m', 'm'):                   # LD (nn),(mem) and LD (mem),(nn): one side is a plain address
        if ops[0][1] == 'abs': v = src(ops[1]); return done('xb = %s;' % v, wr_mem(s, '0x%06xu' % ops[0][2], 'xb'))
        a = addr(ops[0]); return done('xb = %s;' % rd_mem(s, '0x%06xu' % ops[1][2]), wr_mem(s, a, 'xb'))
    if m == 'lda' and kinds == ('r', 'm') and ops[1][1] not in ('dec', 'inc'):
        return '%s = %s;' % (reg(ops[0][1], ops[0][2]), ea_code(ops[1])[1])
    if m == 'push' and kinds == ('r',): return 'PUSH%d(%s);' % (ops[0][1] * 8, reg(ops[0][1], ops[0][2]))
    if m == 'push' and kinds == ('i',): return 'PUSH%d(0x%xu);' % (s * 8, ops[0][1])
    if m == 'push' and kinds == ('m',): v = src(ops[0]); return done('xb = %s;' % v, 'PUSH%d(xb);' % (s * 8))
    if m == 'pop' and kinds == ('r',): return '%s = POP%d();' % (reg(ops[0][1], ops[0][2]), ops[0][1] * 8)
    if m == 'pop' and kinds == ('m',):
        a = addr(ops[0]); return done('xb = POP%d();' % (s * 8), wr_mem(s, a, 'xb'))
    if m in ALU and kinds in (('r', 'r'), ('r', 'i'), ('r', 'm')):
        v = src(ops[1]); r = reg(ops[0][1], ops[0][2])
        L = ['xa = %s; xb = %s;' % (r, v)] + alu_code(ALU[m], s, live) + ([] if m == 'cp' else ['%s = xr;' % r])
        return done(*L)
    if m in ALU and kinds in (('m', 'r'), ('m', 'i')):
        a = addr(ops[0])
        L = ['xa = %s; xb = %s;' % (rd_mem(s, a), src(ops[1]))] + alu_code(ALU[m], s, live) + ([] if m == 'cp' else [wr_mem(s, a, 'xr')])
        return done(*L)
    if m == 'neg' and kinds == ('r',):
        r = reg(ops[0][1], ops[0][2]); return ' '.join(['xa = 0; xb = %s;' % r] + alu_code(2, ops[0][1], live) + ['%s = xr;' % r])
    if m == 'cpl' and kinds == ('r',):
        r = reg(ops[0][1], ops[0][2])
        return ' '.join(['%s = ~%s;' % (r, r)] + [x for x, f in (('fH = 1;', FH), ('fN = 1;', FN)) if live & f])
    if m in ('inc', 'dec') and kinds == ('i', 'r'):
        sz, c, n = ops[1][1], ops[1][2], ops[0][1]; r = reg(sz, c)
        if sz > 1: return '%s %s= %d;' % (r, '-' if m == 'dec' else '+', n)
        return ' '.join(['xa = %s; xb = %d;' % (r, n)] + alu_code(2 if m == 'dec' else 0, 1, live & ~FC) + ['%s = xr;' % r])
    if m in ('inc', 'dec') and kinds == ('i', 'm'):
        a = addr(ops[1])
        return done(*(['xa = %s; xb = %d;' % (rd_mem(s, a), ops[0][1])] + alu_code(2 if m == 'dec' else 0, s, live & ~FC) + [wr_mem(s, a, 'xr')]))
    if m in SHIFT and kinds == ('i', 'r'):                  # count 0 means 16; the count is known here
        n = ops[0][1] or 16; sz, c = ops[1][1], ops[1][2]; r = reg(sz, c)
        L = shift_code(SHIFT[m], sz, n, live)
        if L is None: return None
        return ' '.join(['xa = %s;' % r] + L + ['%s = xr;' % r])
    if m in SHIFT and kinds == ('m',):
        a = addr(ops[0]); L = shift_code(SHIFT[m], s, 1, live)
        if L is None: return None
        return done(*(['xa = %s;' % rd_mem(s, a)] + L + [wr_mem(s, a, 'xr')]))
    if m in ('extz', 'exts') and kinds == ('r',):
        sz, c = ops[0][1], ops[0][2]; half = 'uint%d_t' % (sz * 4) if m == 'extz' else 'int%d_t' % (sz * 4)
        r = reg(sz, c); return '%s = (uint32_t)(%s)%s;' % (r, half, r)
    if m == 'ex' and kinds == ('r', 'r'):
        a, b = reg(ops[0][1], ops[0][2]), reg(ops[1][1], ops[1][2])
        return 'xa = %s; %s = %s; %s = xa;' % (a, a, b, b)
    if m in ('bit', 'set', 'res', 'chg', 'tset') and kinds == ('i', 'r'):
        bit, sz, r = ops[0][1], ops[1][1], reg(ops[1][1], ops[1][2])
        flags = [x for x, f in (('fH = 1;', FH), ('fN = 0;', FN)) if live & f]
        if bit >= sz * 8:                                   # a bit the register does not have
            return ' '.join((['fZ = 1;'] if live & FZ else []) + flags) if m in ('bit', 'tset') else ';'
        z = ['fZ = !((%s >> %d) & 1);' % (r, bit)] if live & FZ else []
        if m == 'bit': return ' '.join(z + flags) or ';'
        if m == 'tset': return ' '.join(z + flags + ['%s |= 0x%xu;' % (r, 1 << bit)])
        return '%s %s 0x%xu;' % (r, {'set': '|=', 'res': '&=', 'chg': '^='}[m], (1 << bit) if m != 'res' else ((~(1 << bit)) & int(MASK[sz][:-1], 16)))
    if m in ('bit', 'set', 'res', 'chg', 'tset') and kinds == ('i', 'm'):
        bit = ops[0][1] & 7; a = addr(ops[1])
        flags = [x for x, f in (('fH = 1;', FH), ('fN = 0;', FN)) if live & f]
        L = ['xa = %s;' % rd_mem(1, a)]
        if m in ('bit', 'tset') and live & FZ: L.append('fZ = !((xa >> %d) & 1);' % bit)
        if m in ('bit', 'tset'): L += flags
        if m != 'bit':
            L.append(wr_mem(1, a, {'set': 'xa | 0x%x' % (1 << bit), 'tset': 'xa | 0x%x' % (1 << bit), 'res': 'xa & 0x%x' % (0xff & ~(1 << bit)), 'chg': 'xa ^ 0x%x' % (1 << bit)}[m]))
        return done(*L)
    if m == 'scc' and kinds == ('cc', 'r'):
        return '%s = %s;' % (reg(ops[1][1], ops[1][2]), CC_EXPR[ops[0][1]])
    if m in ('mul', 'muls') and len(ops) == 2 and ops[0][0] == 'r' and s in (1, 2):
        dst = reg(ops[0][1], ops[0][2]); v = src(ops[1])
        if m == 'mul': return done('%s = (%s & %s) * %s;' % (dst, dst, MASK[s], v))
        return done('%s = (uint32_t)((%s)%s * (%s)%s);' % (dst, SEXT[s], dst, SEXT[s], v))
    if m in ('ldir', 'lddr') and s in (1, 2):
        dp, sp_ = reg(4, ops[0][2]), reg(4, ops[1][2]); step = s if m == 'ldir' else -s
        flags = [x for x, f in (('fH = 0;', FH), ('fV = 0;', FV), ('fN = 0;', FN)) if live & f]
        return ' '.join(['do { WR%d(%s, RD%d(%s)); %s += %d; %s += %d; } while (--W16(R + 1, 0));' % (s * 8, dp, s * 8, sp_, dp, step, sp_, step)] + flags)
    return None

# ---- instructions with no inline form: one C function each, called by both back ends ----
ops_needed = {}                                             # address -> C body of op_<address>(): the next pc
def op_body(a, d):
    """C statements of op_<a>() for an instruction the back ends do not write inline: it works on the CPU state
    (registers in cpu, flags in cpu.sr, with cpu900.h's primitives, as the instruction's case there does) and
    returns the address where execution goes on. Decoded here, once: nothing is decoded at run time."""
    m, ops, s = d.mnem, d.ops, d.size
    kinds = tuple(o[0] for o in ops)
    fall = (a + d.length) & 0xffffff
    kind, cc, target = T.flow(d)
    pre = []
    def addr(o):
        st, e = ea_code(o)
        if st: pre.append(st)
        return e
    def src(o, size):
        if o[0] == 'r': return '(uint32_t)%s' % reg(o[1], o[2])
        if o[0] == 'i': return '0x%xu' % (o[1] & 0xffffffff)
        return 'cpu_rd(%d, %s)' % (size, addr(o))
    guard = 'if (!cpu_cond(%d)) return 0x%06x; ' % (cc, fall) if isinstance(cc, int) else ''
    if m in ('div', 'divs') and kinds[0] == 'r' and s in (1, 2):
        v = src(ops[1], s); r = reg(ops[0][1], ops[0][2])
        return pre + ['xa = %s;' % v, '%s = cpu_div(%d, %s, xa, %d);' % (r, s, r, m == 'divs'), 'return 0x%06x;' % fall]
    if m in ('cpi', 'cpir', 'cpd', 'cpdr') and kinds == ('r', 'm') and s in (1, 2):
        p = '&%s' % reg(4, ops[1][2]); step = -s if m in ('cpd', 'cpdr') else s
        loop = ('uint32_t *p = %s; do { uint16_t c = cpu.sr & F_C; cpu_alu(7, %d, cpu_getr(%d, 0xe0), cpu_rd(%d, *p)); *p += (uint32_t)(%d);'
                ' xa = (cpu_getr(2, 0xe4) - 1) & 0xffff; cpu_setr(2, 0xe4, xa); cpu.sr = (uint16_t)((cpu.sr & ~(F_C | F_V)) | c | (xa ? F_V : 0)); }'
                ' while (%s);' % (p, s, s, s, step, 'xa && !(cpu.sr & F_Z)' if m in ('cpir', 'cpdr') else '0'))
        return ['{ ' + loop + ' }', 'return 0x%06x;' % fall]
    if m == 'ex' and kinds in (('m', 'r'), ('r', 'm')):
        mo, ro = (ops[0], ops[1]) if kinds[0] == 'm' else (ops[1], ops[0])
        e = addr(mo); r = reg(ro[1], ro[2])
        return pre + ['xe = %s; xa = cpu_rd(%d, xe); cpu_wr(%d, xe, %s); %s = xa;' % (e, s, s, r, r), 'return 0x%06x;' % fall]
    if m == 'push' and kinds == ('sr',): return ['cpu_push(2, cpu.sr);', 'return 0x%06x;' % fall]
    if m == 'pop' and kinds == ('sr',): return ['cpu_set_sr((uint16_t)cpu_pop(2));', 'return 0x%06x;' % fall]
    if m in ('ldf', 'incf', 'decf'):
        rfp = '%d' % (ops[0][1] & 3) if m == 'ldf' else '((cpu.sr >> 8) %s 1) & 3' % ('+' if m == 'incf' else '-')
        return ['cpu.sr = (uint16_t)((cpu.sr & ~0x0700) | ((%s) << 8)); CPU_RFP_SET();' % rfp, 'return 0x%06x;' % fall]
    if kind == 'ret' and m == 'reti': return [guard + 'cpu_set_sr((uint16_t)cpu_pop(2)); xa = cpu_pop(4); cpu.cr[0x3c]--;', 'return xa & 0xffffff;']
    if kind == 'ret' and m == 'retd':
        return [guard + 'xa = cpu_pop(4); XSP += (uint32_t)(int32_t)(%d);' % ops[0][1], 'return xa & 0xffffff;']
    if kind == 'ret' and m == 'ret': return [guard + 'return cpu_pop(4) & 0xffffff;']
    if kind in ('jump', 'branch') and m == 'djnz':
        r = reg(ops[0][1], ops[0][2]); return ['if (--%s) return 0x%06x;' % (r, target), 'return 0x%06x;' % fall]
    if kind in ('jump', 'call') and target is None and ops and ops[-1][0] == 'm':
        e = addr(ops[-1])
        body = pre + ['xe = (%s) & 0xffffff;' % e] + (['cpu_push(4, 0x%06x);' % fall] if kind == 'call' else []) + ['return xe;']
        return ([guard.strip()] if guard else []) + body
    raise ValueError('no native form for %06x %s' % (a, T.fmt(d)))
def need_op(a):
    """the name of the C function for the instruction at a (made the first time it is asked for)"""
    if a not in ops_needed: ops_needed[a] = op_body(a, insn(a))
    return 'op_%06x' % a

# ---- liveness of the flags ----
in_use = collections.defaultdict(int)                       # flags a function reads from cpu.sr when it is entered
ret_live = collections.defaultdict(int)                     # flags its callers read after it returns: what its RET stores
PRECISE_RET = getattr(game, 'PRECISE_RET', True)            # False: every RET stores all flags (if a game unwinds its stack)
cur_entry = [None]

def exit_flags(t):
    """flags that must be in cpu.sr when control leaves the function for t"""
    return in_use[t] if t in entries else ALLF

def node_info(a, aset, waits):
    """(read, set, internal successors, flags needed on the edges that leave the function)"""
    d = insn(a)
    if not d: return 0, 0, [], 0
    fall = (a + d.length) & 0xffffff
    kind, cc, target = T.flow(d)
    use = CC_USES.get(cc, 0) if cc not in (None, 'djnz') else 0
    succ, out = [], 0
    def edge(t):
        nonlocal out
        if t in aset: succ.append(t)
        else: out |= exit_flags(t)
    if kind is None:
        ft = True
        r, w = flags_ud(d)
        if native(d, ALLF) is None:                         # the step reads the flags from cpu.sr: all it may read, or keep
            pass
        edge(fall)
        return r, w, succ, out
    if kind == 'halt': edge(fall); return ALLF, 0, succ, out
    if kind == 'swi':
        if d.ops[0][1] == 1: edge(fall)
        return 0, 0, succ, out
    if kind == 'ret':
        if d.mnem == 'reti': return 0, ALLF, succ, out
        if cc is not None: edge(fall)
        return use | (ret_live[cur_entry[0]] if PRECISE_RET else ALLF), 0, succ, out
    if kind in ('jump', 'branch'):
        if a in waits: use |= ALLF
        if target is not None:
            edge(target)
            if kind == 'branch': edge(fall)
        else:
            use |= ALLF
            for t in sorted(ind_jump.get(a, ())): edge(t)
            if kind == 'branch': edge(fall)
        return use, 0, succ, out
    if kind == 'call':
        if target is None:                                  # through a register: what the targets the traces saw read
            ts = dyn_targets.get(a, ())
            callee_in = ALLF if not ts or any(t not in entries for t in ts) else 0
            for t in ts: callee_in |= in_use[t] if t in entries else ALLF
        else: callee_in = in_use[target] if target in entries else ALLF
        edge(fall)
        if cc is None: return callee_in, ALLF, succ, out    # after the call every flag comes from the callee's return
        return use | callee_in, 0, succ, out
    return ALLF, 0, succ, out

def liveness(entry):
    cur_entry[0] = entry
    addrs = body(entry)
    aset = set(addrs)
    waits = find_waits(addrs, aset)
    info = {a: node_info(a, aset, waits) for a in addrs}
    live_in = {a: 0 for a in addrs}
    changed = True
    while changed:
        changed = False
        for a in reversed(addrs):
            r, w, succ, out = info[a]
            lo = out
            for t in succ: lo |= live_in[t]
            li = r | (lo & ~w)
            if li != live_in[a]: live_in[a] = li; changed = True
    live_out = {}
    for a in addrs:
        r, w, succ, out = info[a]
        lo = out
        for t in succ: lo |= live_in[t]
        live_out[a] = lo
    return addrs, aset, waits, live_in, live_out

def leaves(entry, addrs, aset):
    """(static call targets with the call's address, entries this function jumps to): for ret_live"""
    calls, jumps = [], set()
    for a in addrs:
        d = insn(a)
        if not d: continue
        kind, cc, target = T.flow(d)
        fall = (a + d.length) & 0xffffff
        if kind == 'call' and target is not None and target in entries: calls.append((a, target))
        outs = []
        if kind in ('jump', 'branch') and target is not None: outs.append(target)
        if kind in ('jump', 'branch') and target is None: outs += sorted(ind_jump.get(a, ()))
        if kind not in ('jump', 'ret', 'halt') or (kind == 'branch'): outs.append(fall)
        if kind == 'ret' and cc is not None: outs.append(fall)
        for t in outs:
            if t not in aset and t in entries: jumps.add(t)
    return calls, jumps

def find_waits(addrs, aset):
    waits = {}
    for a in addrs:
        d = insn(a)
        if d and d.mnem in ('jr', 'jrl', 'jp', 'djnz'):
            kind, cc, target = T.flow(d)
            if kind == 'branch' and target is not None and target <= a and target in aset:
                k = wait_kind(aset, target, a)
                if k: waits[a] = k
    return waits

owner = set()                                            # the addresses of instructions some function holds

def solve_in_use():
    """which flags each function reads on entry, and which its callers read after it returns: a fixed point over all
    functions (they call each other). Functions entered from outside (start, interrupts, calls through registers)
    return all flags."""
    for e in {start} | vectors | set(EXTRA_ENTRIES): ret_live[e] = ALLF
    for e in entries: owner.update(body(e))
    for t in ind_call:                                   # called through a register: from a call the traces did not see?
        if not ind_call_from[t] or any(f not in owner for f in ind_call_from[t]): ret_live[t] = ALLF
    for it in range(100):
        changed = 0
        for e in sorted(entries):
            addrs, aset, waits, live_in, live_out = liveness(e)
            v = live_in.get(e, ALLF)
            if v != in_use[e]: in_use[e] = v; changed += 1
            calls, jumps = leaves(e, addrs, aset)
            for a, t in calls:
                if live_out[a] & ~ret_live[t]: ret_live[t] |= live_out[a]; changed += 1
            for a in addrs:                              # calls through a register: what the traces saw them reach
                for t in dyn_targets.get(a, ()):
                    if t in entries and live_out[a] & ~ret_live[t]: ret_live[t] |= live_out[a]; changed += 1
            for t in jumps:
                if ret_live[e] & ~ret_live[t]: ret_live[t] |= ret_live[e]; changed += 1
        if not changed: return it + 1
    raise SystemExit('recomp900: the flag liveness does not settle')

def gen_function(entry):
    addrs, aset, waits, live_in, live_out = liveness(entry)
    for a, k in waits.items(): all_waits[a] = k
    labels = set(); lines = []
    def goto(t, src_live=None):
        if t in aset: labels.add(t); return 'goto L_%06x;' % t
        sv = savef(exit_flags(t))
        if t in entries: return '%s return %s();' % (sv, fname(t))
        return '%s return ngp_dispatch(0x%06x);' % (sv, t)
    for idx, a in enumerate(addrs):
        d = insn(a)
        nxt = addrs[idx + 1] if idx + 1 < len(addrs) else None
        L = []
        if not d:
            lines.append((a, ['ngp_fatal(0x%06x, 1); return 0;' % a], 'not an instruction')); continue
        fall = (a + d.length) & 0xffffff
        kind, cc, target = T.flow(d)
        cond = None if cc in (None, 'djnz') else CC_EXPR[cc]
        li, lo = live_in[a], live_out[a]
        step = lambda: ' '.join(x for x in (savef(li), 'cpu.pc = %s(); R = cpu.cur;' % need_op(a)) if x)
        ft = True
        if kind is None:
            c = native(d, lo)
            if c: L.append(c); stats['native'] += 1
            else:
                L.append(step()); L.append(loadf(lo)); stats['step'] += 1; step_forms[T.fmt(d).split()[0]] += 1
        elif kind == 'halt': L.append('%s ngp_wait(0x%06x); R = cpu.cur; %s' % (savef(ALLF), fall, loadf(lo)))
        elif kind == 'swi':
            if d.ops[0][1] == 1: L.append('ngp_bios_call(cpu.bank[3][0] >> 8 & 0xff);')
            else: L.append('ngp_fatal(0x%06x, 1); return 0;' % a); ft = False
        elif kind == 'ret':
            if d.mnem == 'ret' and NATIVE: pop = '%s return POP32() & 0xffffff;' % savef(ret_live[entry] if PRECISE_RET else ALLF)
            else: pop = step() + ' return cpu.pc;'          # RETD and RETI: op_ functions
            if cc is None: L.append(pop); ft = False
            else: L.append('if (%s) { %s }' % (cond, pop))
        elif kind in ('jump', 'branch'):
            def wait_then(t):
                return '%s %s(0x%06x); R = cpu.cur; %s %s' % (savef(ALLF), 'ngp_wait' if waits[a] == 1 else 'ngp_poll', t, loadf(live_in.get(t, ALLF)) if t in aset else '', goto(t))
            if d.mnem == 'djnz':
                r = reg(d.ops[0][1], d.ops[0][2])
                if a in waits: L.append('if (--%s) { %s }' % (r, wait_then(target)))
                else: L.append('if (--%s) %s' % (r, goto(target)))
            elif target is not None:
                if a in waits: L.append('if (%s) { %s }' % (cond, wait_then(target)))
                elif cond: L.append('if (%s) { %s }' % (cond, goto(target)))
                else: L.append(goto(target)); ft = False
            else:                                           # through a register or memory: the traces' targets
                cases = ' '.join('case 0x%06x: %s' % (t, goto(t)) for t in sorted(ind_jump.get(a, ())))
                sw = 'switch (cpu.pc) { %s default: return ngp_dispatch(cpu.pc); }' % cases
                if cond: L.append('%s if (cpu.pc != 0x%06x) %s' % (step(), fall, sw))
                else: L.append('%s %s' % (step(), sw)); ft = False
        elif kind == 'call':
            ret = fall
            if target is not None:
                callee = '%s()' % fname(target) if target in entries else 'ngp_dispatch(0x%06x)' % target
                c = '%s PUSH32(0x%06xu); r = %s; if (r != 0x%06x) return r; R = cpu.cur; %s' % (savef(exit_flags(target)), ret, callee, ret, loadf(lo))
                L.append('if (%s) { %s }' % (cond, c) if cond else c)
            else:
                L.append('%s if (cpu.pc != 0x%06x) { r = ngp_dispatch(cpu.pc); if (r != 0x%06x) return r; R = cpu.cur; %s }' % (step(), fall, ret, loadf(lo)))
        if ft and nxt != fall: L.append(goto(fall))
        lines.append((a, L, T.fmt(d)))
    out = ['uint32_t %s(void)\n{\n    uint32_t *R = cpu.cur, r, t, xa, xb, xc, xr, xe; unsigned fS = 0, fZ = 0, fH = 0, fV = 0, fN = 0, fC = 0;\n'
           '    (void)r; (void)t; (void)xa; (void)xb; (void)xc; (void)xr; (void)xe; (void)R;\n    %s' % (fname(entry), loadf(live_in.get(entry, ALLF)))]
    if addrs and addrs[0] != entry: labels.add(entry); out.append('    goto L_%06x;' % entry)
    for a, L, text in lines:
        out.append('%s%s%s' % ('L_%06x: ' % a if a in labels else '    ', ' '.join(x for x in L if x).ljust(60), '   /* %06x %s */' % (a, text)))
    out.append('    (void)fS; (void)fZ; (void)fH; (void)fV; (void)fN; (void)fC;\n}\n')
    return '\n'.join(out), len(lines)

HDR = '''/* generated by tools/recomp900.py from the game's ROM: do not edit */
#include "ngp.h"
#define CPU900_EXTERN
#include "cpu900.h"
#include "gen900.h"
#include "logic.h"
#include "gen_code.h"
'''

os.makedirs(outdir, exist_ok=True)
for f in os.listdir(outdir):
    if f.startswith('gen_') and (f.endswith('.c') or f.endswith('.h')): os.remove(os.path.join(outdir, f))
all_waits = {}; step_forms = collections.Counter()
order = sorted(entries)
rounds = solve_in_use()
files, cur, n = [], [], 0
for e in order:
    text, k = gen_function(e)
    cur.append(text); n += k
    if n >= 2500: files.append(cur); cur, n = [], 0
if cur or not files: files.append(cur)
for i, texts in enumerate(files):
    with open(os.path.join(outdir, 'gen_%02d.c' % i), 'w') as f:
        f.write(HDR + '\n' + '\n'.join(texts))
with open(os.path.join(outdir, 'gen_code.h'), 'w') as f:
    f.write('/* generated by tools/recomp900.py: do not edit */\n#include <stdint.h>\nuint32_t ngp_dispatch(uint32_t pc);\n')
    f.write('\n'.join('uint32_t %s(void);' % fname(e) for e in order) + '\n')
    ops_in_header = set(ops_needed)
    f.write('\n'.join('uint32_t op_%06x(void);' % a for a in sorted(ops_needed)) + '\n')
with open(os.path.join(outdir, 'gen_dispatch.c'), 'w') as f:
    f.write(HDR + '\n/* run the code at pc until it returns: gives the address where execution goes on */\n'
            'uint32_t ngp_dispatch(uint32_t pc)\n{\n    if (pc >= 0xff0000) return ngp_bios_entry(pc);\n    switch (pc) {\n')
    for e in order: f.write('    case 0x%06x: return %s();\n' % (e, fname(e)))
    f.write('    }\n    ngp_fatal(pc, 2);\n    return 0;\n}\n')
with open(os.path.join(outdir, 'gen_waits.h'), 'w') as f:
    f.write('/* generated by tools/recomp900.py: the branches back of the loops where the game waits (1 = the frame ends, 2 = poll) */\n'
            'static const struct { uint32_t pc; uint8_t kind; } gen_waits[] = {\n')
    for a in sorted(all_waits): f.write('    { 0x%06x, %d },\n' % (a, all_waits[a]))
    f.write('    { 0, 0 }\n};\n')
with open(os.path.join(outdir, 'gen_game.h'), 'w') as f:
    f.write('/* generated by tools/recomp900.py from tools/games/%s.py: do not edit */\n#define NGP_GEN_FILES %d\n'
            % (gamename or '(none)', len(files)))

if os.environ.get('NGP_NO68K') != '1':          # the 68k back end for the Amiga (tools/back900.py)
    import back900
    back900.generate(globals(), os.path.join(outdir, 'gen68k.s'))

with open(os.path.join(outdir, 'gen_ops.c'), 'w') as f:      # after both back ends: what either asked for
    f.write(HDR + '\n/* instructions with no inline form (see op_body in tools/recomp900.py): the next pc */\n')
    for a in sorted(ops_needed):
        f.write('uint32_t op_%06x(void)   /* %s */\n{\n    uint32_t *R = cpu.cur, xa, xe; (void)R; (void)xa; (void)xe;\n    %s\n}\n'
                % (a, T.fmt(insn(a)), '\n    '.join(ops_needed[a])))
    gen_ops_count = len(ops_needed)
with open(os.path.join(outdir, 'gen_code.h'), 'a') as f:
    f.write('\n'.join('uint32_t op_%06x(void);' % a for a in sorted(ops_needed) if a not in ops_in_header) + '\n')

decoded = set()
for e in order: decoded |= set(body(e))
missing = sorted(a for a in executed if in_rom(a) and a not in decoded)
total = stats['native'] + stats['step']
print('entries %d, instructions %d in %d files (seen in the traces: %d, traced but not translated: %d), wait loops %d (%d frame, %d poll)'
      % (len(order), len(decoded), len(files), len(executed), len(missing), len(all_waits),
         sum(1 for k in all_waits.values() if k == 1), sum(1 for k in all_waits.values() if k == 2)))
print('non-flow instructions: %d inline, %d as op_ functions (%.0f%%; %d in gen_ops.c for both back ends); flags settled in %d rounds' % (stats['native'], stats['step'], 100.0 * stats['step'] / max(total, 1), len(ops_needed), rounds))
if step_forms: print('  most frequent as op_ functions:', ' '.join('%s:%d' % kv for kv in step_forms.most_common(14)))
if missing: print('  traced but not translated, first:', ' '.join('%06x' % a for a in missing[:12]))
if ram_code: print('  WARNING: the traces executed code outside the cartridge (in RAM): %d addresses from %06x. Not translated.' % (len(ram_code), ram_code[0]))
