#!/usr/bin/env python3
"""
port_target.py - port a GT3 target header from one retail build to another.

    python tools/gt3/port_target.py <from.h> <from base> <to base>
                                    --build SCES-50294 --region EU --elf SCES_502.94
                                    [-o <to.h>] [--report <file>]

GT3 US (SCUS-97102) and EU (SCES-50294) are one compile, relinked: the same
functions in the same order, moved by a per-translation-unit ladder of deltas.
This tool uses that to carry every #define of a target header across, and
checks every value it produces. Nothing is guessed silently: a define it cannot
port and verify is reported, and the tool exits non-zero.

  FUNCTIONS  Both images carry .eh_frame. Every function (FDE) of the source
             build is paired with the same-size function of the target build
             nearest to the running delta whose code is EQUIVALENT: every
             instruction identical, except the fields a relink changes - a j/jal
             target, a lui's upper half and the lower half paired with it, and a
             $gp-relative offset. Code outside any FDE (assembly, the C runtime)
             is carried by the delta of its neighbours and checked the same way
             over a window.

  CODE       A code address moves with its function. Checked: its function is
  ADDRESSES  equivalent, every call in it lands on the mapped function, and
             every data reference in it agrees with the data map.

  DATA       Every pair of equivalent functions says where each piece of data it
  ADDRESSES  references moved to (lui/lo pairs and $gp offsets). A data address
             is carried by a reference to it, or to the nearest referenced
             address below it when both neighbours agree on the delta; initial
             contents are compared where the image has them (pointers mapped).

  WORDS      A *_WORD / *_Wn define is an instruction (or data word) at one of
             the header's addresses. It is found there in the source image,
             re-read at the ported address in the target, and checked to be the
             same instruction - identical except for relocated fields, with a
             call's target on the mapped function.

  OFFSETS    Values below the image (struct offsets, counts, masks) are kept.
             They are safe to keep only because the code that uses them is
             identical: the report gives the share of all functions that are
             equivalent, and every function the header names is checked.

A few defines are not addresses at all and are computed (the pool, the plugin's
link address) or named (region, build, ELF); see SPECIAL below.
"""
import argparse
import re
import struct
import sys
from collections import Counter, defaultdict

# --------------------------------------------------------------------- images


class Image:
    def __init__(self, path):
        self.path = path
        d = open(path, 'rb').read()
        phoff, = struct.unpack_from('<I', d, 0x1C)
        phent, phnum = struct.unpack_from('<HH', d, 0x2A)
        self.segs = []
        for i in range(phnum):
            t, off, va, pa, fsz, msz, fl, al = struct.unpack_from('<8I', d, phoff + i * phent)
            if t == 1 and fsz:
                self.segs.append((va, d[off:off + fsz]))
        self.lo = min(v for v, _ in self.segs)
        self.hi = max(v + len(b) for v, b in self.segs)
        self.gp = self._find_gp()
        self.eh_lo = None
        self.fdes = self._find_fdes()
        if not self.fdes:
            raise SystemExit('%s: no .eh_frame found' % path)
        self.text_lo = min(s for s, n in self.fdes)
        # Code runs from the image's start to .eh_frame, which follows .text in
        # these links. The C runtime and the assembly at the top of .text carry
        # no FDEs, so the last FDE does not mark the end of code.
        self.code_hi = self.eh_lo

    def u32(self, va):
        for v, b in self.segs:
            if v <= va and va + 4 <= v + len(b):
                return struct.unpack_from('<I', b, va - v)[0]
        return None

    def raw(self, va, n):
        for v, b in self.segs:
            if v <= va and va + n <= v + len(b):
                return b[va - v:va - v + n]
        return None

    def _find_gp(self):
        # crt0: lui $a0, hi ... addiu $a0, $a0, lo ... move $gp, $a0
        hi = None
        for k in range(64):
            w = self.u32(self.lo + 4 * k)
            if w is None:
                break
            if w >> 16 == 0x3C04:
                hi = (w & 0xFFFF) << 16
            elif w >> 16 == 0x2484 and hi is not None:
                gp = (hi + sx16(w & 0xFFFF)) & 0xFFFFFFFF
                return gp
        raise SystemExit('%s: cannot find $gp in crt0' % self.path)

    def _find_fdes(self):
        """(start, size) of every FDE: find each CIE (id 0, version 1, and the
        'eh' or empty augmentation GT3 uses) and walk the records after it."""
        out = set()
        for sva, b in self.segs:
            i = 0
            n = len(b)
            while i < n - 16:
                ln, cid = struct.unpack_from('<II', b, i)
                if cid == 0 and 0x0C <= ln < 0x40 and b[i + 8:i + 12] in (b'\x01eh\x00', b'\x01\x00\x01\x78'):
                    if self.eh_lo is None or sva + i < self.eh_lo:
                        self.eh_lo = sva + i
                    j = i
                    while j < n - 16:
                        l2, c2 = struct.unpack_from('<II', b, j)
                        if l2 == 0 or l2 > 0x10000:
                            break
                        if c2 != 0:
                            loc, rng = struct.unpack_from('<II', b, j + 8)
                            if self.lo <= loc < self.hi and 0 < rng < 0x100000 and loc % 4 == 0:
                                out.add((loc, rng))
                        j += 4 + l2
                    i = j
                i += 4
        return sorted(out)


