"""
check_game.py - Verify a game's generated target headers against the real
executables.

Every address in this project was derived by pattern matching against another
build. That is reliable, but it is inference: a mistyped or mis-resolved value
produces a header that compiles perfectly and crashes the game. This re-checks
each one against the actual image, which is the only thing that can catch it.

What it checks, per game and per build:

  * every ADDR_* that names a FUNCTION points inside .text and begins with a
    plausible instruction rather than data or padding;
  * every ADDR_* that names a POINTER SLOT lives in the data segment and
    currently holds an address inside .text;
  * ADDR_HOutput_Handler specifically holds a `jr $ra` stub, which is what makes
    the HOutput hook meaningful;
  * the injection parameters agree with the image - the pool base global really
    holds the value the header claims, and the memset site really is a `jal`;
  * game.mk's BASE_<REGION> matches the header's PLUGIN_BASE_ADDRESS, so the
    linker and the code that reserves the memory cannot drift apart.

Usage:  python tools/check_game.py [game ...]      (default: all games)
"""

import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ttanalyze import Image, disasm  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)          # the repo
IMAGES = os.path.dirname(ROOT)        # game images live beside the repo
GAMES = os.path.join(ROOT, "source", "games")

# Where to find a decompressed image for a build id. Both layouts are tried.
IMAGE_DIRS = ["GT4exe_bin/extracted", "TTexe_bin/extracted"]

DEFINE = re.compile(r"^#define\s+(\w+)\s+(0x[0-9A-Fa-f]+)\s*$", re.M)

# ADDR_* symbols that name a pointer slot rather than a function.
SLOT_SYMBOLS = {
    "ADDR_PlayStation2_FileDeviceRo_openStream",
    "ADDR_PlayStation2_FileDeviceRo_closeStream",
    "ADDR_PlayStation2_FileDeviceRo2_GetFileEntry",
    "ADDR_PlayStation2_FileDeviceRo2_readStat",
    "ADDR_PDISTD_FileDevice_readStream",
    "ADDR_HOutput_Handler",
}

# ADDR_* symbols that name neither - they are globals holding data.
DATA_SYMBOLS = {
    "ADDR_UnitArena_StreamState",
    "ADDR_UnitArena_StateField0C",
    "ADDR_MStorageMC_Global",
    "ADDR_std_string_EmptyRep",
    "ADDR_std_string_EmptyRep_Refcount",
    "ADDR_std_string_EmptyRep_Flag",
    "ADDR_std_string_EmptyRep_Data",
}


def find_image(build):
    for d in IMAGE_DIRS:
        p = os.path.join(IMAGES, d, "%s.elf" % build)
        if os.path.exists(p):
            return p
    return None


def parse(path):
    with open(path, encoding="utf-8") as f:
        return {m.group(1): int(m.group(2), 0) for m in DEFINE.finditer(f.read())}


def regions_of(game):
    """[(region, build)] from a game's game.mk, plus its BASE_<REGION> values."""
    mk = os.path.join(GAMES, game, "game.mk")
    text = open(mk, encoding="utf-8").read()
    m = re.search(r"^REGIONS\s*:?=\s*(.+)$", text, re.M)
    out = []
    for r in (m.group(1).split() if m else []):
        b = re.search(r"^BUILD_%s\s*:?=\s*(\S+)" % r, text, re.M)
        base = re.search(r"^BASE_%s\s*:?=\s*(0x[0-9A-Fa-f]+)" % r, text, re.M)
        out.append((r, b.group(1) if b else None,
                    int(base.group(1), 0) if base else None))
    return out


def looks_like_code(img, addr):
    ins = disasm(img, addr, 1)
    return bool(ins) and ins[0].mnemonic not in ("nop",)


def check_build(game, region, build, mk_base):
    hdr = os.path.join(GAMES, game, "Targets", "%s.h" % build)
    if not os.path.exists(hdr):
        return ["%s/%s: no target header at %s" % (game, region, hdr)]

    elf = find_image(build)
    if elf is None:
        print("    %s/%-3s  SKIPPED - no decompressed image for %s" % (game, region, build))
        return []

    d = parse(hdr)
    img = Image(elf)
    tb, tblob = img.text
    tlo, thi = tb, tb + len(tblob)
    fails = []
    n_fn = n_slot = 0

    for sym, val in sorted(d.items()):
        if not sym.startswith("ADDR_"):
            continue
        if sym in DATA_SYMBOLS:
            if img.read(val, 4) is None:
                fails.append("%s/%s: %s 0x%X is outside the image" % (game, region, sym, val))
            continue
        if sym in SLOT_SYMBOLS:
            n_slot += 1
            if tlo <= val < thi:
                fails.append("%s/%s: %s 0x%X is a slot but sits in .text" % (game, region, sym, val))
                continue
            held = img.u32(val)
            if held is None or not (tlo <= held < thi):
                fails.append("%s/%s: %s 0x%X holds 0x%s, not a .text pointer"
                             % (game, region, sym, val, "%X" % held if held else "?"))
            elif sym == "ADDR_HOutput_Handler":
                ins = disasm(img, held, 1)
                if not ins or ins[0].mnemonic != "jr":
                    fails.append("%s/%s: %s target 0x%X is not the expected jr-$ra stub"
                                 % (game, region, sym, held))
            continue
        # everything else should be a function
        n_fn += 1
        if not (tlo <= val < thi):
            fails.append("%s/%s: %s 0x%X is not inside .text" % (game, region, sym, val))
        elif not looks_like_code(img, val):
            fails.append("%s/%s: %s 0x%X does not decode as code" % (game, region, sym, val))

    # ---- injection parameters ----
    site = d.get("GLOBAL_MEMSET_OFFSET")
    if site is not None:
        w = img.u32(site)
        if w is None or (w >> 26) != 0x03:
            fails.append("%s/%s: GLOBAL_MEMSET_OFFSET 0x%X is not a jal" % (game, region, site))

    pbg, pbv = d.get("POOL_BASE_GLOBAL"), d.get("POOL_BASE_VALUE")
    if pbg and pbv:
        actual = img.u32(pbg)
        if actual != pbv:
            fails.append("%s/%s: POOL_BASE_GLOBAL 0x%X holds 0x%X, header says 0x%X"
                         % (game, region, pbg, actual or 0, pbv))

    # ---- the link address must match game.mk ----
    hdr_base = d.get("PLUGIN_BASE_ADDRESS")
    if hdr_base is not None and mk_base is not None and hdr_base != mk_base:
        fails.append("%s/%s: game.mk BASE_%s 0x%X != header PLUGIN_BASE_ADDRESS 0x%X"
                     % (game, region, region, mk_base, hdr_base))

    if not fails:
        print("    %s/%-3s  %-12s OK  (%d functions, %d slots)" % (game, region, build, n_fn, n_slot))
    return fails


def main():
    wanted = sys.argv[1:]
    games = sorted(g for g in os.listdir(GAMES)
                   if os.path.exists(os.path.join(GAMES, g, "game.mk")))
    if wanted:
        games = [g for g in games if g in wanted]
        if not games:
            print("No such game. Available: %s" % ", ".join(
                sorted(os.listdir(GAMES))))
            return 1

    all_fails = []
    for game in games:
        print("  %s" % game)
        for region, build, mk_base in regions_of(game):
            if build is None:
                all_fails.append("%s/%s: game.mk has no BUILD_%s" % (game, region, region))
                continue
            all_fails += check_build(game, region, build, mk_base)

    print()
    if all_fails:
        print("FAILURES:")
        for f in all_fails:
            print("  " + f)
        return 1
    print("All checked addresses agree with the executables.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
