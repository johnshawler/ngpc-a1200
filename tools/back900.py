"""back900.py - the 68020 assembly back end of tools/recomp900.py (host only).

The C back end (src/ngpc/gen_*.c) is what the host test runs and what tools/verify_ngpc.sh proves against the
interpreter; this one writes src/ngpc/gen68k.s for the Amiga, from the same decoding, the same functions and the
same flag liveness. The Amiga's checksum against the host's shows the two agree (tools/debug_ngpc.sh finds the
first tick that does not).

Registers while translated code runs (the whole program shares them; nothing is saved at a call):
  d4 d5 d6 d7 = XWA XBC XDE XHL of the current bank      a2 a3 a4 a5 = XIX XIY XIZ XSP
  d3 = the flags, in the 68k condition code order: C bit 0, V 1, Z 2, S 3 (so "move.w ccr,d3" captures them),
       H bit 5, N bit 6 (the TLCS-900's subtract flag)
  a6 = ngp_ram - $4000 (a TLCS address A in RAM is (a6,A.l))
  d0 d1 d2 a0 a1 = scratch; d2 survives the memory routines and calls into C
A function is a 68k subroutine that returns the TLCS address where execution goes on, in d0. CALL pushes its
return address on the TLCS stack (in the game's RAM), calls, and compares d0 with it: another address means the
stack was manipulated, and it is returned upward (as in the C back end). Registers go through asm_regs to the CPU
state (src/ngpc/run68k.c) only where C runs: the interpreter's step, waits, BIOS calls, unknown addresses.
Forms this back end has no code for run through the interpreter's step (SYNC, asm_step, SYNC).
"""
import os, collections, re
import tlcs900 as T

FS, FZ, FH, FV, FN, FC = 0x80, 0x40, 0x10, 0x04, 0x02, 0x01
ALLF = 0xd7
D3BIT = {FC: 0, FV: 1, FZ: 2, FS: 3, FH: 5, FN: 6}
CCRF = FS | FZ | FV | FC                                    # the flags the 68k condition codes can carry
def d3mask(m): return sum(1 << D3BIT[f] for f in D3BIT if m & f)
SZ = {1: 'b', 2: 'w', 4: 'l'}
MASK = {1: 0xff, 2: 0xffff, 4: 0xffffffff}
CC68 = {1: 'lt', 9: 'ge', 2: 'le', 10: 'gt', 3: 'ls', 11: 'hi', 4: 'vs', 12: 'vc', 5: 'mi', 13: 'pl', 6: 'eq', 14: 'ne',
        7: 'cs', 15: 'cc'}
NEGCC = {'lt': 'ge', 'ge': 'lt', 'le': 'gt', 'gt': 'le', 'ls': 'hi', 'hi': 'ls', 'vs': 'vc', 'vc': 'vs', 'mi': 'pl',
         'pl': 'mi', 'eq': 'ne', 'ne': 'eq', 'cs': 'cc', 'cc': 'cs'}
SINGLE = {4: (1, 1), 12: (1, 0), 5: (3, 1), 13: (3, 0), 6: (2, 1), 14: (2, 0), 7: (0, 1), 15: (0, 0)}   # cc: (d3 bit, set?)
ALU_OPS = ('add', 'adc', 'sub', 'sbc', 'and', 'xor', 'or', 'cp')
SHIFT68 = {'sla': 'lsl', 'sll': 'lsl', 'srl': 'lsr', 'sra': 'asr', 'rlc': 'rol', 'rrc': 'ror'}

class NoNative(Exception): pass

def regloc(code):
    if 0xe0 <= code < 0xf0: return 'd%d' % (4 + ((code >> 2) & 3))
    if code >= 0xf0: return 'a%d' % (2 + ((code >> 2) & 3))
    return None

def bankoff(code):
    """a register of a fixed bank (codes 0x00-0x3F): the offset of its long in cpu.bank, or None"""
    return (code >> 4) * 16 + ((code >> 2) & 3) * 4 if code < 0x40 else None
# The CPU state (struct Cpu900 in cpu900.h, checked in run68k.c): bank[4][4] at 0, cur (= &bank[RFP]) at 80
CPU_CUR = 80
BANKOUT = ['move.l _cpu+%d,a0' % CPU_CUR, 'movem.l d4-d7,(a0)']             # the current bank to the CPU state
BANKIN = ['move.l _cpu+%d,a0' % CPU_CUR, 'movem.l (a0),d4-d7']              # and back: that bank may be the one written

# ---- memory at addresses known when translating ----
RE_ADDR = re.compile(r'^\tmove\.l #\$([0-9a-f]+),(d0|d2)$')
RE_MEM = re.compile(r'^\tbsr (rd|wr)(8|16|32)$')
SWAP_LE = ['ror.w #8,d1', 'swap d1', 'ror.w #8,d1']

def mem_direct(kind, n, a):
    """68k lines for a read (value to d1) or write (value in d1) of n bytes at the fixed address a, or None (the routine)"""
    if kind == 'rd' and 0x4000 <= a and a + n <= 0x8000:            # work RAM and the shared RAM: no side effects
        if n == 1: return ['moveq #0,d1', 'move.b %d(a6),d1' % a]
        if n == 2: return ['moveq #0,d1', 'move.w %d(a6),d1' % a, 'ror.w #8,d1']
        return ['move.l %d(a6),d1' % a] + SWAP_LE
    if kind == 'rd' and 0x200000 <= a and a + n <= 0x400000:        # the cartridge
        o = a - 0x200000
        if n == 1: return ['move.l _ngp_rom,a0', 'moveq #0,d1', 'move.b (%d,a0),d1' % o]
        if n == 2: return ['move.l _ngp_rom,a0', 'moveq #0,d1', 'move.w (%d,a0),d1' % o, 'ror.w #8,d1']
        return ['move.l _ngp_rom,a0', 'move.l (%d,a0),d1' % o] + SWAP_LE
    if kind == 'wr' and 0x4000 <= a and a + n <= 0x7000:            # work RAM (the shared RAM wakes the sound processor)
        if n == 1: return ['move.b d1,%d(a6)' % a]
        if n == 2: return ['ror.w #8,d1', 'move.w d1,%d(a6)' % a, 'ror.w #8,d1']
        return SWAP_LE + ['move.l d1,%d(a6)' % a] + SWAP_LE
    if kind == 'wr' and 0x8800 <= a and a + n <= 0xc000 and n < 4:   # maps, sprites, characters: vidw marks the change
        if n == 1: return ['move.l #$%x,d0' % (a - 0x8000), 'bsr vidw']
        return ['move.l #$%x,d0' % (a - 0x8000), 'bsr vidw', 'move.l #$%x,d0' % (a - 0x7fff), 'ror.w #8,d1', 'bsr vidw', 'ror.w #8,d1']
    return None

# a memory routine's work-RAM case, inline: d0 = the address (a value with bits 24-31 set takes the routine), d1 = the
# value (zero-extended for reads); the routine itself (out of line, at 'slow') does everything else
def _rd_inline(n, last):
    def f(slow):
        return ['move.l d0,d1', 'sub.l #$4000,d1', 'cmp.l #$%x,d1' % last, 'bhs %s' % slow] + {
            1: ['moveq #0,d1', 'move.b (a6,d0.l),d1'], 2: ['moveq #0,d1', 'move.w (a6,d0.l),d1', 'ror.w #8,d1'],
            4: ['move.l (a6,d0.l),d1'] + SWAP_LE}[n]
    return f
def _wr_inline(n, last):
    def f(slow):
        return ['move.l d0,a0', 'sub.l #$4000,a0', 'cmp.l #$%x,a0' % last, 'bhs %s' % slow] + {
            1: ['move.b d1,(a6,d0.l)'], 2: ['ror.w #8,d1', 'move.w d1,(a6,d0.l)', 'ror.w #8,d1'],
            4: SWAP_LE + ['move.l d1,(a6,d0.l)'] + SWAP_LE}[n]
    return f
