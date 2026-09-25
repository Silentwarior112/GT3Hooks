"""seinf_tool.py - read, check and merge GT3 / Lupo 2 `sound/se.inf` files.

The format below is taken from the CODE (retail US), not from a sample file:

  US 0x0022C3A0 (relocate; Lupo 2 twin 0x0023B740)
      base_stored = u32 @+0x08          ; overwritten with the load address
      count       = u32 @+0x0C
      for i < count: entry[i].name += (load_addr - base_stored)
  US 0x0022C4C4..0x0022C52C (registration; Lupo 2 0x0023B878..0x0023B8E8)
      for i in 0..count-1:                     ; se.inf entries, ascending
          for s in 0..TABLE_N-1:               ; 0x28 retail, 0x33 Lupo 2
              if strcmp(entry[i].name, table[s].name) == 0:
                  table[s].seIndex = i ; break  ; first table hit, LAST se.inf hit wins
      (no write when nothing matches: seIndex keeps its static value, 0 in both builds)
  US 0x0022C588 getSEDescriptor(slot) = u32 @ blob + 0x14 + table[slot].seIndex*8
      bank = packed>>24 (x0x20 into bank array), progA = (packed>>12)&0xFFF, progB = packed&0xFFF
      acquire (US 0x0022C5B0 / L2 0x0023B970) uses progB only when its mode arg == 2.

  struct SeInfHeader { u32 unk0; u32 unk4; u32 base; u32 count; SeInfEntry e[count]; }
  struct SeInfEntry  { u32 name /* base-relative pointer to NUL-terminated string */; u32 packed; }
  +0x00/+0x04 are never read by the EE code (only 5 accesses to the blob pointer, all listed above).

Usage:
  seinf_tool.py dump    <se.inf>
  seinf_tool.py resolve <se.inf> [--banks N]      emulate retail+Pod (51-name) registration
  seinf_tool.py merge   <retail se.inf> <lupo2 se.inf> <out> [--prefix pod_]
  seinf_tool.py selftest
"""
import os, struct, sys, tempfile

RETAIL_NAMES = ("buy cancel car_wash cursor disable next oil ok param return select slide wheel "
                "b_off body_noiz clash clash2 clash3 clash4 clash5 count_d count_s heri in_car "
                "off_bound off_roadnoiz off_skill on_bound on_road on_skill rain_noiz rumble "
                "turbine turbine2 air city crowds sea tunnel tunnel2").split()
POD_NAMES = ("pod_genki pod_iya pod_kowai1 pod_kowai2 pod_kyoro pod_naku pod_nemui pod_odoroku "
             "pod_okoru pod_sabishii pod_wink").split()
TABLE51 = RETAIL_NAMES + POD_NAMES
assert len(RETAIL_NAMES) == 40 and len(TABLE51) == 51


def parse(blob):
    if len(blob) < 0x10:
        raise ValueError('file shorter than the 0x10-byte header')
    unk0, unk4, base, count = struct.unpack_from('<4I', blob, 0)
    if 0x10 + count * 8 > len(blob):
        raise ValueError('count %d runs past EOF (%d bytes)' % (count, len(blob)))
    ents = []
    for i in range(count):
        np, packed = struct.unpack_from('<II', blob, 0x10 + i * 8)
        off = (np - base) & 0xFFFFFFFF
        if off >= len(blob):
            raise ValueError('entry %d name offset 0x%X outside file' % (i, off))
        end = blob.find(b'\0', off)
        if end < 0:
            raise ValueError('entry %d name not NUL-terminated' % i)
        ents.append((blob[off:end].decode('latin-1'), packed))
    return (unk0, unk4, base), ents


def dec(p):
    return p >> 24, (p >> 12) & 0xFFF, p & 0xFFF


def build(hdr, ents):
    unk0, unk4, base = hdr
    pool = bytearray(); offs = []
    strstart = 0x10 + len(ents) * 8
    for name, _ in ents:
        offs.append(strstart + len(pool)); pool += name.encode('latin-1') + b'\0'
    out = bytearray(struct.pack('<4I', unk0, unk4, base, len(ents)))
    for (name, packed), o in zip(ents, offs):
        out += struct.pack('<II', (base + o) & 0xFFFFFFFF, packed)
    out += pool
    while len(out) % 16:
        out += b'\0'
    return bytes(out)


def resolve(ents, names=TABLE51):
    """Exact emulation of US 0x0022C4C4..0x0022C52C with the table widened to len(names)."""
    seidx = [0] * len(names)            # static value in both builds' tables (VERIFIED)
    hit = [False] * len(names)
    for i, (nm, _) in enumerate(ents):
        for s, tn in enumerate(names):
            if nm == tn:
                seidx[s] = i; hit[s] = True
                break
    return seidx, hit


