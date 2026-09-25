"""
inject_cave2.py - add one of a plugin's extra caves to an injected ELF.

The problem
-----------
ps2plugininjector copies exactly one section of the plugin - .text - into one
new PT_LOAD. A game can link parts of its plugin somewhere else as well: Gran
Turismo 4 Online puts the hooks that no longer fit its IME cave over code it
never runs, as sections named .cave2, .cave2b and .cave2c (see
source/games/gt4o/linkfile). The injector knows nothing of them, so this writes
one in afterwards, named with --section.

In place
--------
A cave linked over dead code or data the image already carries needs no new
segment: the section's bytes are written straight into that segment's file
image. With --base, the bytes being replaced are first compared with the
untouched base image, so a range that is not what it is supposed to be - or was
already patched by something else - is refused rather than overwritten. This is
what Gran Turismo 4 Online uses.

A new segment
-------------
A cave anywhere else is added as one more PT_LOAD, after all the others. If it
lies inside the declared p_memsz tail of an existing segment - memory a loader
zero-fills, such as the space just past .bss - that tail is split at the cave:
the old segment now stops at the cave's first byte, and the new one carries the
zero-fill on to where the old one used to end. Every byte that was zero-filled
before still is, no two segments overlap, and the result does not depend on the
order a loader fills tails and copies segments in.

Running it twice is harmless: a cave that is already there is left alone. An
empty section is nothing to add.

Usage:  python tools/inject_cave2.py <injected.elf> <plugin.elf>
                                     [--section .cave2] [--addr 0x48D780]
                                     [--reserve 0x38E0] [--base <base image>]
"""

import argparse
import struct
import sys

PT_LOAD = 1
SHT_NOBITS = 8
PF_RWX = 7
SEG_ALIGN = 0x80


def section(d, name):
    """(sh_type, sh_addr, sh_offset, sh_size) of the named section, or None."""
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
    strtab = struct.unpack_from("<I", d, shoff + shstrndx * shentsize + 16)[0]
    for i in range(shnum):
        sh_name, sh_type, _flags, sh_addr, sh_offset, sh_size = struct.unpack_from(
            "<6I", d, shoff + i * shentsize
        )
        start = strtab + sh_name
        if d[start:d.index(b"\0", start)] == name.encode():
            return sh_type, sh_addr, sh_offset, sh_size
    return None


def file_regions(d, segs):
    """File offsets something already lives at: segment images and sections."""
    used = [s[1] for s in segs if s[4]]
    shoff = struct.unpack_from("<I", d, 0x20)[0]
    shentsize, shnum = struct.unpack_from("<HH", d, 0x2E)
    if shoff:
        used.append(shoff)
    for i in range(shnum):
        sh_type, = struct.unpack_from("<I", d, shoff + i * shentsize + 4)
        sh_offset, sh_size = struct.unpack_from("<II", d, shoff + i * shentsize + 16)
        if sh_type != SHT_NOBITS and sh_size:
            used.append(sh_offset)
    return used


def load_segments(d):
    """The PT_LOADs of an ELF image, as [type, offset, vaddr, paddr, filesz, memsz, flags, align]."""
    phoff = struct.unpack_from("<I", d, 0x1C)[0]
    phentsize, phnum = struct.unpack_from("<HH", d, 0x2A)
    segs = [list(struct.unpack_from("<8I", d, phoff + i * phentsize)) for i in range(phnum)]
    return [s for s in segs if s[0] == PT_LOAD]


def file_bytes(d, addr, size):
    """The file bytes behind [addr, addr + size) if one PT_LOAD's file image holds all of them."""
    for s in load_segments(d):
        if s[2] <= addr and addr + size <= s[2] + s[4]:
            off = s[1] + (addr - s[2])
            return off, s
    return None, None