RAM_INLINE = {('rd', 1): _rd_inline(1, 0x4000), ('rd', 2): _rd_inline(2, 0x3fff), ('rd', 4): _rd_inline(4, 0x3ffd),
              ('wr', 1): _wr_inline(1, 0x3000), ('wr', 2): _wr_inline(2, 0x2fff), ('wr', 4): _wr_inline(4, 0x2ffd)}

def static_mem(L):
    """peephole over one function: a memory routine called with an address loaded as a constant becomes a direct
    access. 'move.l #$a,d0' ... 'bsr rd8' (lines between that do not touch d0), and the read-modify-write form
    'move.l #$a,d0', 'move.l d0,d2', 'bsr rd8' ... 'move.l d2,d0', 'bsr wr8' (d2 untouched between). The code after a
    memory routine never counts on d0 (the routines' slow paths change it)."""
    out, stubs = [], []
    d0 = d2 = None                  # constant addresses in d0 and d2, if known
    d0_at = d2_at = None            # the lines that loaded them
    for line in L:
        m = RE_MEM.match(line)
        if m:
            n = int(m.group(2)) // 8
            if d0 is not None and mem_direct(m.group(1), n, d0) is not None:
                if d2_at is not None and d2_at > d0_at and d2 == d0: out[d2_at] = '\tmove.l #$%x,d2' % d0
                out[d0_at] = None
                out.extend('\t' + x for x in mem_direct(m.group(1), n, d0))
            elif d0 is None and d2 is not None and out and out[-1] == '\tmove.l d2,d0' and mem_direct(m.group(1), n, d2) is not None:
                out.pop(); out.extend('\t' + x for x in mem_direct(m.group(1), n, d2))
            else:                                                   # work RAM inline, the routine out of line
                k = len(stubs); slow, back = '.m%d' % k, '.m%db' % k
                out.extend('\t' + x for x in RAM_INLINE[m.group(1), n](slow))
                out.append(back + ':')
                stubs += [slow + ':', line, '\tbra %s' % back]
            d0 = d0_at = None                                       # the routines keep d2
            continue
        a = RE_ADDR.match(line)
        if a:
            out.append(line)
            if a.group(2) == 'd0': d0, d0_at = int(a.group(1), 16), len(out) - 1
            else: d2, d2_at = int(a.group(1), 16), None
            continue
        if line == '\tmove.l d0,d2' and d0 is not None:
            out.append(line); d2, d2_at = d0, len(out) - 1; continue
        if line.lstrip().startswith(';'): out.append(line); continue  # comments
        if not line.startswith('\t') or 'SYNC' in line or re.match(r'^\t(bsr|jsr|bra|jmp|rts|b[a-z][a-z](\.[sbwl])?|db[a-z]+)\s', line + ' '):
            if not re.match(r'^\tbsr (vidw|push\d+|pop\d+)$', line): d2 = d2_at = None
            d0 = d0_at = None                                       # labels, branches, calls
            out.append(line); continue
        if re.search(r'\bd0\b', line): d0 = d0_at = None
        if re.search(r'\bd2\b', line): d2 = d2_at = None
        out.append(line)
    return [x for x in out if x is not None] + stubs

def rot_to_low(size, code, r):
    """lines that bring the part of r a register code names to the low byte or word of r, and lines that undo it"""
    if size == 4: return [], []
    if size == 2: return (['swap %s' % r], ['swap %s' % r]) if code & 2 else ([], [])
    k = code & 3
    return {0: ([], []), 1: (['ror.w #8,%s' % r], ['rol.w #8,%s' % r]), 2: (['swap %s' % r], ['swap %s' % r]),
            3: (['rol.l #8,%s' % r], ['ror.l #8,%s' % r])}[k]

class Fn(object):
    """68k code of one function"""
    def __init__(self, g, entry):
        self.g = g; self.entry = entry; self.L = []; self.ccr = 0; self.nlab = 0

    def emit(self, *lines):
        for l in lines:
            if l: self.L.append('\t' + l)
        if lines: self.ccr = 0

    def label(self):
        self.nlab += 1; return '.t%d' % self.nlab

    # ---- operands ----
    def src(self, o, size, tmp):
        """(lines, operand) of a source value of this size; tmp: a data register that may be used"""
        k = o[0]
        if k == 'i': return [], '#$%x' % (o[1] & MASK[size])
        if k == 'r':
            loc = regloc(o[2])
            if loc is None and bankoff(o[2]) is not None:
                pre, _ = rot_to_low(size, o[2], tmp)
                return ['move.l _cpu+%d,%s' % (bankoff(o[2]), tmp)] + pre, tmp
            if loc is None: raise NoNative('previous-bank register')
            if size == 4: return [], loc
            pre, _ = rot_to_low(size, o[2], tmp)
            if loc[0] == 'd' and not pre: return [], loc
            return ['move.l %s,%s' % (loc, tmp)] + pre, tmp
        if k == 'm':
            lines = self.ea(o)
            return lines + ['bsr rd%d' % (size * 8)] + ([] if tmp == 'd1' else ['move.l d1,%s' % tmp]), tmp
        raise NoNative('operand %s' % k)

    def ea(self, o):
        """lines that leave the address of a memory operand in d0"""
        m = o[1]
        if m in ('abs', 'pcrel'): return ['move.l #$%x,d0' % o[2]]
        loc = regloc(o[2])
        if loc is None and bankoff(o[2]) is not None and m in ('ind', 'disp', 'idx'):
            first = ['move.l _cpu+%d,d0' % bankoff(o[2])]
            if m == 'ind': return first
            if m == 'disp': return first + ['add.l #%d,d0' % o[3]]
            pre, x = self.src(('r', o[4], o[3]), o[4], 'd1')
            if x != 'd1': pre = pre + ['move.l %s,d1' % x]
            return pre + (['ext.w d1', 'ext.l d1'] if o[4] == 1 else ['ext.l d1']) + first + ['add.l d1,d0']
        if loc is None: raise NoNative('previous-bank address register')
        if m == 'ind': return ['move.l %s,d0' % loc]
        if m == 'disp':
            if loc[0] == 'a' and -32768 <= o[3] <= 32767: return ['lea %d(%s),a0' % (o[3], loc), 'move.l a0,d0']
            return ['move.l %s,d0' % loc, 'add.l #%d,d0' % o[3]]
        if m == 'idx':
            pre, x = self.src(('r', o[4], o[3]), o[4], 'd1')
            ext = ['ext.w %s' % x, 'ext.l %s' % x] if o[4] == 1 else ['ext.l %s' % x]
            if x != 'd1': pre = pre + ['move.l %s,d1' % x]; ext = ['ext.w d1', 'ext.l d1'] if o[4] == 1 else ['ext.l d1']
            return pre + ext + ['add.l %s,d1' % loc, 'move.l d1,d0']
        if m == 'dec': return ['subq.l #%d,%s' % (o[3], loc), 'move.l %s,d0' % loc]
        if m == 'inc': return ['move.l %s,d0' % loc, 'addq.l #%d,%s' % (o[3], loc)]
        raise NoNative('mode %s' % m)

    def dst(self, o, size):
        """(pre, operand, post, is_memory) for an operand that is read, changed and written back"""
        if o[0] == 'r':
            loc = regloc(o[2])
            if loc is None and bankoff(o[2]) is not None:
                pre, post = rot_to_low(size, o[2], 'd2')
                return (['move.l _cpu+%d,d2' % bankoff(o[2])] + pre, 'd2', post + ['move.l d2,_cpu+%d' % bankoff(o[2])], False)
            if loc is None: raise NoNative('previous-bank register')
            if loc[0] == 'd':
                pre, post = rot_to_low(size, o[2], loc)
                return pre, loc, post, False
            pre, post = rot_to_low(size, o[2], 'd2')
            return ['move.l %s,d2' % loc] + pre, 'd2', post + ['move.l d2,%s' % loc], False
        if o[0] == 'm':
            return self.ea(o) + ['move.l d0,d2', 'bsr rd%d' % (size * 8)], 'd1', ['move.l d2,d0', 'bsr wr%d' % (size * 8)], True
        raise NoNative('destination %s' % o[0])

    def safe_src(self, o, size, tmp, pre):
        """src(), but a register the destination's rotation (pre) would move is copied first"""
        lines, S = self.src(o, size, tmp)
        if pre and S[0] in 'da' and any(S in p for p in pre) and S != tmp:
            lines = lines + ['move.l %s,%s' % (S, tmp)]; S = tmp
        return lines, S

    def put(self, o, size, val):
        """lines that store the value in data register val (not d0, d1 or d2 for memory) into an operand"""
        if o[0] == 'r':
            pre, D, post, _ = self.dst(o, size)
            return pre + ['move.%s %s,%s' % (SZ[size], val, D)] + post
        lines = self.ea(o)
        return lines + ([] if val == 'd1' else ['move.l %s,d1' % val]) + ['bsr wr%d' % (size * 8)]

    # ---- flags ----
    def capture(self, need, live, defs, from_ccr, sets=0, clears=0):
        """after a 68k instruction: put the TLCS flags 'need' (those of 'defs' read later) into d3. from_ccr: flags the
        68k condition codes hold correctly now; sets/clears: flags that are 1 or 0 after this instruction."""
        if not need: return
        keep = live & ~defs                                 # flags that pass through this instruction
        m = need & from_ccr
        out = []
        if not keep & ~need:                                # nothing to keep: the whole of d3 may be replaced
            out.append('move.w ccr,d3')                     # (bits 5 and 6, H and N, become 0)
            for f in (FH, FN):
                if need & f & sets: out.append('bset #%d,d3' % D3BIT[f])
            self.L += ['\t' + x for x in out]
            self.ccr = m if len(out) == 1 else 0
            return
        if m: out += ['move.w ccr,d0', 'and.b #$%02x,d0' % d3mask(m)]
        clr = d3mask(need & ~from_ccr & clears) | d3mask(m)
        st = d3mask(need & ~from_ccr & sets)
        if clr: out.append('and.b #$%02x,d3' % (0xff & ~clr))
        if st: out.append('or.b #$%02x,d3' % st)
        if m: out.append('or.b d0,d3')
        self.L += ['\t' + x for x in out]
        self.ccr = 0

    def parity(self, size, reg):
        """V = an even number of bits set in the low byte or word of reg"""
        self.emit('move.l %s,d0' % reg if reg != 'd0' else '', 'bsr par%d' % (size * 8))

    def branch(self, cc, target, label_here):
        """lines that jump to target when TLCS condition cc holds (flags from the CCR if it still has them)"""
        if cc == 8: return ['bra %s' % target]
        if cc == 0: return []
        uses = self.g['CC_USES'][cc]
        if not label_here and uses and (uses & ~self.ccr) == 0: return ['b%s %s' % (CC68[cc], target)]
        if cc in SINGLE:
            bit, pos = SINGLE[cc]
            return ['btst #%d,d3' % bit, '%s %s' % ('bne' if pos else 'beq', target)]
        return ['move.w d3,ccr', 'b%s %s' % (CC68[cc], target)]

    def cond_set(self, cc, reg):
        """reg (a data register, low byte) = 1 when cc holds, else 0"""
        if cc in SINGLE:
            bit, pos = SINGLE[cc]
            return ['btst #%d,d3' % bit, '%s %s' % ('sne' if pos else 'seq', reg), 'neg.b %s' % reg]
        if cc in (0, 8): return ['moveq #%d,%s' % (cc == 8, reg)]
        return ['move.w d3,ccr', 's%s %s' % (CC68[cc], reg), 'neg.b %s' % reg]