def cmd_dump(path):
    hdr, ents = parse(open(path, 'rb').read())
    print('unk0=0x%08X unk4=0x%08X base=0x%08X count=%d' % (hdr[0], hdr[1], hdr[2], len(ents)))
    for i, (n, p) in enumerate(ents):
        b, a, c = dec(p)
        print('%4d  %-20s packed=%08X bank=%d progA=%d progB=%d' % (i, n, p, b, a, c))


def cmd_resolve(path, banks=4):
    hdr, ents = parse(open(path, 'rb').read())
    seidx, hit = resolve(ents)
    bad = 0
    for s, nm in enumerate(TABLE51):
        i = seidx[s]; b, a, c = dec(ents[i][1]) if ents else (0, 0, 0)
        tag = ''
        if not hit[s] and s >= 40:
            tag = '  <-- NOT FOUND: the plugin leaves this pod sound silent'; bad += 1
        elif not hit[s]:
            tag = '  <-- NOT FOUND: stays 0, plays entry 0 "%s"' % (ents[0][0] if ents else '?'); bad += 1
        elif b >= banks:
            tag = '  <-- bank %d >= %d loaded banks: indexes past the bank array' % (b, banks); bad += 1
        print('slot %2d %-14s seIndex=%-4d bank=%d progA=%d progB=%d%s' % (s, nm, i, b, a, c, tag))
    print('%d problem(s)' % bad)
    return bad


def cmd_merge(retail, donor, out, prefix='pod_'):
    rh, re_ = parse(open(retail, 'rb').read())
    dh, de = parse(open(donor, 'rb').read())
    rnames = {n: p for n, p in re_}
    # report entries both files name but encode differently (bank layouts differ between builds)
    for n, p in de:
        if n in rnames and rnames[n] != p:
            print('NOTE: %-16s retail %08X vs Lupo 2 %08X (bank/prog differ)' % (n, rnames[n], p))
    add = [(n, p) for n, p in de if n.startswith(prefix) and n not in rnames]
    for n, p in add:
        b, a, c = dec(p)
        print('append %-16s packed=%08X bank=%d progA=%d progB=%d' % (n, p, b, a, c))
    missing = [n for n in POD_NAMES if n not in {x for x, _ in add} and n not in rnames]
    if missing:
        print('WARNING: donor lacks', missing)
    blob = build(rh, list(re_) + add)          # retail entries keep their indices
    open(out, 'wb').write(blob)
    print('wrote %s: %d entries (%d retail + %d appended), %d bytes' % (out, len(re_) + len(add), len(re_), len(add), len(blob)))


def selftest():
    # synthetic: retail-shaped file with 40 names in banks 0..2, donor with the 11 pod names in bank 3
    r = build((0x11, 0x22, 0x1000), [(n, (i % 3) << 24 | i << 12 | i) for i, n in enumerate(RETAIL_NAMES)])
    d = build((0, 0, 0), [(n, 3 << 24 | (i + 1) << 12 | (i + 1)) for i, n in enumerate(POD_NAMES)])
    with tempfile.TemporaryDirectory() as td:
        rp, dp, mp = (os.path.join(td, f) for f in ('r.inf', 'd.inf', 'm.inf'))
        open(rp, 'wb').write(r); open(dp, 'wb').write(d)
        _, re_ = parse(r)
        seidx, hit = resolve(re_)
        assert all(hit[:40]) and not any(hit[40:]) and all(seidx[s] == 0 for s in range(40, 51))
        cmd_merge(rp, dp, mp)
        assert cmd_resolve(mp, 4) == 0
        assert cmd_resolve(mp, 3) == 11
        h, me = parse(open(mp, 'rb').read())
        assert h == (0x11, 0x22, 0x1000) and me[:40] == re_
    print('selftest OK')


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a or a[0] not in ('dump', 'resolve', 'merge', 'selftest'):
        print(__doc__); sys.exit(1)
    if a[0] == 'dump': cmd_dump(a[1])
    elif a[0] == 'resolve':
        nb = int(a[a.index('--banks') + 1]) if '--banks' in a else 4
        sys.exit(1 if cmd_resolve(a[1], nb) else 0)
    elif a[0] == 'merge':
        pf = a[a.index('--prefix') + 1] if '--prefix' in a else 'pod_'
        cmd_merge(a[1], a[2], a[3], pf)
    else: selftest()