def sx16(x):
    return x - 0x10000 if x & 0x8000 else x


# ------------------------------------------------------ instruction equivalence

J, JAL, LUI, ORI = 0x02, 0x03, 0x0F, 0x0D
# instructions whose imm16 can be the low half of an address (or a $gp offset)
LO_OPS = {0x09, 0x19, ORI, 0x1A, 0x1B, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
          0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x31, 0x36, 0x37, 0x39, 0x3E, 0x3F}


LOADS = {0x1A, 0x1B, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x37}
ALU_I = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x18, 0x19}


def gpr_written(w):
    """The general register an instruction writes, or None (approximate, but
    it errs towards 'writes' - which only drops a lui/lo pairing)."""
    op = w >> 26
    if op == 0:
        fn = w & 63
        if fn in (0x08, 0x0C, 0x0D, 0x0F, 0x11, 0x13, 0x18, 0x19, 0x1A, 0x1B):
            return None                       # jr, syscall, break, sync, mthi, mtlo, mult/div
        return (w >> 11) & 31
    if op in LOADS or op in ALU_I or op == LUI:
        return (w >> 16) & 31
    if op == JAL:
        return 31
    if op == 0x11 and ((w >> 21) & 31) in (0, 1, 2):   # mfc1, dmfc1, cfc1
        return (w >> 16) & 31
    if op == 0x1C:                            # MMI
        return (w >> 11) & 31
    return None


def same_instruction(wa, wb):
    """wa and wb are the same instruction, but for a field a relink changes."""
    op = wa >> 26
    if op != wb >> 26:
        return False
    if op in (J, JAL):
        return True
    if op == LUI or op in LO_OPS:
        return (wa ^ wb) & 0xFFFF0000 == 0
    return wa == wb


def float_constant_diffs(A, a, B, b, n):
    """Differences the relocation rules would otherwise accept as addresses:
    a lui (or lui/ori) value that goes into an FPU register - a float constant.
    PAL retunes these (60.0 -> 50.0), so they are reported, not failed."""
    out = []
    vals = {}                                 # reg -> (valueA, valueB)
    for k in range(n):
        wa, wb = A.u32(a + 4 * k), B.u32(b + 4 * k)
        if wa is None or wb is None or wa >> 26 != wb >> 26:
            break
        op = wa >> 26
        rs, rt = (wa >> 21) & 31, (wa >> 16) & 31
        if op == LUI:
            vals[rt] = ((wa & 0xFFFF) << 16, (wb & 0xFFFF) << 16)
            continue
        if op == ORI and rs in vals and rt == rs:
            va, vb = vals[rs]
            vals[rt] = (va | (wa & 0xFFFF), vb | (wb & 0xFFFF))
            continue
        if op == 0x11 and ((wa >> 21) & 31) in (4, 5) and rt in vals:      # mtc1 / dmtc1
            va, vb = vals[rt]
            if va != vb:
                fa = struct.unpack('<f', struct.pack('<I', va))[0]
                fb = struct.unpack('<f', struct.pack('<I', vb))[0]
                out.append('+0x%X float %.7g vs %.7g' % (4 * k, fa, fb))
            continue
        d = gpr_written(wa)
        if d is not None:
            vals.pop(d, None)
    return out


