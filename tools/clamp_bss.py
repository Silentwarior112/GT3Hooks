"""
clamp_bss.py - Post-build fix-up for an injected Tourist Trophy ELF.

The problem
-----------
Tourist Trophy's second PT_LOAD declares a large .bss tail: p_filesz ends at
0x61B5FC but p_memsz runs to 0xE1B5FC (US). The plugin is linked at 0x7A6A00,
which is inside that declared tail. The injector appends the plugin's own
PT_LOAD after it, so a loader that walks segments in order would write the
plugin last and everything would be fine - but a loader that zero-fills every
segment's memsz tail after loading them all would wipe the plugin instead.

Rather than depend on which of those a given loader does, this clamps p_memsz
down to p_filesz on the original segment. That is safe because the executable
does not rely on the loader to clear .bss: crt0 zeroes _fbss.._end itself with
an explicit loop before anything else runs.

Only segments that do NOT contain the plugin are clamped, and only where
p_memsz > p_filesz, so running this twice is harmless.

Usage:  python tools/clamp_bss.py <injected.elf> [--base 0x7A6A00]
"""

import argparse
import struct
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--base", help="plugin link address, so its segment is left alone")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    base = int(args.base, 0) if args.base else None

    with open(args.elf, "rb") as f:
        d = bytearray(f.read())

    if d[:4] != b"\x7fELF":
        print("not an ELF: %s" % args.elf)
        return 1

    phoff = struct.unpack_from("<I", d, 0x1C)[0]
    phentsize = struct.unpack_from("<H", d, 0x2A)[0]
    phnum = struct.unpack_from("<H", d, 0x2C)[0]

    changed = 0
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz = struct.unpack_from(
            "<IIIIII", d, off
        )
        if p_type != 1:
            continue
        # The plugin's segment is the one with real file content at the link
        # address. The original segment only covers it via its memsz tail -
        # which is precisely the tail being clamped - so this must test filesz,
        # not memsz, or the wrong segment is spared.
        holds_plugin = base is not None and p_vaddr <= base < p_vaddr + p_filesz
        tag = ""
        if holds_plugin:
            tag = "  <- holds the plugin, left alone"
        elif p_memsz > p_filesz:
            tag = "  <- clamping memsz 0x%X -> 0x%X" % (p_memsz, p_filesz)
            if not args.dry_run:
                struct.pack_into("<I", d, off + 20, p_filesz)
            changed += 1
        print("  PT_LOAD vaddr=0x%08X filesz=0x%-8X memsz=0x%-8X%s"
              % (p_vaddr, p_filesz, p_memsz, tag))

    if changed and not args.dry_run:
        with open(args.elf, "wb") as f:
            f.write(d)
        print("clamped %d segment(s) in %s" % (changed, args.elf))
    elif changed:
        print("(dry run) would clamp %d segment(s)" % changed)
    else:
        print("nothing to clamp")
    return 0


if __name__ == "__main__":
    sys.exit(main())
