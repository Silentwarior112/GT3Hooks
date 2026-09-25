"""
align_segments.py - place an injected ELF's segments where the PS2 BIOS can load them.

The problem
-----------
The BIOS does not map an executable into memory; it copies it. EELOAD has the
IOP's XLOADFILE ("loadelf version 3.30") load it, and XLOADFILE reads the file
from offset 0 in blocks of 64 KB, into two buffers in turn, and sends each
PT_LOAD to the EE by SIF DMA one piece at a time: the part of the segment in
the current block, to p_vaddr plus the bytes already sent. The EE end of that
DMA writes whole quadwords at a 16-aligned address only, so a piece whose
destination is not 16-aligned is written (destination & 15) bytes low.

A piece can only start misaligned at a block boundary, and only when the
segment's file offset is not congruent to its address modulo 16. So such a
segment loads intact while it sits inside one 64 KB block of the file, and
everything past a boundary arrives shifted once it straddles one.
ps2plugininjector puts the plugin's segment straight after the game's data at
an 8-aligned offset, which left it to chance:

  GT3 US   file 0x2541E8..0x25EEEC   inside one block          loads intact
  GT3 EU   file 0x255B88..0x26087C   crosses 0x260000          the last 0x87C
           bytes, the plugin's function pointers, land 8 bytes low; _sprintf
           reads 0, and init()'s first LOG jumps to address 0

PCSX2 runs the BIOS's own EELOAD and XLOADFILE, so it fails the same way as a
console.

The fix
-------
The file is padded in front of every segment whose offset is not congruent to
its address modulo 0x80 - 16 is what the DMA needs, 0x80 is the linkfile's
alignment and inject_cave2.py's rule for the segments it adds - and every later
segment, section and the section header table move with it. What is loaded,
and where, does not change.

Then the load is replayed as XLOADFILE performs it, for every buffer size it
can fall back to when memory is short (64 KB down to 512 bytes), and every
segment must arrive exactly as the file says. A segment whose address is not
16-aligned cannot be loaded correctly at all and is refused.

Running it twice is harmless: a file already in place is left alone.

Usage:  python tools/align_segments.py <injected.elf>
"""

import argparse
import struct
import sys

PT_LOAD = 1
SHT_NULL = 0
SHT_NOBITS = 8
SEG_ALIGN = 0x80
DMA_ALIGN = 16

# XLOADFILE asks for 128 KB and halves the request up to seven times if the IOP
# cannot supply it; each of its two buffers is half of what it got.
BUFFER_SIZES = [0x10000 >> k for k in range(8)]


def headers(d):
    e_phoff, e_shoff = struct.unpack_from("<II", d, 0x1C)
    phentsize, phnum, shentsize, shnum = struct.unpack_from("<HHHH", d, 0x2A)
    return e_phoff, phentsize, phnum, e_shoff, shentsize, shnum


def segments(d):
    """The PT_LOADs with file bytes, as (offset, vaddr, filesz), in file order."""
    phoff, phentsize, phnum = headers(d)[:3]
    out = []
    for i in range(phnum):
        t, off, va, _pa, fsz = struct.unpack_from("<5I", d, phoff + i * phentsize)
        if t == PT_LOAD and fsz:
            out.append((off, va, fsz))
    return sorted(out)


def insert(d, at, n):
    """Open n zero bytes at file offset at; everything from there on moves up by n."""
    phoff, phentsize, phnum, shoff, shentsize, shnum = headers(d)
    if phoff + phnum * phentsize > at:
        return "the program header table reaches past 0x%X" % at
    for off, va, fsz in segments(d):
        if off < at < off + fsz:
            return "0x%X is inside the segment at 0x%08X" % (at, va)
    for i in range(shnum):
        sh_type, = struct.unpack_from("<I", d, shoff + i * shentsize + 4)
        sh_offset, sh_size = struct.unpack_from("<II", d, shoff + i * shentsize + 16)
        if sh_type not in (SHT_NULL, SHT_NOBITS) and sh_offset < at < sh_offset + sh_size:
            return "0x%X is inside section %d" % (at, i)

    d[at:at] = bytes(n)
    for i in range(phnum):
        p = phoff + i * phentsize + 4
        off, = struct.unpack_from("<I", d, p)
        if off >= at:
            struct.pack_into("<I", d, p, off + n)
    if shoff >= at:
        shoff += n
        struct.pack_into("<I", d, 0x20, shoff)
    for i in range(shnum):
        p = shoff + i * shentsize
        sh_type, = struct.unpack_from("<I", d, p + 4)
        off, = struct.unpack_from("<I", d, p + 16)
        if sh_type != SHT_NULL and off >= at:
            struct.pack_into("<I", d, p + 16, off + n)
    return None