def first_difference(A, a, B, b, n):
    """The first non-relocation difference, for the report."""
    ok, why, _, _ = equivalent(A, a, B, b, n)
    return why


def equivalent(A, a, B, b, n, pre=0):
    """Compare n words at A:a and B:b. Returns (ok, why, code_pairs, data_pairs):
    code_pairs are (source, target) call targets, data_pairs (source, target)
    data addresses, both as the two images compute them.

    The walk is linear, so a lui's upper half is kept across a branch join: a
    register that consumes its own half (addiu a0,a0,lo) keeps it, since
    another path may still use it. That can pair a later immediate with a stale
    half; such a pair computes the same address in both images and is not
    recorded, so it can neither pollute the data map nor hide a difference.

    pre: words before `a` scanned only for lui's (a window opened mid-function)."""
    code, data = [], []
    his = {}                                  # reg -> (hiA, hiB), from a lui
    for k in range(-pre, 0):
        wa, wb = A.u32(a + 4 * k), B.u32(b + 4 * k)
        if wa is None or wb is None:
            continue
        if wa >> 26 == LUI and (wa ^ wb) & 0xFFFF0000 == 0:
            his[(wa >> 16) & 31] = (wa & 0xFFFF, wb & 0xFFFF)
        else:
            d = gpr_written(wa)
            if d is not None and not (wa >> 26 in LO_OPS and (wa >> 21) & 31 == d):
                his.pop(d, None)
    for k in range(n):
        wa, wb = A.u32(a + 4 * k), B.u32(b + 4 * k)
        if wa is None or wb is None:
            return False, 'outside the image at +0x%X' % (4 * k), code, data
        op = wa >> 26
        if op != wb >> 26:
            return False, 'different instruction at +0x%X (%08X vs %08X)' % (4 * k, wa, wb), code, data
        rs, rt = (wa >> 21) & 31, (wa >> 16) & 31
        if op in (J, JAL):
            if wa != wb:
                code.append(((wa & 0x3FFFFFF) << 2 | (a & 0xF0000000),
                             (wb & 0x3FFFFFF) << 2 | (b & 0xF0000000)))
            his.pop(31, None)
            continue
        if op == LUI:
            if (wa ^ wb) & 0xFFFF0000:
                return False, 'different registers at +0x%X' % (4 * k), code, data
            his[rt] = (wa & 0xFFFF, wb & 0xFFFF)
            continue
        if op in LO_OPS:
            if (wa ^ wb) & 0xFFFF0000:
                return False, 'different registers at +0x%X (%08X vs %08X)' % (4 * k, wa, wb), code, data
            la, lb = wa & 0xFFFF, wb & 0xFFFF
            pair = None
            if rs == 28:
                pair = ((A.gp + sx16(la)) & 0xFFFFFFFF, (B.gp + sx16(lb)) & 0xFFFFFFFF)
            elif rs in his:
                ha, hb = his[rs]
                if op == ORI:
                    pair = (ha << 16 | la, hb << 16 | lb)
                else:
                    pair = ((ha << 16) + sx16(la) & 0xFFFFFFFF, (hb << 16) + sx16(lb) & 0xFFFFFFFF)
            elif la != lb:
                return False, 'different immediate at +0x%X (%08X vs %08X)' % (4 * k, wa, wb), code, data
            if pair and pair[0] != pair[1]:
                data.append(pair)
            if rs in his and gpr_written(wa) == rs:
                continue                      # consumes its own upper half: keep it (joins)
        elif wa != wb:
            return False, 'different word at +0x%X (%08X vs %08X)' % (4 * k, wa, wb), code, data
        d = gpr_written(wa)
        if d is not None:
            # addu/daddu/add carry a pending upper half into their result: an
            # indexed table read is lui at,hi; addu at,at,idx; lw x,lo(at).
            if op == 0 and (wa & 63) in (0x20, 0x21, 0x2C, 0x2D) and (rs in his or rt in his):
                his[d] = his[rs] if rs in his else his[rt]
            else:
                his.pop(d, None)
    return True, '', code, data