def generate(g, path):
    insn, entries, liveness, flags_ud = g['insn'], g['entries'], g['liveness'], g['flags_ud']
    ind_jump, dyn_targets, fname, NATIVE = g['ind_jump'], g['dyn_targets'], g['fname'], g['NATIVE']
    stats = collections.Counter(); stepped = collections.Counter()

    def native(F, d, live):
        """68k code of one non-flow instruction into F, or NoNative"""
        m, ops, s = d.mnem, d.ops, d.size
        kinds = tuple(o[0] for o in ops)
        r, defs = flags_ud(d)
        need = live & defs
        if not NATIVE: raise NoNative('all through the step')
        if m == 'nop': return
        if m in ('ei', 'di'):
            F.emit('pea %d.w' % (7 if m == 'di' else ops[0][1] & 7), 'jsr _asm_ei', 'addq.l #4,sp'); return
        if m in ('scf', 'rcf', 'ccf', 'zcf'):
            if live & FC:
                if m == 'scf': F.emit('bset #0,d3')
                elif m == 'rcf': F.emit('bclr #0,d3')
                elif m == 'ccf': F.emit('bchg #0,d3')
                else: F.emit('btst #2,d3', 'seq d0', 'and.b #1,d0', 'and.b #$fe,d3', 'or.b d0,d3')
            if live & FH and m in ('scf', 'rcf'): F.emit('bclr #5,d3')
            if live & FN: F.emit('bclr #6,d3')
            return
        if any(k in ('sr', 'f', 'f2', 'cr', 'a') for k in kinds): raise NoNative('special register')
        if m == 'ld' and len(ops) == 2:
            if kinds[0] == 'r' and kinds[1] in ('r', 'i'):
                loc = regloc(ops[0][2])
                if loc is None and bankoff(ops[0][2]) is None: raise NoNative('previous-bank register')
                if loc and s == 4 and kinds[1] == 'i' and loc[0] == 'd' and -128 <= (ops[1][1] if ops[1][1] < 0x80000000 else ops[1][1] - 0x100000000) <= 127:
                    v = ops[1][1] if ops[1][1] < 0x80000000 else ops[1][1] - 0x100000000
                    F.emit('moveq #%d,%s' % (v, loc)); return
                if loc and s == 4 and loc[0] == 'a':
                    pre, S = F.src(ops[1], 4, 'd1'); F.emit(*(pre + ['move.l %s,%s' % (S, loc)])); return
                if loc is None and s == 4:
                    pre, S = F.src(ops[1], 4, 'd1'); F.emit(*(pre + ['move.l %s,_cpu+%d' % (S, bankoff(ops[0][2]))])); return
                pre, D, post, _ = F.dst(ops[0], s)
                pre_s, S = F.safe_src(ops[1], s, 'd1', pre)
                F.emit(*(pre_s + pre + ['move.%s %s,%s' % (SZ[s], S, D)] + post)); return
            if kinds == ('r', 'm'):
                pre_s, S = F.src(ops[1], s, 'd1')
                loc = regloc(ops[0][2])
                if s == 4 and loc and loc[0] == 'a': F.emit(*(pre_s + ['move.l %s,%s' % (S, loc)])); return
                pre, D, post, _ = F.dst(ops[0], s)
                F.emit(*(pre_s + pre + ['move.%s %s,%s' % (SZ[s], S, D)] + post)); return
            if kinds[0] == 'm' and kinds[1] in ('r', 'i'):
                pre_v, V = F.src(ops[1], s, 'd1')
                lines = F.ea(ops[0])
                F.emit(*(lines + pre_v + ([] if V == 'd1' else ['move.%s %s,d1' % (SZ[s], V)]) + ['bsr wr%d' % (s * 8)])); return
            if kinds == ('m', 'm'):
                F.emit(*(F.ea(ops[1]) + ['bsr rd%d' % (s * 8), 'move.l d1,d2'] + F.ea(ops[0]) + ['move.l d2,d1', 'bsr wr%d' % (s * 8)])); return
        if m == 'lda' and kinds == ('r', 'm') and ops[1][1] not in ('dec', 'inc'):
            lines = F.ea(ops[1]); loc = regloc(ops[0][2])
            if ops[0][1] == 4 and loc: F.emit(*(lines + ['move.l d0,%s' % loc])); return
            F.emit(*(lines + F.put(ops[0], 2, 'd0'))); return
        if m == 'push' and kinds in (('r',), ('i',), ('m',)):
            sz = ops[0][1] if kinds == ('r',) else s
            pre, V = F.src(ops[0], sz, 'd1')
            F.emit(*(pre + ([] if V == 'd1' else ['move.l %s,d1' % V if V[0] in 'da' else 'move.%s %s,d1' % (SZ[sz], V)]) + ['bsr push%d' % (sz * 8)])); return
        if m == 'pop' and kinds == ('r',):
            sz = ops[0][1]; loc = regloc(ops[0][2])
            if sz == 4 and loc: F.emit('bsr pop32', 'move.l d1,%s' % loc); return
            F.emit('bsr pop%d' % (sz * 8), 'move.l d1,d0', *F.put(ops[0], sz, 'd0')); return
        if m == 'pop' and kinds == ('m',):
            F.emit('bsr pop%d' % (s * 8), 'move.l d1,d2', *(F.ea(ops[0]) + ['move.l d2,d1', 'bsr wr%d' % (s * 8)])); return
        if m in ALU_OPS and len(ops) == 2 and kinds[1] in ('r', 'i', 'm') and kinds[0] in ('r', 'm'):
            alu(F, m, s, ops[0], ops[1], live, defs, need); return
        if m == 'neg' and kinds == ('r',):
            sz = ops[0][1]
            if need & FH and sz < 4:
                pre_s, S = F.src(ops[0], sz, 'd1')
                F.emit(*(pre_s + (['move.l %s,d1' % S] if S != 'd1' else []) + ['moveq #0,d0', 'bsr sub%d_f' % (sz * 8)]))
                F.emit(*F.put(ops[0], sz, 'd0')); return
            pre, D, post, _ = F.dst(ops[0], sz)
            F.emit(*(pre + ['neg.%s %s' % (SZ[sz], D)]))
            F.capture(need, live, defs, CCRF, sets=FN, clears=FH if sz == 4 else 0)
            F.emit(*post); return
        if m == 'cpl' and kinds == ('r',):
            sz = ops[0][1]; pre, D, post, _ = F.dst(ops[0], sz)
            F.emit(*(pre + ['not.%s %s' % (SZ[sz], D)] + post))
            if need & FH: F.emit('bset #5,d3')
            if need & FN: F.emit('bset #6,d3')
            return
        if m in ('inc', 'dec') and kinds in (('i', 'r'), ('i', 'm')):
            n = ops[0][1]; o = ops[1]; sz = o[1] if kinds[1] == 'r' else s
            op = 'addq' if m == 'inc' else 'subq'
            if kinds[1] == 'r' and sz > 1:                  # a word or long register: no flags
                loc = regloc(o[2])
                if loc and loc[0] == 'a' and sz == 4: F.emit('%s.l #%d,%s' % (op, n, loc)); return
                pre, D, post, _ = F.dst(o, sz)
                F.emit(*(pre + ['%s.%s #%d,%s' % (op, SZ[sz], n, D)] + post)); return
            if need & FH:
                pre, D, post, mem = F.dst(o, sz)
                F.emit(*(pre + ['move.l %s,d0' % D, 'moveq #%d,d1' % n, 'bsr %s%d_f' % ('inc' if m == 'inc' else 'dec', sz * 8),
                                'move.%s d0,%s' % (SZ[sz], D)] + post)); return
            pre, D, post, _ = F.dst(o, sz)
            F.emit(*(pre + ['%s.%s #%d,%s' % (op, SZ[sz], n, D)]))
            F.capture(need, live, defs, FS | FZ | FV, sets=FN if m == 'dec' else 0, clears=FN if m == 'inc' else 0)
            F.emit(*post); return
        if m in SHIFT68 and kinds in (('i', 'r'), ('m',)):
            if kinds == ('m',): o, sz, n = ops[0], s, 1
            else: o, sz, n = ops[1], ops[1][1], ops[0][1] or 16
            pre, D, post, mem = F.dst(o, sz)
            lines = pre + (['%s.%s #%d,%s' % (SHIFT68[m], SZ[sz], n, D)] if n <= 8 else ['moveq #%d,d0' % n, '%s.%s d0,%s' % (SHIFT68[m], SZ[sz], D)])
            F.emit(*lines)
            F.capture(need, live, defs, FS | FZ | FC | (FV if sz == 4 else 0), clears=FH | FN)
            if need & FV and sz < 4: F.parity(sz, D)
            F.emit(*post); return
        if m in ('extz', 'exts') and kinds == ('r',):
            sz = ops[0][1]; pre, D, post, _ = F.dst(ops[0], sz)
            line = ('and.w #$ff,%s' if sz == 2 else 'and.l #$ffff,%s') % D if m == 'extz' else ('ext.w %s' if sz == 2 else 'ext.l %s') % D
            F.emit(*(pre + [line] + post)); return
        if m == 'ex' and kinds == ('r', 'r'):
            a, b = regloc(ops[0][2]), regloc(ops[1][2])
            if a is None or b is None: raise NoNative('fixed-bank register')
            sz = ops[0][1]
            if sz == 4: F.emit('exg %s,%s' % (a, b)); return
            if sz == 2 and a == b and a[0] == 'd' and (ops[0][2] ^ ops[1][2]) & 2: F.emit('swap %s' % a); return
            pre_a, A = F.src(ops[0], sz, 'd0')
            if A != 'd0': pre_a.append('move.l %s,d0' % A)
            pre_b, B = F.src(ops[1], sz, 'd1')
            if B != 'd1': pre_b.append('move.l %s,d1' % B)
            F.emit(*(pre_a + pre_b + F.put(ops[0], sz, 'd1') + F.put(ops[1], sz, 'd0'))); return
        if m in ('bit', 'set', 'res', 'chg', 'tset') and kinds in (('i', 'r'), ('i', 'm')):
            bit = ops[0][1]
            if kinds[1] == 'r':
                sz = ops[1][1]; loc = regloc(ops[1][2])
                if bit >= sz * 8:
                    if m in ('bit', 'tset'):
                        if need & FZ: F.emit('bset #2,d3')
                        if need & FH: F.emit('bset #5,d3')
                        if need & FN: F.emit('bclr #6,d3')
                    return
                if loc is None:                             # a fixed bank's register, through memory
                    pre, D, post, _ = F.dst(ops[1], sz); F.emit(*pre)
                    if m in ('bit', 'tset'): F.emit('btst #%d,%s' % (bit, D)); F.capture(need, live, defs, FZ, sets=FH, clears=FN)
                    if m != 'bit': F.emit('%s #%d,%s' % ({'set': 'bset', 'tset': 'bset', 'res': 'bclr', 'chg': 'bchg'}[m], bit, D), *post)
                    return
                k = (ops[1][2] & 3) if sz == 1 else ((ops[1][2] >> 1) & 1) * 2 if sz == 2 else 0
                nb = bit + 8 * k
                R = loc
                if loc[0] == 'a': F.emit('move.l %s,d2' % loc); R = 'd2'
                if m in ('bit', 'tset'):
                    F.emit('btst #%d,%s' % (nb, R)); F.capture(need, live, defs, FZ, sets=FH, clears=FN)
                if m != 'bit': F.emit('%s #%d,%s' % ({'set': 'bset', 'tset': 'bset', 'res': 'bclr', 'chg': 'bchg'}[m], nb, R))
                if loc[0] == 'a' and m != 'bit': F.emit('move.l d2,%s' % loc)
                return
            F.emit(*(F.ea(ops[1]) + ['move.l d0,d2', 'bsr rd8']))
            if m in ('bit', 'tset'):
                F.emit('btst #%d,d1' % (bit & 7)); F.capture(need, live, defs, FZ, sets=FH, clears=FN)
            if m != 'bit': F.emit('%s #%d,d1' % ({'set': 'bset', 'tset': 'bset', 'res': 'bclr', 'chg': 'bchg'}[m], bit & 7), 'move.l d2,d0', 'bsr wr8')
            return
        if m == 'scc' and kinds == ('cc', 'r'):
            sz = ops[1][1]
            F.emit('moveq #0,d0', *F.cond_set(ops[0][1], 'd0'))
            F.emit(*F.put(ops[1], sz, 'd0')); return
        if m in ('mul', 'muls') and len(ops) == 2 and ops[0][0] == 'r' and s in (1, 2):
            dsz = ops[0][1]
            pre, D, post, _ = F.dst(ops[0], dsz)
            pre_s, S = F.safe_src(ops[1], s, 'd1', pre)
            if S != 'd1': pre_s.append('move.l %s,d1' % S)
            F.emit(*pre_s)
            if s == 1:
                if m == 'mul': F.emit(*(pre + ['move.w %s,d0' % D, 'and.w #$ff,d0', 'and.w #$ff,d1', 'mulu.w d1,d0', 'move.w d0,%s' % D] + post))
                else: F.emit(*(pre + ['move.w %s,d0' % D, 'ext.w d0', 'ext.w d1', 'muls.w d1,d0', 'move.w d0,%s' % D] + post))
            else:
                F.emit(*(pre + ['%s.w d1,%s' % ('mulu' if m == 'mul' else 'muls', D)] + post))
            return
        if m in ('ldir', 'lddr') and s in (1, 2):
            dp, sp_ = regloc(ops[0][2]), regloc(ops[1][2]); step = s
            op = 'addq.l' if m == 'ldir' else 'subq.l'
            lab = F.label()
            F.emit('%s:' % lab)
            F.L[-1] = lab + ':'
            F.L += ['\t' + x for x in ['move.l %s,d0' % sp_, 'bsr rd%d' % (s * 8), 'move.l %s,d0' % dp, 'bsr wr%d' % (s * 8),
                                      '%s #%d,%s' % (op, step, sp_), '%s #%d,%s' % (op, step, dp), 'subq.w #1,d5', 'bne %s' % lab]]
            clr = d3mask(need & (FH | FV | FN))
            if clr: F.emit('and.b #$%02x,d3' % (0xff & ~clr))
            return
        raise NoNative('form %s %s' % (m, kinds))

    def fixed_banks(d):
        """the fixed banks whose registers the instruction names"""
        regs = []
        for o in d.ops:
            if o[0] == 'r': regs.append(o[2])
            elif o[0] == 'm' and o[1] in ('ind', 'disp', 'idx'):
                regs.append(o[2])
                if o[1] == 'idx': regs.append(o[3])
        return sorted(set(r >> 4 for r in regs if r < 0x40))

    def fixed_regs(d):
        """None, 'read' or 'written': does the instruction name a register of a fixed bank, and is that the one it writes"""
        regs = []
        for o in d.ops:
            if o[0] == 'r': regs.append(o[2])
            elif o[0] == 'm' and o[1] in ('ind', 'disp', 'idx', 'dec', 'inc'):
                regs.append(o[2])
                if o[1] == 'idx': regs.append(o[3])
                if o[1] in ('dec', 'inc') and o[2] < 0x40: return 'no'
        if not any(r < 0x40 for r in regs): return None
        m = d.mnem
        if m == 'ex': return 'no'
        if m in ('cp', 'bit', 'push', 'btst'): return 'read'
        w = d.ops[-1] if m in ('inc', 'dec', 'set', 'res', 'chg', 'tset', 'rlc', 'rrc', 'rl', 'rr', 'sla', 'sra', 'sll', 'srl') else d.ops[0]
        return 'written' if w[0] == 'r' and w[2] < 0x40 else 'read'

    def alu(F, m, s, do, so, live, defs, need):
        arith = m in ('add', 'adc', 'sub', 'sbc', 'cp')
        full = m in ('adc', 'sbc') or (need & FH and arith and s < 4)
        pre, D, post, mem = F.dst(do, s)
        if full:                                            # the subroutine computes the result and all six flags
            sub = {'add': 'add', 'adc': 'adc', 'sub': 'sub', 'sbc': 'sbc', 'cp': 'sub'}[m]
            if mem:                                         # the destination's value first (d1), then the source
                pre_s, S = F.src(so, s, 'd1')
                F.emit(*(pre + ['move.l d1,d0'] + pre_s + (['move.l %s,d1' % S] if S != 'd1' else []) + ['bsr %s%d_f' % (sub, s * 8)]))
            else:
                pre_s, S = F.safe_src(so, s, 'd1', pre)
                if S != 'd1': pre_s.append('move.l %s,d1' % S)
                F.emit(*(pre_s + pre + ['move.l %s,d0' % D, 'bsr %s%d_f' % (sub, s * 8)]))
            if m == 'cp': F.emit(*([] if mem else post))
            else: F.emit('move.%s d0,%s' % (SZ[s], D), *post)
            return
        if mem:
            pre_s, S = F.src(so, s, 'd0')
            lines = pre + pre_s
        else:
            pre_s, S = F.safe_src(so, s, 'd1', pre)
            lines = pre_s + pre
        if S[0] == 'a' and (m in ('and', 'or', 'xor') or s == 1):
            t = 'd0' if mem else 'd1'; lines.append('move.l %s,%s' % (S, t)); S = t
        op = {'add': 'add', 'sub': 'sub', 'cp': 'cmp', 'and': 'and', 'or': 'or', 'xor': 'eor'}[m]
        F.emit(*(lines + ['%s.%s %s,%s' % (op, SZ[s], S, D)]))
        logic = m in ('and', 'or', 'xor')
        F.capture(need, live, defs, (FS | FZ | FC | (FV if s == 4 else 0)) if logic else CCRF,
                  sets=(FH if m == 'and' else 0) | (FN if m in ('sub', 'cp') else 0),
                  clears=FN if m in ('add', 'and', 'or', 'xor') else (FH if s == 4 else 0) | (FH if m in ('or', 'xor') else 0))
        if logic and need & FV and s < 4: F.parity(s, D)
        if m == 'cp' and mem: return
        F.emit(*post)

    out = [HEADER]
    for e in sorted(entries):
        F = Fn(g, e)
        addrs, aset, waits, live_in, live_out = liveness(e)
        labels = set()
        body = []
        for idx, a in enumerate(addrs):
            d = insn(a)
            if not d: continue
            kind, cc, target = T.flow(d)
            if kind in ('jump', 'branch') and target is not None and target in aset: labels.add(target)
            if kind in ('jump', 'branch') and target is None:
                for t in ind_jump.get(a, ()):
                    if t in aset: labels.add(t)
            fall = (a + d.length) & 0xffffff
            if idx + 1 < len(addrs) and addrs[idx + 1] != fall and fall in aset: labels.add(fall)
        if addrs and addrs[0] != e: labels.add(e)
        F.L.append('F_%06x:' % e)
        if addrs and addrs[0] != e: F.L.append('\tbra .L%06x' % e)
        def goto(t):
            if t in aset: return ['bra .L%06x' % t]
            if t in entries: return ['bra F_%06x' % t]
            return ['move.l #$%06x,d0' % t, 'bra dispatch']
        def jcc(cc, t, here):
            """lines: jump to t when cc holds"""
            if t in aset: return F.branch(cc, '.L%06x' % t, here)
            if t in entries: return F.branch(cc, 'F_%06x' % t, here)
            skip = F.label()
            return F.branch(cc ^ 8, skip, here) + ['move.l #$%06x,d0' % t, 'bra dispatch', '%s:' % skip]
        def step(a, sync_flags=True):
            return ['SYNCOUT', 'pea $%06x' % a, 'jsr _asm_step', 'addq.l #4,sp', 'SYNCIN']
        runs, run = [], None                    # guarded runs of instructions that name registers by their bank
        for idx, a in enumerate(addrs):
            d = insn(a)
            nxt = addrs[idx + 1] if idx + 1 < len(addrs) else None
            here = a in labels
            if here: F.L.append('.L%06x:' % a); F.ccr = 0
            F.L.append('\t\t\t\t\t; %06x %s' % (a, T.fmt(d) if d else '?'))
            if not d:
                F.emit('SYNCOUT', 'pea 1.w', 'pea $%06x' % a, 'jsr _asm_fatal', 'addq.l #8,sp', 'SYNCIN', 'moveq #0,d0', 'rts'); continue
            fall = (a + d.length) & 0xffffff
            kind, cc, target = T.flow(d)
            lo = live_out[a]
            ft = True
            if here or kind is not None: run = None          # a guarded run ends at a label or any flow
            if kind is None:
                mark = len(F.L)
                try:
                    fixed = fixed_regs(d)
                    if fixed == 'no': raise NoNative('fixed bank with (-r) or (r+)')
                    if fixed:                               # registers named by their bank: in cpu.bank, unless that
                        F.ccr = 0                           # bank is the current one (in d4-d7): then out of line
                        native(F, d, lo)
                        ccr_after = F.ccr
                        body = F.L[mark:]; del F.L[mark:]
                        if any(not x.startswith('\t') for x in body): raise NoNative('labels in a banked instruction')
                        slow_body = ['\t' + x for x in BANKOUT] + body + (['\t' + x for x in BANKIN] if fixed == 'written' else [])
                        banks = fixed_banks(d)
                        between = F.L[run['back_at'] + 1:] if run else None
                        if run and F.L[run['back_at']] == run['back'] + ':' and all(x.startswith('\t') for x in between):
                            del F.L[run['back_at']:]        # one guard for the run: the bank cannot change inside it
                            for b in banks:
                                if b not in run['banks']:
                                    at = run['guard_at'] + 1 + 2 * len(run['banks'])
                                    F.L[at:at] = ['\tcmp.l #_cpu+%d,a0' % (16 * b), '\tbeq %s' % run['slow']]
                                    run['banks'].append(b)
                            F.L += between + body
                            run['body'] += between + slow_body
                        else:
                            run = {'slow': F.label(), 'back': F.label(), 'banks': list(banks), 'guard_at': len(F.L), 'body': slow_body}
                            F.L.append('\tmove.l _cpu+%d,a0' % CPU_CUR)
                            for b in banks: F.L += ['\tcmp.l #_cpu+%d,a0' % (16 * b), '\tbeq %s' % run['slow']]
                            F.L += body
                            runs.append(run)
                        F.L.append(run['back'] + ':'); run['back_at'] = len(F.L) - 1
                        F.ccr = ccr_after                   # BANKIN and the branch back keep the condition codes
                    else: native(F, d, lo)
                    stats['native'] += 1
                except NoNative as ex:
                    del F.L[mark:]
                    F.emit(*step(a)); stats['step'] += 1; stepped[d.mnem] += 1; run = None
            elif kind == 'halt':
                F.emit('SYNCOUT', 'pea $%06x' % fall, 'jsr _asm_wait', 'addq.l #4,sp', 'SYNCIN')
            elif kind == 'swi':
                if d.ops[0][1] == 1: F.emit('SYNCOUT', 'jsr _asm_swi', 'SYNCIN')
                else: F.emit('SYNCOUT', 'pea 1.w', 'pea $%06x' % a, 'jsr _asm_fatal', 'addq.l #8,sp', 'SYNCIN', 'moveq #0,d0', 'rts'); ft = False
            elif kind == 'ret':
                if d.mnem == 'ret' and NATIVE:
                    tail = ['bsr pop32', 'move.l d1,d0', 'and.l #$ffffff,d0', 'rts']
                else: tail = step(a) + ['rts']               # RETD and RETI: the step leaves the address in d0
                if cc is None: F.emit(*tail); ft = False
                else:
                    skip = F.label()
                    F.emit(*(F.branch(cc ^ 8, skip, here) + tail)); F.L.append(skip + ':')
            elif kind in ('jump', 'branch'):
                if a in waits and d.mnem == 'djnz':         # count down, and wait while the count is not 0
                    fn = '_asm_wait' if waits[a] == 1 else '_asm_poll'
                    o = d.ops[0]; sz = o[1]
                    pre, D, post, _ = F.dst(o, sz)
                    F.emit(*(pre + ['subq.%s #1,%s' % (SZ[sz], D), 'sne d0'] + post + ['tst.b d0']))
                    skip = F.label()
                    F.emit('beq %s' % skip, 'SYNCOUT', 'pea $%06x' % target, 'jsr %s' % fn, 'addq.l #4,sp', 'SYNCIN', *goto(target))
                    F.L.append(skip + ':')
                elif a in waits:
                    fn = '_asm_wait' if waits[a] == 1 else '_asm_poll'
                    skip = F.label()
                    F.emit(*(F.branch(cc ^ 8 if cc is not None else 0, skip, here) +
                             ['SYNCOUT', 'pea $%06x' % target, 'jsr %s' % fn, 'addq.l #4,sp', 'SYNCIN'] + goto(target)))
                    F.L.append(skip + ':')
                elif d.mnem == 'djnz':
                    o = d.ops[0]; sz = o[1]; loc = regloc(o[2])
                    if loc is None or sz == 4:
                        F.emit(*step(a)); F.emit('cmp.l #$%06x,d0' % fall, 'beq.s .d%06x' % a, *goto(target)); F.L.append('.d%06x:' % a)
                    elif loc[0] == 'd' and sz == 1 and o[2] & 3 == 1:   # B, W, D, H: bits 8-15
                        F.emit('sub.w #$100,%s' % loc, 'cmp.w #$100,%s' % loc)
                        tl = '.L%06x' % target if target in aset else None
                        if tl: F.emit('bhs %s' % tl)
                        else: skip = F.label(); F.emit('blo %s' % skip, *goto(target)); F.L.append(skip + ':')
                    else:
                        pre, D, post, _ = F.dst(o, sz)
                        F.emit(*(pre + ['subq.%s #1,%s' % (SZ[sz], D)]))
                        if post: F.emit('sne d0', *post); F.emit('tst.b d0')
                        tl = '.L%06x' % target if target in aset else None
                        if tl: F.emit('bne %s' % tl)
                        else: skip = F.label(); F.emit('beq %s' % skip, *goto(target)); F.L.append(skip + ':')
                elif target is not None:
                    if cc is None: F.emit(*goto(target)); ft = False
                    else: F.emit(*jcc(cc, target, here))
                else:                                       # through a register or memory: the traces' targets
                    try: lines = F.ea(d.ops[-1])
                    except NoNative: lines = None
                    if lines is None: F.emit(*step(a))
                    else:
                        if cc is not None: skip = F.label(); F.emit(*F.branch(cc ^ 8, skip, here))
                        F.emit(*(lines + ['and.l #$ffffff,d0']))
                    if cc is not None and lines is None: F.emit('cmp.l #$%06x,d0' % fall, 'beq .x%06x' % a)
                    for t in sorted(ind_jump.get(a, ())):
                        if t in aset: F.emit('cmp.l #$%06x,d0' % t, 'beq .L%06x' % t)
                        elif t in entries: F.emit('cmp.l #$%06x,d0' % t, 'beq F_%06x' % t)
                    F.emit('bra dispatch')
                    if cc is not None: F.L.append(('.x%06x:' % a) if lines is None else (skip + ':'))
                    else: ft = False
            elif kind == 'call':
                ret = fall
                if target is not None:
                    call = (['move.l #$%06x,d1' % ret, 'bsr push32'] + (['bsr F_%06x' % target] if target in entries else ['move.l #$%06x,d0' % target, 'bsr dispatch']) +
                            ['cmp.l #$%06x,d0' % ret, 'bne .unw'])
                    if cc is None: F.emit(*call)
                    else:
                        skip = F.label()
                        F.emit(*(F.branch(cc ^ 8, skip, here) + call)); F.L.append(skip + ':')
                else:                                       # through a register or memory: the operand's address
                    try: lines = F.ea(d.ops[-1])
                    except NoNative: lines = None
                    if lines is None:
                        F.emit(*step(a))
                        F.emit('cmp.l #$%06x,d0' % fall, 'beq .c%06x' % a, 'bsr dispatch', 'cmp.l #$%06x,d0' % ret, 'bne .unw')
                        F.L.append('.c%06x:' % a)
                    else:
                        skip = F.label()
                        if cc is not None: F.emit(*F.branch(cc ^ 8, skip, here))
                        known = sorted(t for t in dyn_targets.get(a, ()) if t in entries)
                        F.emit(*(lines + ['and.l #$ffffff,d0', 'move.l d0,d2', 'move.l #$%06x,d1' % ret, 'bsr push32', 'move.l d2,d0']))
                        go = F.label()
                        for t in known[:8]:                 # the targets the traces saw: called directly
                            F.emit('cmp.l #$%06x,d0' % t, 'bne.s %s_%x' % (go, t), 'bsr F_%06x' % t, 'bra.s %s' % go)
                            F.L.append('%s_%x:' % (go, t))
                        F.emit('bsr dispatch')
                        F.L.append(go + ':')
                        F.emit('cmp.l #$%06x,d0' % ret, 'bne .unw')
                        F.L.append(skip + ':')
            if ft and nxt != fall: F.emit(*goto(fall))
        F.L.append('.unw:\trts')
        for r in runs: F.L += [r['slow'] + ':'] + r['body'] + ['\tbra %s' % r['back']]
        out.append('\n'.join(static_mem(F.L)) + '\n')
    table = ['\tcnop 0,4', 'disp_tab:'] + ['\tdc.l $%06x,F_%06x' % (e, e) for e in sorted(entries)]
    out.append('\n'.join(table) + '\nNENT\tequ %d\n' % len(entries))
    labels = sorted(set(re.findall(r'^(F_[0-9a-f]{6}|[a-z]\w*):', '\n'.join(out), re.M)))
    out.append('\tifd NGP_PROF\n' + ''.join('\txdef %s\n' % ', '.join(labels[i:i + 8]) for i in range(0, len(labels), 8)) + '\tendc\n')
    open(path, 'w').write('\n'.join(out))
    total = stats['native'] + stats['step']
    print('68k back end: %s, %d non-flow instructions native, %d through the step%s' % (os.path.basename(path), stats['native'], stats['step'],
          (' (' + ' '.join('%s:%d' % kv for kv in stepped.most_common(10)) + ')') if stepped else ''))

