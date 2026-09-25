"""
startup_slot.py - take crt0's startup call out of the plugin hook's delay slot.

The problem
-----------
ps2plugininjector hooks startup by turning crt0's `ei` into `jal INVOKER`, the
plugin's entry point, so the word after that `ei` becomes the jal's delay slot.
In Gran Turismo 4 Online, and in every Tourist Trophy build, that word is
crt0's own `jal` into the game's early setup: a jump in a delay slot. MIPS
leaves that undefined, and the two machines settle it differently:

  PCSX2  skips the jump in the slot. INVOKER runs, and its `addiu $ra, -4`
         returns to the slot, so crt0's call is made after all.
  PS2    takes the jump in the slot after one instruction of INVOKER - its
         `ei` - and returns into crt0 from there. init() never runs, so no
         hook is installed, and the game boots as if unmodified.

The fix
-------
With --slot and --call, the word in the slot is checked - it must be crt0's
`jal <call>`, in the base image too - and made a nop, and the game's INVOKER
makes that call itself once init() is done. The values come from
STARTUP_SLOT_<region> and STARTUP_CALL_<region> in the game's game.mk. Without
them, a jump in the slot is only reported, as a warning: that executable's
hooks will not run on a PS2.

Running it twice is harmless: a slot that is already a nop is left alone.

Usage:  python tools/startup_slot.py <injected.elf> <plugin.elf> [--base <base image>]
                                     [--slot 0x1001FC --call 0x4DA490]
"""

import argparse
import struct
import sys

PT_LOAD = 1
PF_X = 1
INSN_EI = 0x42000038
INSN_NOP = 0x00000000


def load_segments(d):
    """The PT_LOADs of an ELF image, as [type, offset, vaddr, paddr, filesz, memsz, flags, align]."""
    phoff = struct.unpack_from("<I", d, 0x1C)[0]
    phentsize, phnum = struct.unpack_from("<HH", d, 0x2A)
    segs = [list(struct.unpack_from("<8I", d, phoff + i * phentsize)) for i in range(phnum)]
    return [s for s in segs if s[0] == PT_LOAD]


def file_offset(d, addr):
    """Where the word at addr lies in the file, or None if no file image holds it."""
    for s in load_segments(d):
        if s[2] <= addr and addr + 4 <= s[2] + s[4]:
            return s[1] + (addr - s[2])
    return None


def word(d, addr):
    off = file_offset(d, addr)
    return None if off is None else struct.unpack_from("<I", d, off)[0]


def jal(target):
    return 0x0C000000 | ((target >> 2) & 0x3FFFFFF)


def is_jump(w):
    """Whether a word is a jump or branch - something that has a delay slot of its own."""
    op = w >> 26
    if op in (0x02, 0x03) or 0x04 <= op <= 0x07 or 0x14 <= op <= 0x17:
        return True                                     # j, jal, beq..bgtz, beql..bgtzl
    if op == 0x01:
        return ((w >> 16) & 31) in (0x00, 0x01, 0x02, 0x03, 0x10, 0x11, 0x12, 0x13)
    if op == 0x00:
        return (w & 63) in (0x08, 0x09)                 # jr, jalr
    if op in (0x10, 0x11, 0x12):
        return ((w >> 21) & 31) == 0x08                 # bc0x, bc1x, bc2x
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", help="the injected executable; modified in place")
    ap.add_argument("plugin", help="the plugin.elf that was injected into it")
    ap.add_argument("--base", help="the untouched base image, to check the slot against")
    ap.add_argument("--slot", help="the hook's delay slot, where crt0's call is")
    ap.add_argument("--call", help="the address crt0 calls from the slot; INVOKER calls it instead")
    args = ap.parse_args()

    with open(args.plugin, "rb") as f:
        plugin = f.read()
    with open(args.elf, "rb") as f:
        d = bytearray(f.read())
    base = None
    if args.base:
        with open(args.base, "rb") as f:
            base = f.read()
    for path, blob in ((args.plugin, plugin), (args.elf, d), (args.base, base)):
        if blob is not None and blob[:4] != b"\x7fELF":
            print("not an ELF: %s" % path)
            return 1

    # The plugin is linked with INVOKER as its entry, and that is what the
    # injector calls: find the one jal to it in the executable's code.
    invoker = struct.unpack_from("<I", plugin, 0x18)[0]
    hook = jal(invoker)
    sites = []
    for s in load_segments(d):
        if not s[6] & PF_X:
            continue
        for i in range(0, s[4] - 3, 4):
            if struct.unpack_from("<I", d, s[1] + i)[0] == hook:
                sites.append(s[2] + i)
    if len(sites) != 1:
        print("expected one jal to INVOKER (0x%08X), found %d" % (invoker, len(sites)))
        return 1
    site = sites[0]
    if base is not None and word(base, site) != INSN_EI:
        print("the hook at 0x%08X is not over an `ei` in the base image" % site)
        return 1

    slot = site + 4
    now = word(d, slot)
    print("  startup hook: jal INVOKER (0x%08X) at 0x%08X; its delay slot 0x%08X holds %08X"
          % (invoker, site, slot, now))

    if not args.slot:
        if is_jump(now):
            print("WARNING: that is a jump in the hook's delay slot. A PS2 takes it and never runs"
                  " INVOKER, so none of this executable's hooks will be installed on a console."
                  " Give the game STARTUP_SLOT and STARTUP_CALL in its game.mk and have INVOKER"
                  " make the call.")
        return 0

    if not args.call:
        print("--slot needs --call: the address crt0 calls from the slot")
        return 1
    want_slot = int(args.slot, 0)
    call = int(args.call, 0)
    if slot != want_slot:
        print("the hook's delay slot is 0x%08X, not %s" % (slot, args.slot))
        return 1
    if base is not None and word(base, slot) != jal(call):
        print("the base image holds %08X at 0x%08X, not jal 0x%08X"
              % (word(base, slot), slot, call))
        return 1
    if now == INSN_NOP:
        print("  already a nop - left alone")
        return 0
    if now != jal(call):
        print("0x%08X holds %08X, not jal 0x%08X - refusing to change it" % (slot, now, call))
        return 1

    struct.pack_into("<I", d, file_offset(d, slot), INSN_NOP)
    with open(args.elf, "wb") as f:
        f.write(d)
    print("  crt0's jal 0x%08X moved out of the slot: now a nop, and INVOKER makes the call" % call)
    return 0


if __name__ == "__main__":
    sys.exit(main())