def in_place(args, d, off, host, addr, data):
    """Write the cave over bytes the image already carries."""
    size = len(data)
    if bytes(d[off:off + size]) == data:
        print("  %s is already in place at 0x%08X - left alone" % (args.section, addr))
        return 0

    if args.base:
        with open(args.base, "rb") as f:
            base = f.read()
        boff, _ = file_bytes(base, addr, size)
        if boff is None:
            print("the base image has no file bytes at 0x%08X..0x%08X" % (addr, addr + size))
            return 1
        if bytes(d[off:off + size]) != base[boff:boff + size]:
            print("0x%08X..0x%08X does not hold what the base image does there -"
                  " refusing to overwrite it" % (addr, addr + size))
            return 1

    print("  in place: 0x%08X..0x%08X, file offset 0x%X, inside the segment at 0x%08X"
          % (addr, addr + size, off, host[2]))
    if args.dry_run:
        print("(dry run) nothing written")
        return 0

    d[off:off + size] = data
    with open(args.elf, "wb") as f:
        f.write(d)
    print("wrote %s in place in %s" % (args.section, args.elf))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", help="the injected executable; modified in place")
    ap.add_argument("plugin", help="the plugin.elf that was injected into it")
    ap.add_argument("--addr", help="where the section must be linked, as a cross-check")
    ap.add_argument("--reserve", help="bytes reserved for it, as a cross-check")
    ap.add_argument("--section", default=".cave2")
    ap.add_argument("--base", help="the untouched base image, to check bytes replaced in place")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    with open(args.plugin, "rb") as f:
        plugin = f.read()
    with open(args.elf, "rb") as f:
        d = bytearray(f.read())
    for path, blob in ((args.plugin, plugin), (args.elf, d)):
        if blob[:4] != b"\x7fELF":
            print("not an ELF: %s" % path)
            return 1

    sec = section(plugin, args.section)
    if sec is None:
        print("%s has no %s section" % (args.plugin, args.section))
        return 1
    sh_type, addr, offset, size = sec
    reserve = int(args.reserve, 0) if args.reserve else None
    if args.addr and addr != int(args.addr, 0):
        print("%s is linked at 0x%08X, not %s" % (args.section, addr, args.addr))
        return 1
    if reserve is not None and size > reserve:
        print("%s is 0x%X bytes, over its 0x%X reserve" % (args.section, size, reserve))
        return 1
    if size == 0:
        print("%s is empty - nothing to add" % args.section)
        return 0
    data = bytes(size) if sh_type == SHT_NOBITS else plugin[offset:offset + size]
    if reserve is not None:
        print("  %s: %d of %d bytes" % (args.section, size, reserve))

    # Linked over bytes the image already carries: write them there.
    off, host = file_bytes(d, addr, size)
    if off is not None:
        return in_place(args, d, off, host, addr, data)

    phoff = struct.unpack_from("<I", d, 0x1C)[0]
    phentsize, phnum = struct.unpack_from("<HH", d, 0x2A)
    # p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align
    segs = [list(struct.unpack_from("<8I", d, phoff + i * phentsize)) for i in range(phnum)]

    tail = None
    for i, s in enumerate(segs):
        if s[0] != PT_LOAD:
            continue
        if s[2] == addr and s[4] == size and s[1] + size <= len(d) \
                and d[s[1]:s[1] + size] == data:
            print("  already carries %s at 0x%08X - left alone" % (args.section, addr))
            return 0
        if s[2] < addr + size and addr < s[2] + s[4]:
            print("0x%08X..0x%08X overlaps the file image of the segment at 0x%08X"
                  % (addr, addr + size, s[2]))
            return 1
        if s[2] + s[4] <= addr < s[2] + s[5]:
            tail = i

    memsz = size
    if tail is not None:
        t = segs[tail]
        memsz = max(size, t[2] + t[5] - addr)
        print("  PT_LOAD vaddr=0x%08X filesz=0x%-8X memsz=0x%X -> 0x%X  <- its tail now stops at the cave"
              % (t[2], t[4], t[5], addr - t[2]))
        t[5] = addr - t[2]

    for i, s in enumerate(segs):
        if s[0] == PT_LOAD and i != tail and s[2] < addr + memsz and addr < s[2] + s[5]:
            print("the cave's memory 0x%08X..0x%08X overlaps the segment at 0x%08X"
                  % (addr, addr + memsz, s[2]))
            return 1

    # One more program header has to fit between the table and the first thing
    # after it, in bytes nothing uses.
    slot = phoff + phnum * phentsize
    first = min(o for o in file_regions(d, segs) if o >= slot)
    if slot + phentsize > first or any(d[slot:slot + phentsize]):
        print("no room for another program header at 0x%X" % slot)
        return 1

    align = SEG_ALIGN if addr % SEG_ALIGN == 0 else 4
    new_off = len(d) + (addr - len(d)) % align
    used = " (%d of %d bytes)" % (size, reserve) if reserve is not None else ""
    print("  PT_LOAD vaddr=0x%08X filesz=0x%-8X memsz=0x%X  <- %s%s"
          % (addr, size, memsz, args.section, used))

    if args.dry_run:
        print("(dry run) nothing written")
        return 0

    d += bytes(new_off - len(d))
    d += data
    for i, s in enumerate(segs):
        struct.pack_into("<8I", d, phoff + i * phentsize, *s)
    struct.pack_into("<8I", d, slot, PT_LOAD, new_off, addr, addr, size, memsz, PF_RWX, align)
    struct.pack_into("<H", d, 0x2C, phnum + 1)
    with open(args.elf, "wb") as f:
        f.write(d)
    print("added %s to %s" % (args.section, args.elf))
    return 0


if __name__ == "__main__":
    sys.exit(main())