HEADER = r'''; generated by tools/back900.py from the game's ROM: do not edit (see that file for the register use)
        section code,code
        xdef    _asm_call
        xref    _asm_regs,_asm_regs_in,_asm_regs_out,_asm_step,_asm_wait,_asm_poll,_asm_swi,_asm_bios,_asm_fatal,_asm_ei
        xref    _asm_rd8,_asm_rd16,_asm_rd32,_asm_wr8,_asm_wr16,_asm_wr32,_ngp_ram,_ngp_rom,_ngp_even,_cpu
        xref    _ngp_vid,_ngp_vid_gen,_ngp_dirty

SYNCOUT macro
        movem.l d3-d7/a2-a5,_asm_regs
        endm
SYNCIN  macro
        movem.l _asm_regs,d3-d7/a2-a5
        endm

; uint32_t asm_call(uint32_t pc): run the game's code at pc until it returns; the address where execution goes on
_asm_call:
        movem.l d2-d7/a2-a6,-(sp)
        jsr     _asm_regs_in
        SYNCIN
        lea     _ngp_ram-$4000,a6
        move.l  48(sp),d0
        bsr     dispatch
        SYNCOUT
        move.l  d0,-(sp)
        jsr     _asm_regs_out
        move.l  (sp)+,d0
        movem.l (sp)+,d2-d7/a2-a6
        rts

; d0 = an address: jump to its function (which returns in d0 where execution goes on)
dispatch:
        and.l   #$ffffff,d0
        cmp.l   #$ff0000,d0
        bhs.s   .bios
        lea     disp_tab,a0
        move.w  #NENT,d1
.l      move.w  d1,d2
        lsr.w   #1,d2
        beq.s   .end
        lea     (a0,d2.w*8),a1
        cmp.l   (a1),d0
        blo.s   .left
        move.l  a1,a0
        sub.w   d2,d1
        bra.s   .l
.left   move.w  d2,d1
        bra.s   .l
.end    cmp.l   (a0),d0
        bne.s   .none
        move.l  4(a0),a0
        jmp     (a0)
.bios   SYNCOUT
        move.l  d0,-(sp)
        jsr     _asm_bios
        addq.l  #4,sp
        SYNCIN
        rts
.none   SYNCOUT
        pea     2.w
        move.l  d0,-(sp)
        jsr     _asm_fatal
        addq.l  #8,sp
        SYNCIN
        moveq   #0,d0
        rts

; memory: d0 = a TLCS address; reads leave the value in d1 (zero-extended), writes take it from d1.
; d0 d1 a0 a1 may change, nothing else. RAM and the cartridge are reached here, the rest through C (run68k.c).
rd8:    and.l   #$ffffff,d0
        move.l  d0,d1
        sub.l   #$4000,d1
        cmp.l   #$4000,d1
        bhs.s   .x
        moveq   #0,d1
        move.b  (a6,d0.l),d1
        rts
.x      move.l  d0,d1
        sub.l   #$200000,d1
        cmp.l   #$200000,d1
        bhs.s   .s
        move.l  _ngp_rom,a0
        move.b  (a0,d1.l),d1
        and.l   #$ff,d1
        rts
.s      move.l  d0,-(sp)
        jsr     _asm_rd8
        addq.l  #4,sp
        move.l  d0,d1
        rts
rd16:   and.l   #$ffffff,d0
        move.l  d0,d1
        sub.l   #$4000,d1
        cmp.l   #$3fff,d1
        bhs.s   .x
        moveq   #0,d1
        move.w  (a6,d0.l),d1
        ror.w   #8,d1
        rts
.x      move.l  d0,d1
        sub.l   #$200000,d1
        cmp.l   #$1fffff,d1
        bhs.s   .s
        move.l  _ngp_rom,a0
        move.w  (a0,d1.l),d1
        ror.w   #8,d1
        and.l   #$ffff,d1
        rts
.s      move.l  d0,-(sp)
        jsr     _asm_rd16
        addq.l  #4,sp
        move.l  d0,d1
        rts
rd32:   and.l   #$ffffff,d0
        move.l  d0,d1
        sub.l   #$4000,d1
        cmp.l   #$3ffd,d1
        bhs.s   .x
        move.l  (a6,d0.l),d1
        ror.w   #8,d1
        swap    d1
        ror.w   #8,d1
        rts
.x      move.l  d0,d1
        sub.l   #$200000,d1
        cmp.l   #$1ffffd,d1
        bhs.s   .s
        move.l  _ngp_rom,a0
        move.l  (a0,d1.l),d1
        ror.w   #8,d1
        swap    d1
        ror.w   #8,d1
        rts
.s      move.l  d0,-(sp)
        jsr     _asm_rd32
        addq.l  #4,sp
        move.l  d0,d1
        rts
wr8:    and.l   #$ffffff,d0
        move.l  d0,a0
        sub.l   #$4000,a0
        cmp.l   #$3000,a0
        bhs.s   .v
        move.b  d1,(a6,d0.l)
        rts
.v      sub.l   #$4800,a0                       ; maps, sprites and characters: 0x8800-0xBFFF
        cmp.l   #$3800,a0
        bhs.s   .s
        sub.l   #$8000,d0
        bra     vidw
.s      move.l  d1,-(sp)
        move.l  d0,-(sp)
        jsr     _asm_wr8
        addq.l  #8,sp
        rts
wr16:   and.l   #$ffffff,d0
        move.l  d0,a0
        sub.l   #$4000,a0
        cmp.l   #$2fff,a0
        bhs.s   .v
        ror.w   #8,d1
        move.w  d1,(a6,d0.l)
        ror.w   #8,d1
        rts
.v      sub.l   #$4800,a0                       ; maps, sprites and characters: 0x8800-0xBFFE
        cmp.l   #$37ff,a0
        bhs.s   .s
        sub.l   #$8000,d0
        move.l  d0,-(sp)
        bsr     vidw
        move.l  (sp)+,d0
        addq.l  #1,d0
        ror.w   #8,d1
        bsr     vidw
        ror.w   #8,d1
        rts
.s      move.l  d1,-(sp)
        move.l  d0,-(sp)
        jsr     _asm_wr16
        addq.l  #8,sp
        rts
wr32:   and.l   #$ffffff,d0
        move.l  d0,a0
        sub.l   #$4000,a0
        cmp.l   #$2ffd,a0
        bhs.s   .s
        ror.w   #8,d1
        swap    d1
        ror.w   #8,d1
        move.l  d1,(a6,d0.l)
        ror.w   #8,d1
        swap    d1
        ror.w   #8,d1
        rts
.s      move.l  d1,-(sp)
        move.l  d0,-(sp)
        jsr     _asm_wr32
        addq.l  #8,sp
        rts
; one byte d1 into the K2GE's memory at offset d0 (0x800-0x3FFF), as ngp_vidw() in ngp.h: store it, count the
; change and mark the map cell or the character it is in. Uses a0 and a1.
vidw:   lea     _ngp_vid,a0
        cmp.b   (a0,d0.l),d1
        beq.s   .same
        move.b  d1,(a0,d0.l)
        addq.l  #1,_ngp_vid_gen
        lea     _ngp_dirty,a1
        sub.l   #$1000,d0
        bcs.s   .same                           ; below the maps: registers, sprites
        cmp.l   #$1000,d0
        bhs.s   .chr
        lsr.l   #1,d0
        move.b  #1,(a1,d0.l)                    ; cell[]
        move.b  #1,2048+512(a1)                 ; any
.same   rts
.chr    sub.l   #$1000,d0
        lsr.l   #4,d0
        move.b  #1,2048(a1,d0.l)                ; chr[]
        move.b  #1,2048+512(a1)
        rts
; the TLCS stack (XSP = a5): push the value in d1, pop into d1
push8:  subq.l  #1,a5
        move.l  a5,d0
        bra     wr8
push16: subq.l  #2,a5
        move.l  a5,d0
        bra     wr16
push32: subq.l  #4,a5
        move.l  a5,d0
        bra     wr32
pop8:   move.l  a5,d0
        addq.l  #1,a5
        bra     rd8
pop16:  move.l  a5,d0
        addq.l  #2,a5
        bra     rd16
pop32:  move.l  a5,d0
        addq.l  #4,a5
        bra     rd32

; V (d3 bit 1) = an even number of bits set in the low byte (par8) or word (par16) of d0
par8:   and.l   #$ff,d0
        lea     _ngp_even,a0
        bclr    #1,d3
        tst.b   (a0,d0.l)
        beq.s   .o
        bset    #1,d3
.o      rts
par16:  move.l  d1,-(sp)
        moveq   #0,d1
        move.b  d0,d1
        lea     _ngp_even,a0
        move.b  (a0,d1.l),d1
        lsr.w   #8,d0
        and.l   #$ff,d0
        cmp.b   (a0,d0.l),d1
        seq     d0
        bclr    #1,d3
        tst.b   d0
        beq.s   .o
        bset    #1,d3
.o      move.l  (sp)+,d1
        rts

; arithmetic with all six flags: d0 = a, d1 = b -> d0 = result, d3 = the flags (as cpu_alu in cpu900.h). H = bit 4
; of a ^ b ^ result (bytes and words), N = 1 after a subtraction. ADC and SBC take the carry from d3.
FLAGH   macro                   ; \1 = size: d2 holds a on entry, d0 the result, d1 b
        eor.\1  d1,d2
        eor.\1  d0,d2
        and.b   #$10,d2
        add.b   d2,d2
        or.b    d2,d3
        endm
add8_f: move.l  d2,-(sp)
        move.b  d0,d2
        add.b   d1,d0
        move.w  ccr,d3
        FLAGH   b
        move.l  (sp)+,d2
        rts
add16_f: move.l d2,-(sp)
        move.w  d0,d2
        add.w   d1,d0
        move.w  ccr,d3
        FLAGH   w
        move.l  (sp)+,d2
        rts
add32_f: add.l  d1,d0
        move.w  ccr,d3
        rts
sub8_f: move.l  d2,-(sp)
        move.b  d0,d2
        sub.b   d1,d0
        move.w  ccr,d3
        FLAGH   b
        bset    #6,d3
        move.l  (sp)+,d2
        rts
sub16_f: move.l d2,-(sp)
        move.w  d0,d2
        sub.w   d1,d0
        move.w  ccr,d3
        FLAGH   w
        bset    #6,d3
        move.l  (sp)+,d2
        rts
sub32_f: sub.l  d1,d0
        move.w  ccr,d3
        bset    #6,d3
        rts
XPREP   macro                   ; the CCR for ADDX/SUBX: X = the TLCS carry, Z = 1 (so that Z ends as "result is 0")
        btst    #0,d3
        bne.s   .c\@
        move.w  #$04,-(sp)
        bra.s   .g\@
.c\@    move.w  #$15,-(sp)
.g\@    move.w  (sp)+,ccr
        endm
adc8_f: move.l  d2,-(sp)
        move.b  d0,d2
        XPREP
        addx.b  d1,d0
        move.w  ccr,d3
        FLAGH   b
        move.l  (sp)+,d2
        rts
adc16_f: move.l d2,-(sp)
        move.w  d0,d2
        XPREP
        addx.w  d1,d0
        move.w  ccr,d3
        FLAGH   w
        move.l  (sp)+,d2
        rts
adc32_f: XPREP
        addx.l  d1,d0
        move.w  ccr,d3
        rts
sbc8_f: move.l  d2,-(sp)
        move.b  d0,d2
        XPREP
        subx.b  d1,d0
        move.w  ccr,d3
        FLAGH   b
        bset    #6,d3
        move.l  (sp)+,d2
        rts
sbc16_f: move.l d2,-(sp)
        move.w  d0,d2
        XPREP
        subx.w  d1,d0
        move.w  ccr,d3
        FLAGH   w
        bset    #6,d3
        move.l  (sp)+,d2
        rts
sbc32_f: XPREP
        subx.l  d1,d0
        move.w  ccr,d3
        bset    #6,d3
        rts
; INC and DEC of a byte or word with all flags but C (C stays): d0 = a, d1 = n -> d0 = result
INCDEC  macro                   ; \1 add/sub, \2 size
        move.l  d2,-(sp)
        move.l  d3,-(sp)
        move.\2 d0,d2
        \1.\2   d1,d0
        move.w  ccr,d3
        FLAGH   \2
        and.b   #$fe,d3
        move.l  (sp)+,d1
        and.b   #1,d1
        or.b    d1,d3
        move.l  (sp)+,d2
        endm
inc8_f: INCDEC  add,b
        rts
inc16_f: INCDEC add,w
        rts
dec8_f: INCDEC  sub,b
        bset    #6,d3
        rts
dec16_f: INCDEC sub,w
        bset    #6,d3
        rts

'''
