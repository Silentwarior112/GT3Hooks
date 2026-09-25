#####################################################
# Game: Gran Turismo 3: A-Spec
#
# The only build file that knows about GT3. The root makefile knows nothing
# about it; it finds this by globbing source/games/*/game.mk.
#####################################################

GAME_LONGNAME := Gran Turismo 3: A-Spec

# ---- regions ---------------------------------------------------------------
# Every build names its region - `build gt3 US` or `build gt3 EU`; there is no
# default. EU (SCES-50294) is the US compile relinked: its target header is
# generated from US's by tools/gt3/port_target.py (see the README), which
# checks every address it carries. EU runs at 50 Hz (GAME_FRAME_HZ).
# Lupo 2 (PAPX-90509) and GT Concept (SCES-50858) are different compiles, the
# features' donor and PD's own 50 Hz reference - not regions of this game.
REGIONS        := US EU

BUILD_US := SCUS-97102
ELF_US   := SCUS_971.02
# Must agree with PLUGIN_BASE_ADDRESS in Targets/SCUS-97102.h.
BASE_US  := 0x4B2C80

BUILD_EU := SCES-50294
ELF_EU   := SCES_502.94
# Must agree with PLUGIN_BASE_ADDRESS in Targets/SCES-50294.h.
BASE_EU  := 0x4B4680

# Bytes reserved for the plugin. GT3 has no dead region big enough to be worth
# using in every build, so like Tourist Trophy this comes out of the engine's
# pool - see source/games/gt3/main.c. Kept at 64 KB rather than TT's 128 KB
# because GT3's pool headroom is unmeasured; see Targets/SCUS-97102.h.
PLUGIN_RESERVE := 0x10000

# ---- startup ---------------------------------------------------------------
# Deliberately NO STARTUP_SLOT_US / STARTUP_CALL_US.
#
# The injector hooks crt0's `ei`, which makes the following word the hook's
# delay slot. In the GT4 family that word is crt0's call into the game's early
# setup, so the build has to nop it and INVOKER makes the call itself. GT3 US's
# word is `lui $v0, 0x35`, which is not a jump and is harmless to run early -
# there is nothing to nop, and build.bat's startup_slot.py step correctly does
# nothing here.
#
# GT3 needs the opposite favour instead: $v0 is live across the hook, so INVOKER
# preserves it. That is source/games/gt3/Invoker.S, not a makefile setting.
#
# Lupo 2 WOULD need STARTUP_SLOT/STARTUP_CALL (its slot is `jal 0x00103458`),
# which is one more reason it is not a region here.

# ---- knobs -----------------------------------------------------------------
# HostFS is not part of this project: GT3 already supports it and it is enabled
# by patching memory. These two exist because the shared makefile passes them
# unconditionally; LOG() is gated on HOSTFS_PRINT.
HOSTFS_PRINT       := 1
PRINT_HOSTFS_READS := 0

# The US base image's single PT_LOAD has p_memsz == p_filesz, so there is no
# declared .bss tail for a plugin to land inside and nothing to clamp.
CLAMP_BSS          := 0

# Extra compiler flags for this game (the root makefile appends GAME_DEFS to
# EE_CFLAGS, so flags go here as well as defines).
#
# THE ODD HIGH FLOAT REGISTERS. The game is GCC 2.9x EABI with single floats,
# where ALL of $f20..$f31 are callee-saved. This compiler is n32, where only the
# even ones are - it uses $f21, $f23, ... $f31 as scratch without saving them.
# Every plugin function the game calls directly or through a thunk (the switch
# query, the rn_01 trampoline, the per-frame hook, the squat and crash handlers)
# would then return with game FPU state destroyed. Reserving those six
# registers makes n32's saving rules match the game's for everything the plugin
# compiles. (Verified with a register-pressure probe: without these flags GCC
# 14 allocates $f21..$f31; with them, never.)
#
# NO FUSED MULTIPLY-ADD. Gearbox.c and Hybrid.c reproduce Lupo 2's float
# arithmetic operation for operation, and a fused multiply-add rounds once
# where the game rounds twice. -ffp-contract=off keeps every product and sum
# a separate instruction, as the game's compiler emitted them.
GAME_DEFS := -ffixed-f21 -ffixed-f23 -ffixed-f25 -ffixed-f27 -ffixed-f29 -ffixed-f31 \
	-ffp-contract=off

# ---- sources ---------------------------------------------------------------
# Invoker.S must be present: -Wl,--entry=INVOKER names the symbol it defines,
# and the injector resolves INVOKER out of the plugin to build its jal.
GAME_SRCS := \
	source/games/gt3/Invoker.S \
	source/games/gt3/PodThunk.S \
	source/games/gt3/DriveThunk.S \
	source/games/gt3/main.c \
	source/games/gt3/Pod.c \
	source/games/gt3/PodSound.c \
	source/games/gt3/Drive.c \
	source/games/gt3/Gearbox.c \
	source/games/gt3/Hybrid.c \
	source/core/ps2/Memory.c \
	source/core/ps2/Sio.c \
	source/core/game/IO.c

GAME_SRCS_pcsx2 :=
GAME_SRCS_ps2   :=