# --------------------------------------------------------------- the maps

class Port:
    def __init__(self, A, B):
        self.A, self.B = A, B
        self.fmap = {}                        # source function start -> target start
        self.fsize = {}
        self.ok_funcs = 0
        self.data_votes = defaultdict(Counter)
        self.code_votes = defaultdict(Counter)
        self._match_functions()
        self._build_data_map()

    def _match_functions(self):
        A, B = self.A, self.B
        by_size = defaultdict(list)
        for s, n in B.fdes:
            by_size[n].append(s)
        prev = 0
        for s, n in A.fdes:
            self.fsize[s] = n
            cands = [e for e in by_size[n] if abs(e - s) < 0x8000]
            cands.sort(key=lambda e: abs((e - s) - prev))
            for e in cands:
                ok, _, code, data = equivalent(A, s, B, e, n // 4)
                if ok:
                    self.fmap[s] = e
                    prev = e - s
                    for p, q in code:
                        self.code_votes[p][q] += 1
                    for p, q in data:
                        self.data_votes[p][q] += 1
                    break
        self.ok_funcs = len(self.fmap)

    def _build_data_map(self):
        self.dmap = {}
        for p, votes in self.data_votes.items():
            q, c = votes.most_common(1)[0]
            self.dmap[p] = q
        self.dkeys = sorted(self.dmap)

    # -- code
    def func_of(self, va):
        best = None
        for s, n in self.A.fdes:            # few lookups: linear is fine
            if s <= va < s + n:
                if best is None or n < best[1]:
                    best = (s, n)
        return best

    def neighbour_deltas(self, va):
        before = [s for s in self.fmap if s <= va]
        after = [s for s in self.fmap if s > va]
        ds = []
        if before:
            s = max(before)
            ds.append(self.fmap[s] - s)
        if after:
            s = min(after)
            ds.append(self.fmap[s] - s)
        return ds

    def code(self, va):
        """Port a code address. Returns (target, how) or (None, why)."""
        f = self.func_of(va)
        if f and f[0] in self.fmap:
            s, n = f
            e = self.fmap[s]
            ok, why, code, data = equivalent(self.A, s, self.B, e, n // 4)
            bad = self._check_refs(code, data)
            if bad:
                return None, 'function %08X: %s' % (s, bad)
            fl = float_constant_diffs(self.A, s, self.B, e, n // 4)
            return e + (va - s), 'in function %08X (+0x%X), equivalent, %d calls and %d data refs agree%s' % (
                s, va - s, len(code), len(data), ('; FLOAT CONSTANTS DIFFER: ' + ', '.join(fl)) if fl else '')
        # Outside any FDE (crt0, the C runtime, assembly), or its function did not
        # match. Candidate deltas: calls to it or near it from matched code, its
        # matched neighbours, and none at all; the one that makes a window around
        # it equivalent wins.
        cands = []
        if va in self.code_votes:
            cands.append(self.code_votes[va].most_common(1)[0][0] - va)
        near = sorted(self.code_votes, key=lambda t: abs(t - va))[:6]
        cands += [self.code_votes[t].most_common(1)[0][0] - t for t in near]
        cands += self.neighbour_deltas(va) + [0]
        for dlt in dict.fromkeys(cands):
            lo = max(self.A.lo, va - 0x20)
            n = min(32, (self.A.code_hi - lo) // 4)
            ok, why, code, data = equivalent(self.A, lo, self.B, lo + dlt, n, pre=16)
            if ok and not self._check_refs(code, data):
                src = 'calls to it' if va in self.code_votes and dlt == cands[0] else 'nearby calls / neighbours'
                ctx = ''
                if f:                         # an FDE function that is not equivalent as a whole
                    s, fn = f
                    ctx = '; its function %08X DIFFERS: %s' % (s, first_difference(self.A, s, self.B, s + dlt, fn // 4))
                fl = float_constant_diffs(self.A, lo, self.B, lo + dlt, n)
                if fl:
                    ctx += '; FLOAT CONSTANTS DIFFER: ' + ', '.join(fl)
                return va + dlt, 'no matched FDE; delta %+#x (%s), %d-word window equivalent%s' % (dlt, src, n, ctx)
        return None, 'no matched function and no candidate delta gives equivalent code'

    def _check_refs(self, code, data):
        for p, q in code:
            if p in self.fmap and self.fmap[p] != q:
                return 'call to %08X lands on %08X, not the mapped %08X' % (p, q, self.fmap[p])
        for p, q in data:
            if p in self.dmap and self.dmap[p] != q:
                return 'data ref %08X -> %08X disagrees with the map (%08X)' % (p, q, self.dmap[p])
        return ''

    # -- data
    def data(self, va):
        if va in self.dmap:
            n = sum(self.data_votes[va].values())
            return self.dmap[va], 'referenced directly (%d references)' % n
        import bisect
        i = bisect.bisect_left(self.dkeys, va)
        below = self.dkeys[i - 1] if i > 0 else None
        above = self.dkeys[i] if i < len(self.dkeys) else None
        if below is None or above is None:
            return None, 'no references around it'
        d1 = self.dmap[below] - below
        d2 = self.dmap[above] - above
        if d1 != d2:
            return None, 'references below (%08X, %+#x) and above (%08X, %+#x) disagree' % (below, d1, above, d2)
        return va + d1, 'between references %08X and %08X, both %+#x' % (below, above, d1)

    known = {}                                # extra (source, target) value pairs, e.g. _end

    def pointer_maps(self, wa, wb):
        if wa == wb or self.known.get(wa) == wb:
            return True
        if self.A.lo <= wa < self.A.code_hi:
            m, _ = self.code(wa)
            return m == wb
        if self.A.code_hi <= wa < 0x02000000:
            m, _ = self.data(wa)
            return m == wb
        return False

    def content_check(self, va, tv, n=16):
        """Compare initial contents word by word; pointers must map."""
        for k in range(0, n, 4):
            wa, wb = self.A.u32(va + k), self.B.u32(tv + k)
            if wa is None and wb is None:
                return 'no initial contents (.bss)'
            if wa is None or wb is None:
                return 'MISMATCH: only one image has contents'
            if not self.pointer_maps(wa, wb):
                return 'MISMATCH at +0x%X: %08X vs %08X' % (k, wa, wb)
        return 'contents agree (%d bytes, pointers mapped)' % n


# ------------------------------------------------------------------ header

DEFINE = re.compile(r'^#define\s+(\w+)\s+(\S+)(.*)$')


def parse_header(path):
    out = []
    for line in open(path, encoding='utf-8'):
        m = DEFINE.match(line.rstrip('\n'))
        if m:
            out.append((m.group(1), m.group(2)))
    return out


def num(v):
    v = v.rstrip('uU')
    return int(v, 0)


def is_word(name):
    return name.endswith('_WORD') or re.search(r'_W\d$', name) is not None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('header')
    ap.add_argument('src_base')
    ap.add_argument('dst_base')
    ap.add_argument('--build', required=True)
    ap.add_argument('--region', required=True)
    ap.add_argument('--elf', required=True)
    ap.add_argument('-o', '--out')
    ap.add_argument('--report')
    ap.add_argument('--title', default='')
    args = ap.parse_args()

    A, B = Image(args.src_base), Image(args.dst_base)
    P = Port(A, B)
    defs = parse_header(args.header)
    val = {n: v for n, v in defs}
    rep = []
    say = rep.append
    say('source %s: gp %08X, %d FDEs, code %08X..%08X' % (args.src_base, A.gp, len(A.fdes), A.lo, A.code_hi))
    say('target %s: gp %08X, %d FDEs, code %08X..%08X' % (args.dst_base, B.gp, len(B.fdes), B.lo, B.code_hi))
    say('functions paired as equivalent: %d of %d (%.2f%%); data addresses referenced: %d'
        % (P.ok_funcs, len(A.fdes), 100.0 * P.ok_funcs / max(1, len(A.fdes)), len(P.dmap)))
    deltas = Counter(e - s for s, e in P.fmap.items())
    say('distinct function deltas: %d; most common: %s' % (
        len(deltas), ', '.join('%+#x x%d' % kv for kv in deltas.most_common(6))))

    out = {}
    how = {}
    failed = []

    # --- SPECIAL: named, computed
    region_old = [n for n, _ in defs if n.startswith('GAME_REGION_')]
    for n in region_old:
        out[n] = None                                      # renamed below
    out['REGION_NAME'] = '"%s"' % args.region
    out['BUILD_NAME'] = '"%s"' % args.build
    out['ELF_NAME'] = '"%s"' % args.elf
    for n in ('REGION_NAME', 'BUILD_NAME', 'ELF_NAME'):
        how[n] = 'named'

    # The frame rate: PD's PAL conversion replaced the frame time 1/60 with 1/50
    # throughout (US holds 1/60 in 52 data words, EU in 2; EU holds 1/50 in 56).
    if 'GAME_FRAME_HZ' in val:
        def count(I, w):
            return sum(1 for v, b in I.segs for i in range(0, len(b) - 3, 4)
                       if v + i >= I.code_hi and struct.unpack_from('<I', b, i)[0] == w)
        n60 = count(B, 0x3C888889) + count(B, 0x3C888888)
        n50 = count(B, 0x3CA3D70A)
        if n60 > 4 * n50:
            hz = 60
        elif n50 > 4 * n60:
            hz = 50
        else:
            hz = None
        if hz is None:
            failed.append(('GAME_FRAME_HZ', 'cannot tell: 1/60 x%d, 1/50 x%d in the target\'s data' % (n60, n50)))
        else:
            out['GAME_FRAME_HZ'] = str(hz)
            how['GAME_FRAME_HZ'] = 'detected: the frame time in the target\'s data is 1/60 x%d, 1/50 x%d' % (n60, n50)

    # _end: the heap's base word ships holding it, and so does sbrk's break. It
    # moves with .bss, which nothing references, so tell the content checks.
    if 'POOL_BASE_GLOBAL' in val:
        t, _ = P.data(num(val['POOL_BASE_GLOBAL']))
        if t is not None and A.u32(num(val['POOL_BASE_GLOBAL'])) is not None and B.u32(t) is not None:
            P.known[A.u32(num(val['POOL_BASE_GLOBAL']))] = B.u32(t)

    # --- addresses first (words need them)
    addr_names = []
    for n, v in defs:
        if n in out or is_word(n):
            continue
        try:
            x = num(v)
        except ValueError:
            out[n] = v
            how[n] = 'kept (not a number)'
            continue
        if n in ('GAME_END', 'POOL_BASE_VALUE', 'PLUGIN_BASE_ADDRESS', 'POOL_BASE_NEW', 'PLUGIN_RESERVE_BYTES',
                 'GAME_FRAME_HZ'):
            continue
        if A.lo <= x < A.code_hi:
            t, h = P.code(x)
            kind = 'code'
        elif A.code_hi <= x < 0x02000000:
            t, h = P.data(x)
            kind = 'data'
            if t is not None:
                c = P.content_check(x, t)
                h += '; ' + c
                if c.startswith('MISMATCH'):
                    t = None
        else:
            out[n] = v
            how[n] = 'kept: offset/constant'
            continue
        if t is None:
            failed.append((n, '%s address %08X: %s' % (kind, x, h)))
        else:
            out[n] = '0x%08X' % t
            how[n] = '%s: %s' % (kind, h)
            addr_names.append(n)

    # --- SPECIAL: the pool and the plugin's link address
    if 'POOL_BASE_GLOBAL' in out:
        pbg = num(out['POOL_BASE_GLOBAL'])
        end = B.u32(pbg)
        if end is None:
            failed.append(('POOL_BASE_VALUE', 'target image has no word at POOL_BASE_GLOBAL %08X' % pbg))
        else:
            reserve = num(val.get('PLUGIN_RESERVE_BYTES', '0x10000'))
            base = (end + 127) & ~127
            out['GAME_END'] = '0x%08X' % end
            out['POOL_BASE_VALUE'] = '0x%08X' % end
            out['PLUGIN_BASE_ADDRESS'] = '0x%08X' % base
            out['PLUGIN_RESERVE_BYTES'] = val.get('PLUGIN_RESERVE_BYTES', '0x10000')
            out['POOL_BASE_NEW'] = '0x%08X' % (base + reserve)
            for n in ('GAME_END', 'POOL_BASE_VALUE'):
                how[n] = 'computed: the word POOL_BASE_GLOBAL ships holding (_end)'
            how['PLUGIN_BASE_ADDRESS'] = 'computed: _end rounded up to 128'
            how['PLUGIN_RESERVE_BYTES'] = 'kept'
            how['POOL_BASE_NEW'] = 'computed: PLUGIN_BASE_ADDRESS + PLUGIN_RESERVE_BYTES'
            if 'SBRK_BREAK_GLOBAL' in out:
                sb = B.u32(num(out['SBRK_BREAK_GLOBAL']))
                if sb != end:
                    failed.append(('SBRK_BREAK_GLOBAL', 'target sbrk break holds %08X, not _end %08X' % (sb or 0, end)))

    # --- words
    sites = [(n, num(val[n]), num(out[n])) for n in addr_names]
    for n, v in defs:
        if not is_word(n):
            continue
        w = num(v)
        base_name = re.sub(r'(_W\d|_WORD)$', '', n)
        cands = []
        for sn, sa, sb in sites:
            for k in range(0, 0x20, 4):
                if A.u32(sa + k) == w:
                    common = len(_common_prefix(sn, base_name))
                    cands.append((common, sn, sa, sb, k))
        if not cands:
            failed.append((n, 'word %08X is at none of the header addresses (+0..+0x1C)' % w))
            continue
        best = max(c[0] for c in cands)
        chosen = [c for c in cands if c[0] == best]
        new = set()
        notes = []
        bad = ''
        for _, sn, sa, sb, k in chosen:
            wb = B.u32(sb + k)
            new.add(wb)
            if A.lo <= sa < A.code_hi:
                # An instruction. Its function was checked equivalent when the
                # site was ported (which covers a relocated lui/lo pair); here:
                # the same instruction, and a call lands on the mapped function.
                if not same_instruction(w, wb):
                    bad = 'at %s+0x%X: %08X vs %08X is not the same instruction' % (sn, k, w, wb)
                elif w >> 26 in (J, JAL) and w != wb:
                    tu = (w & 0x3FFFFFF) << 2
                    tb = (wb & 0x3FFFFFF) << 2
                    m, _ = P.code(tu)
                    if m != tb:
                        bad = 'at %s+0x%X: call to %08X lands on %08X, the map says %s' % (
                            sn, k, tu, tb, '%08X' % m if m is not None else 'nothing')
            elif not P.pointer_maps(w, wb):
                bad = 'data word at %s: %08X vs %08X, not a mapped pointer' % (sn, w, wb)
            notes.append('%s+0x%X' % (sn, k))
        if bad or len(new) != 1:
            failed.append((n, bad or 'sites disagree in the target: %s' % ', '.join('%08X' % x for x in new)))
            continue
        nw = new.pop()
        out[n] = '0x%08X' % nw
        how[n] = 'word at %s%s' % (', '.join(notes), '' if nw == w else ' (relocated)')

    # --- report
    say('')
    for n, v in defs:
        if n in out and out[n] is not None:
            say('%-28s %-12s -> %-12s %s' % (n, v, out[n], how.get(n, '')))
    say('')
    for n, why in failed:
        say('NOT PORTED  %-26s %s' % (n, why))
    say('')
    say('%d defines, %d ported, %d not ported' % (len(defs), len([1 for n, _ in defs if out.get(n) is not None]) + len(region_old), len(failed)))
    text = '\n'.join(rep) + '\n'
    if args.report:
        open(args.report, 'w', encoding='utf-8').write(text)
    else:
        sys.stdout.write(text)

    if args.out and not failed:
        write_header(args, defs, out, how, region_old, P)
    elif args.out:
        print('header NOT written: %d define(s) not ported (see the report)' % len(failed))
    return 1 if failed else 0


def _common_prefix(a, b):
    k = 0
    while k < min(len(a), len(b)) and a[k] == b[k]:
        k += 1
    return a[:k]


def write_header(args, defs, out, how, region_old, P):
    lines = []
    src_lines = open(args.header, encoding='utf-8').read().splitlines()
    lines.append('#pragma once')
    lines.append('')
    lines.append('/*')
    if args.title:
        lines.append('    %s' % args.title)
    lines.append('    Build ID : %s    Boot ELF : %s' % (args.build, args.elf))
    lines.append('')
    lines.append('    GENERATED by tools/gt3/port_target.py from %s - do not edit by hand;' % _basename(args.header))
    lines.append('    change that header and rerun the tool (the command is in the README).')
    lines.append('    What each address is, and the evidence for it, is documented there: this')
    lines.append('    file carries only the values, each with the source build\'s for reference.')
    lines.append('')
    lines.append('    Every value was ported by function matching and code reference, and checked:')
    lines.append('    %d of %d functions pair as equivalent code (identical but for relocated' % (P.ok_funcs, len(P.A.fdes)))
    lines.append('    fields); every code address lies in an equivalent function whose calls and')
    lines.append('    data references agree with the map; every instruction word was re-read here')
    lines.append('    and checked to be the same instruction.')
    diffs = []
    for n, h in how.items():
        for m in re.finditer(r'its function ([0-9A-F]{8}) DIFFERS: ([^;]+)', h):
            diffs.append('function %s (source build): %s' % (m.group(1), m.group(2)))
        m = re.search(r'in function ([0-9A-F]{8}).*FLOAT CONSTANTS DIFFER: ([^;]+)', h)
        if m:
            diffs.append('function %s (source build): %s' % (m.group(1), m.group(2)))
    if diffs:
        lines.append('')
        lines.append('    Functions holding these addresses that are NOT identical to the source')
        lines.append('    build\'s - a frame count or float constant retuned for this build; around')
        lines.append('    every address the header names, the code is equivalent:')
        for d in sorted(set(diffs)):
            lines.append('      %s' % d)
    lines.append('*/')
    for s in src_lines:
        m = DEFINE.match(s)
        if m:
            n, v = m.group(1), m.group(2)
            if n in region_old:
                lines.append('#define GAME_REGION_%s 1' % args.region)
                continue
            lines.append('#define %-28s %-12s /* %s %s */' % (n, out[n], _region_of(args.header), v))
        elif re.match(r'^/\* =+ .* \*/$', s) or re.match(r'^/\* -+ .* \*/$', s) or re.match(r'^/\* -- .* -+ \*/$', s):
            lines.append('')
            lines.append(s)
    lines.append('')
    open(args.out, 'w', encoding='utf-8', newline='\n').write('\n'.join(lines))


def _basename(p):
    return re.split(r'[\\/]', p)[-1]


def _region_of(header):
    b = _basename(header)
    return {'SCUS-97102.h': 'US', 'SCES-50294.h': 'EU'}.get(b, b)


if __name__ == '__main__':
    sys.exit(main())