def xloadfile(d, bufsize):
    """Replay XLOADFILE's copy of every PT_LOAD with the given buffer size.

    Returns (base, memory) - memory as loaded, from address base - and the pieces,
    as (file offset, bytes, destination, where it really landed)."""
    segs = segments(d)
    base = min(va for _off, va, _fsz in segs) & ~(DMA_ALIGN - 1)
    top = max(va + fsz for _off, va, fsz in segs) + 2 * DMA_ALIGN
    mem = bytearray(top - base)
    pieces = []
    for off, va, fsz in segs:                           # XLOADFILE sorts them by offset
        done = 0
        while done < fsz:
            pos = off + done
            block = pos - pos % bufsize                 # the buffer that holds pos
            n = min(block + bufsize - pos, fsz - done)
            src = pos & ~3                              # the IOP DMAs from a word address
            size = (n + DMA_ALIGN - 1) & ~(DMA_ALIGN - 1)
            data = bytes(d[src:min(src + size, block + bufsize)])
            data += b"\xEE" * (size - len(data))        # past the buffer: unknown bytes
            dest = va + done
            real = dest & ~(DMA_ALIGN - 1)              # the EE writes whole quadwords
            mem[real - base:real - base + size] = data
            pieces.append((pos, n, dest, real))
            done += n
    return base, mem, pieces


def check_load(d):
    """Problems a replay of the BIOS load finds, at every buffer size: [] if none."""
    problems = []
    for bufsize in BUFFER_SIZES:
        base, mem, pieces = xloadfile(d, bufsize)
        for off, va, fsz in segments(d):
            got = mem[va - base:va - base + fsz]
            if got != d[off:off + fsz]:
                first = next(i for i in range(fsz) if got[i] != d[off + i])
                problems.append("%d KB buffers: the segment at 0x%08X arrives wrong from 0x%08X on"
                                % (bufsize >> 10, va, va + first))
        if problems:
            for pos, n, dest, real in pieces:
                if dest != real:
                    problems.append("  file 0x%X+0x%X for 0x%08X lands at 0x%08X" % (pos, n, dest, real))
            break
    return problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", help="the injected executable; modified in place")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    with open(args.elf, "rb") as f:
        d = bytearray(f.read())
    if d[:4] != b"\x7fELF":
        print("not an ELF: %s" % args.elf)
        return 1

    for off, va, fsz in segments(d):
        if va % DMA_ALIGN:
            print("the segment at 0x%08X is not 16-aligned: the BIOS cannot load it intact" % va)
            return 1

    moved = 0
    while True:
        bad = [(off, va) for off, va, _fsz in segments(d) if (va - off) % SEG_ALIGN]
        if not bad:
            break
        off, va = bad[0]
        n = (va - off) % SEG_ALIGN
        err = insert(d, off, n)
        if err:
            print("cannot move the segment at 0x%08X: %s" % (va, err))
            return 1
        print("  PT_LOAD vaddr=0x%08X: file offset 0x%X -> 0x%X, congruent to its address mod 0x%X"
              % (va, off, off + n, SEG_ALIGN))
        moved += 1

    problems = check_load(d)
    if problems:
        print("the BIOS would not load this executable as the file says:")
        for p in problems:
            print("  " + p)
        return 1
    print("  BIOS load replayed with %s-byte buffers: every segment arrives intact"
          % "/".join("%X" % b for b in BUFFER_SIZES))

    if not moved:
        return 0
    if args.dry_run:
        print("(dry run) nothing written")
        return 0
    with open(args.elf, "wb") as f:
        f.write(d)
    print("  wrote %s" % args.elf)
    return 0


if __name__ == "__main__":
    sys.exit(main())
