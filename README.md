# GT3Hooks

Code hooks for Gran Turismo 3, injected with
[PS2 Plugin Injector](https://github.com/ermaccer/ps2plugininjector).

The architecture is one shared core, a thin layer per game, and every
address literal in a single per-build header under `Targets/`.

## Features
- **Toyota Pod** — face, tail, color, squat, sounds <br>
Transplanting GT Concept's sound files is required to get the Pod's sounds.
- **Gearbox types** — CVT (Type 1), Clutchless-stepped (Type 2), Electric single speed (Type 3). <br>
Requires appending a column to the ParamDB's GEAR table: 0x30 as a byte. <br>
- **4wd types** — Rear-drive hybrid, crank motor, front motors, battery (Type 5) <br>

## Decompressing a GT3 ELF for custom builds
An updated [ELFBuilderTool](https://github.com/Silentwarior112/PDTools/releases/tag/GT4ElfBuilderTool-gt3fix_1.0) is required to handle `core.gt3` correctly.

## Building with `build.bat`
[This ps2sdk](https://github.com/ThirteenAG/ps2sdk) is required.

```
build                                        list the builds
gt3                                          The game: Gran Turismo 3
EU                                           The region: EU (SCES-50294)
for:ps2                                      The target platform: Standard PS2 w/ 32MB of RAM
out:path\to\elf                              The ELF output: Optional, this will write the ELF here in addition to the repo's work/out directory.
build gt3 US for:ps2 out:path\to\elf         A full command. Wrap the out: part with "" if there are spaces in the path like so: "out:path\to\elf"
build clean gt3 EU                           Deletes the files in the repo's work/out directory.
```

## Regions: US and EU

EU is the US compile relinked, so its target header,
`Targets/SCES-50294.h`, is **generated** from the US one. Change
`SCUS-97102.h`, then regenerate:

```
GT3Hooks-main\tools\gt3\port_target.py source\games\gt3\Targets\SCUS-97102.h ^
    ..\GT3Hooks-work\bases\BASE_SCUS-97102 ..\GT3Hooks-work\bases\BASE_SCES-50294 ^
    --build SCES-50294 --region EU --elf SCES_502.94 ^
    --title "Gran Turismo 3: A-Spec (Europe)" ^
    -o source\games\gt3\Targets\SCES-50294.h --report port_EU.txt
```

## Technical information:
### What GT3 needs that the GT4 family does not

**A different `ei` encoding.** GT3's crt0 uses the EE form `ei = 0x42000038`,
not the MIPS32r2 form `0x41606020`. ps2plugininjector's default pattern
`"38 00 00 42"` already matches it, so no `-p` or `-n` argument is needed. The
hook site is `0x00100094` in all three GT3 builds.

**`$v0` preserved, not a nopped delay slot.** The two treatments are inverted
from GT4/TT. Their delay slot holds crt0's call into the game and has to be
nopped; GT3 US/EU hold `lui $v0, 0x35`, which is harmless to run early — but
`$v0` is live across the hook and carries the argv block into `main()`.
GT4Hooks' compiled `INVOKER` destroys it. `source/games/gt3/Invoker.S` saves and
restores it, and is written in assembly because GCC has no
`__attribute__((naked))` for MIPS.

### Where the plugin lives

Linked at `0x004B2C80` — `_end` rounded up to 128 — with `init()` raising the
engine pool's base word past it, the same trick Tourist Trophy uses. It is
easier here: TT's pool base is an immediate in code, GT3's is a writable data
word (`0x003531D0`).

The pool is built lazily on the first `malloc`/`free`, not at startup, so it
does not exist yet when the hook runs. The pool's own `memset` is deliberately
**not** NOPed: raising the base already moves the clear above the plugin.

The reserve is 64 KB. GT4 Online blue-clocked after losing 32 KB of its 23 MB
pool and GT3's headroom within its 26.30 MB is unmeasured, so this starts
smaller than TT's 128 KB. The plugin currently uses 48 KB of it, `.bss`
included — the build is `-O0` throughout, and the gearbox and Dualnote physics
are most of it. There is still 17 KB to spare; if a later feature needs more,
raise `PLUGIN_RESERVE` in `game.mk` (and `PLUGIN_RESERVE_BYTES`/`POOL_BASE_NEW`
in the target header with it).

`init()` ends by flushing the caches, so that the code it patched is what the CPU
fetches on a real console.

### Layout

```
mk/          build machinery, game-agnostic (from GT4Hooks)
tools/       startup_slot.py, clamp_bss.py, inject_cave2.py, check_game.py,
             align_segments.py (places each segment where the BIOS loader
             can copy it intact, and proves it - FINDINGS.md section 16)
tools/gt3/   gt3pack.py (core.gt3 <-> ELF), port_target.py (US header -> EU
             header, every value checked), seinf_tool.py (sound/se.inf),
             and the ELF and injection checkers
source/
  core/      compiled into every game; no address literal, no game name
    Target.h   the seam - includes the selected build's Targets/ header
    Log.h      LOG() over the EE SIO port
    ps2/       Memory.c (hook primitives), Sio.c
    game/      pointers bound into the game's own code
  games/gt3/
    Invoker.S    the entry point; preserves $v0
    main.c       init(): the pool reserve, the hooks, the cache flush
    GameConfig.h every feature's switches
    Pod.c        the Pod: emotion machine, face, tail, squat
    PodSound.c   the Pod's voice: SE table, fourth bank, playback
    PodThunk.S   the Pod's calls that pass a float to or from the game
    Drive.h      the gearbox types and the Dualnote: shared declarations
    Drive.c        their installer and the call sites both use
    Gearbox.c      the gearbox types
    Hybrid.c       the Honda Dualnote
    DriveThunk.S   their thunks and float calls
    Targets/     one header per build - the only place addresses live;
                 SCES-50294.h (EU) is generated from SCUS-97102.h (US)
```
