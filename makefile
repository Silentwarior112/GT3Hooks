#####################################################
# GT3Hooks - shared build driver
#
# This file knows nothing about any particular game. It discovers games by
# globbing source/games/*/game.mk, and everything game-specific lives in that
# file and in the game's Targets/ headers.
#
#   make GAME=gt4o PLATFORM=pcsx2
#   make GAME=tt REGION=EU PLATFORM=ps2
#   make GAME=tt PLATFORM=ps2 print-config
#   make GAME=tt PLATFORM=ps2 clean
#
# Normally you would use build.bat, which drives this and then runs the plugin
# injector:
#
#   build gt4o US for:pcsx2
#   build tt EU for:ps2
#####################################################

# ---- game discovery -------------------------------------------------------
GAMES := $(sort $(patsubst source/games/%/game.mk,%,$(wildcard source/games/*/game.mk)))

ifeq ($(GAME),)
$(error No GAME set. Available games: $(GAMES).  Try: make GAME=<name>, or use build.bat)
endif

GAME_DIR := source/games/$(GAME)
ifeq ($(wildcard $(GAME_DIR)/game.mk),)
$(error Unknown game '$(GAME)'. Available games: $(GAMES))
endif

# ---- the game supplies its regions, sources and knobs ---------------------
include $(GAME_DIR)/game.mk

# A game may name a default region; GT3 does not, so every build names one.
REGION ?= $(DEFAULT_REGION)
ifeq ($(REGION),)
$(error Name a region for '$(GAME)': one of $(REGIONS) - e.g. make GAME=$(GAME) REGION=$(firstword $(REGIONS)))
endif

BUILD        := $(BUILD_$(REGION))
GAME_ELF     := $(ELF_$(REGION))
BASE_ADDRESS := $(BASE_$(REGION))

# A game may also link parts of its plugin elsewhere, in up to three more caves:
# CAVE2_<region>, CAVE2B_<region> and CAVE2C_<region>, each with a matching
# *_RESERVE, plus a GAME_LINKFILE that places them. The injector only knows
# about one address, so build.bat adds the rest with tools/inject_cave2.py.
# Empty for a game that has none.
CAVE2_ADDRESS  := $(CAVE2_$(REGION))
CAVE2B_ADDRESS := $(CAVE2B_$(REGION))
CAVE2C_ADDRESS := $(CAVE2C_$(REGION))

# And a game with a dev RAM reserve (DEVRAM_<region> and DEVRAM_RESERVE) links
# the code that lives in it there; see source/games/gt4o/DevRam.c.
DEVRAM_ADDRESS := $(DEVRAM_$(REGION))

# And a game whose crt0 calls into the game from the word after its `ei` names
# that word and the call (STARTUP_SLOT_<region>, STARTUP_CALL_<region>): the
# injector's hook would leave the call in its delay slot, so build.bat makes the
# slot a nop with tools/startup_slot.py and the game's INVOKER makes the call.
STARTUP_SLOT := $(STARTUP_SLOT_$(REGION))
STARTUP_CALL := $(STARTUP_CALL_$(REGION))

ifeq ($(BUILD),)
$(error Unknown region '$(REGION)' for game '$(GAME)'. Available regions: $(REGIONS))
endif

# ---- platform --------------------------------------------------------------
# Every build is for one platform: ps2 (a real console) or pcsx2. A game lists
# the sources every build gets in GAME_SRCS, and those only one platform gets in
# GAME_SRCS_pcsx2 / GAME_SRCS_ps2, which are linked after them. The code sees
# PLATFORM_PS2 and PLATFORM_PCSX2, one of them 1 and the other 0. build.bat
# takes it as for:ps2 or for:pcsx2.
PLATFORMS := ps2 pcsx2
ifeq ($(filter $(PLATFORMS),$(PLATFORM)),)
$(error Set PLATFORM to one of: $(PLATFORMS). build.bat takes it as for:ps2 or for:pcsx2)
endif
ifneq ($(words $(PLATFORM)),1)
$(error PLATFORM must be exactly one of: $(PLATFORMS))
endif

EE_SRCS := $(GAME_SRCS) $(GAME_SRCS_$(PLATFORM))

# ---- things that live OUTSIDE this repository -------------------------------
# Nothing binary is kept in the repo: not the SDK, not the base images, not the
# build output. Both of these can be overridden from the environment or on the
# make command line if your layout differs. See the README.
PS2SDK_HOME ?= ../ps2sdk-main
WORK        ?= ../GT3Hooks-work

# ---- output layout ---------------------------------------------------------
# Per game, per region AND per platform, so building one never clobbers
# another's objects.
OUTDIR := $(WORK)/out/$(GAME)/$(BUILD)/$(PLATFORM)
OBJDIR := $(OUTDIR)/obj
EE_BIN := $(OUTDIR)/plugin.elf
EE_MAP := $(OUTDIR)/plugin.map

# Object names are the source path flattened, so sources from different
# directories cannot collide in one flat obj dir.
OBJ_OF = $(OBJDIR)/$(subst /,_,$(basename $(1))).o
EE_OBJS := $(foreach s,$(EE_SRCS),$(call OBJ_OF,$(s)))

# ---- flags -----------------------------------------------------------------
NEWLIB_NANO = 0
KERNEL_NOPATCH = 0
# The plugin links against nothing from the SDK: every libc and kernel call it
# makes goes to the GAME's own copy through a function pointer. Current ps2sdk
# no longer ships libps2sdkc / libkernel-nopatch, which is what
# eemakefile.eeglobal's NODEFAULTLIBS path asks for, so both knobs stay off.

# A game may bring its own linkfile (GAME_LINKFILE in its game.mk).
EE_LINKFILE = $(if $(GAME_LINKFILE),$(GAME_LINKFILE),mk/linkfile)

# -Isource lets shared headers be reached the same way from any depth, e.g.
# "core/ps2/Memory.h", regardless of how deeply a game nests its sources.
# The Targets include path plus TARGET_HEADER below form the seam described in
# source/core/Target.h: shared code includes "core/Target.h" and lands on the
# selected build's address header without naming a game or region.
# eemakefile.eeglobal appends to EE_INCS, so this must be set before it runs.
EE_INCS := -Isource -I$(GAME_DIR) -I$(GAME_DIR)/Targets

EE_CFLAGS = -fno-zero-initialized-in-bss -O0 \
	-DTARGET_HEADER='"$(BUILD).h"' \
	-DPLATFORM_PS2=$(if $(filter ps2,$(PLATFORM)),1,0) \
	-DPLATFORM_PCSX2=$(if $(filter pcsx2,$(PLATFORM)),1,0) \
	-DHOSTFS_PRINT=$(HOSTFS_PRINT) \
	-DPRINT_HOSTFS_READS=$(PRINT_HOSTFS_READS) \
	$(GAME_DEFS)

ifeq ($(USE_DEV_RAM),1)
  EE_CFLAGS += -DUSE_DEV_RAM
endif

EE_LDFLAGS = -Wl,--entry=INVOKER -Wl,-Map,$(EE_MAP) \
	-Wl,'--defsym=BASE_ADDRESS=$(BASE_ADDRESS)' \
	-ffunction-sections -Wl,--gc-sections -fdata-sections

# The second cave's address and size go to both the linkfile (--defsym) and the
# C code (-D), so the linker and init() agree on where the cave is and where the
# pool must start. PLUGIN_RESERVE goes along so that linkfile can check the
# first cave's size too.
ifneq ($(CAVE2_ADDRESS),)
  EE_CFLAGS  += -DCAVE2_ADDRESS=$(CAVE2_ADDRESS) -DCAVE2_RESERVE=$(CAVE2_RESERVE)
  EE_LDFLAGS += -Wl,'--defsym=CAVE2_ADDRESS=$(CAVE2_ADDRESS)' \
	-Wl,'--defsym=CAVE2_RESERVE=$(CAVE2_RESERVE)' \
	-Wl,'--defsym=PLUGIN_RESERVE=$(PLUGIN_RESERVE)'
endif
ifneq ($(CAVE2B_ADDRESS),)
  EE_CFLAGS  += -DCAVE2B_ADDRESS=$(CAVE2B_ADDRESS) -DCAVE2B_RESERVE=$(CAVE2B_RESERVE)
  EE_LDFLAGS += -Wl,'--defsym=CAVE2B_ADDRESS=$(CAVE2B_ADDRESS)' \
	-Wl,'--defsym=CAVE2B_RESERVE=$(CAVE2B_RESERVE)'
endif
ifneq ($(CAVE2C_ADDRESS),)
  EE_LDFLAGS += -Wl,'--defsym=CAVE2C_ADDRESS=$(CAVE2C_ADDRESS)' \
	-Wl,'--defsym=CAVE2C_RESERVE=$(CAVE2C_RESERVE)'
endif
# Passed on every platform: the linkfile places the section either way, and it
# is simply empty in a build that has no reserve sources.
ifneq ($(DEVRAM_ADDRESS),)
  EE_CFLAGS  += -DDEVRAM_ADDRESS=$(DEVRAM_ADDRESS) -DDEVRAM_RESERVE=$(DEVRAM_RESERVE)
  EE_LDFLAGS += -Wl,'--defsym=DEVRAM_ADDRESS=$(DEVRAM_ADDRESS)' \
	-Wl,'--defsym=DEVRAM_RESERVE=$(DEVRAM_RESERVE)'
endif
ifneq ($(STARTUP_CALL),)
  EE_CFLAGS  += -DSTARTUP_CALL=$(STARTUP_CALL)
endif

all: $(EE_BIN)

# ---- build.bat reads these; keep the names stable --------------------------
.PHONY: print-config games regions clean
print-config:
	@echo GAME=$(GAME)
	@echo REGION=$(REGION)
	@echo PLATFORM=$(PLATFORM)
	@echo BUILD=$(BUILD)
	@echo GAME_ELF=$(GAME_ELF)
	@echo BASE_ADDRESS=$(BASE_ADDRESS)
	@echo OUTDIR=$(OUTDIR)
	@echo OBJDIR=$(OBJDIR)
	@echo EE_BIN=$(EE_BIN)
	@echo CLAMP_BSS=$(CLAMP_BSS)
	@echo PLUGIN_RESERVE=$(PLUGIN_RESERVE)
	@echo CAVE2_ADDRESS=$(CAVE2_ADDRESS)
	@echo CAVE2_RESERVE=$(CAVE2_RESERVE)
	@echo CAVE2B_ADDRESS=$(CAVE2B_ADDRESS)
	@echo CAVE2B_RESERVE=$(CAVE2B_RESERVE)
	@echo CAVE2C_ADDRESS=$(CAVE2C_ADDRESS)
	@echo CAVE2C_RESERVE=$(CAVE2C_RESERVE)
	@echo DEVRAM_ADDRESS=$(DEVRAM_ADDRESS)
	@echo DEVRAM_RESERVE=$(DEVRAM_RESERVE)
	@echo STARTUP_SLOT=$(STARTUP_SLOT)
	@echo STARTUP_CALL=$(STARTUP_CALL)
	@echo REGIONS=$(REGIONS)
	@echo WORK=$(WORK)
	@echo PS2SDK_HOME=$(PS2SDK_HOME)

games:
	@echo $(GAMES)

regions:
	@echo $(REGIONS)

clean:
	-rm -rf $(OUTDIR)

PS2SDK = $(PS2SDK_HOME)/ps2sdk
include $(PS2SDK)/samples/Makefile.pref
include mk/eemakefile.eeglobal

# ---- per-source compile rules ---------------------------------------------
# Generated so each object lands in OBJDIR under its flattened name. These are
# a different target pattern from eemakefile.eeglobal's `%.o: %.c`, so that
# generic rule simply never fires.
#
# The object directory is created once at parse time rather than per recipe, so
# the rules stay quiet whichever shell make picks. build.bat also creates it, so
# this only matters when make is invoked directly.
$(shell mkdir -p "$(OBJDIR)" 2>/dev/null)

define GAME_OBJ_RULE
$(call OBJ_OF,$(1)): $(1)
	$$(EE_CC) $$(EE_CFLAGS) $$(EE_INCS) -c $$< -o $$@
endef
$(foreach s,$(EE_SRCS),$(eval $(call GAME_OBJ_RULE,$(s))))

# Relink when the linkfile changes: it decides what lands where.
$(EE_BIN): $(EE_LINKFILE)
