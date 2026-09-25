#include "core/ps2/Memory.h"
#include "core/Target.h"
#include "core/Log.h"

#include "GameConfig.h"
#include "Pod.h"
#include "Drive.h"

/*
    Gran Turismo 3 plugin.

    The entry point is INVOKER, in Invoker.S - see that file for why GT3 needs
    assembly there and why its requirements are the inverse of the GT4 family's.
    This file is only what runs once INVOKER has reissued the `ei`.

    What has already happened by the time init() runs (FINDINGS.md section 1):
    .bss is cleared, $gp and $sp are set, the syscall table and kernel patch are
    in place and the SIF RPC thread exists - but NO C++ constructor has run and
    NO byte has been allocated. The static-constructor runner __main is called
    from the first instruction of main(), which is after this. So init() can
    patch any engine global before anything reads it.
*/

/* The kernel's FlushCache(int op) - integers only. */
static void (*FlushCache)(int op) = (void*)ADDR_FlushCache;

/* A word of the game's image, for the in-place edits below. */
#define W(a)                (*(volatile unsigned int*)(a))

#if MOVIE_FRAME_MODE
/*
    Every movie in frame mode (GameConfig.h). The MPEG object's mode word is
    read by exactly two functions, each deciding with one `bne` to its
    frame-mode case (Targets/SCUS-97102.h, "movies"); both become an
    unconditional branch there. Nothing of the plugin runs afterwards.
*/
#define B_TO(site, target)  (0x10000000u | ((((target) - (site) - 4) >> 2) & 0xFFFFu))

static void Movie_FrameMode(void)
{
    if (W(MPEG_MODE_SIZE_BNE) != MPEG_MODE_SIZE_BNE_WORD ||
        W(MPEG_MODE_DRAW_BNE) != MPEG_MODE_DRAW_BNE_WORD) {
        LOG("[gt3hooks] movie: mode branches hold %08X %08X, expected %08X %08X - "
            "frame mode NOT installed\n",
            W(MPEG_MODE_SIZE_BNE), W(MPEG_MODE_DRAW_BNE),
            MPEG_MODE_SIZE_BNE_WORD, MPEG_MODE_DRAW_BNE_WORD);
        return;
    }
    PATCH_INT(MPEG_MODE_SIZE_BNE, B_TO(MPEG_MODE_SIZE_BNE, MPEG_MODE_SIZE_FRAME));
    PATCH_INT(MPEG_MODE_DRAW_BNE, B_TO(MPEG_MODE_DRAW_BNE, MPEG_MODE_DRAW_FRAME));
    LOG("[gt3hooks] movie: every movie plays in frame mode\n");
}
#endif

#if QUIET_TIME_LOG
/*
    The per-frame "Timezone=" and "SummerTime=" console lines (GameConfig.h):
    the jal to printf in each of the two getters becomes a nop
    (Targets/SCUS-97102.h, "console").
*/
static void Quiet_TimeLog(void)
{
    if (W(SCF_TIMEZONE_PRINTF) != SCF_TIMEZONE_PRINTF_WORD ||
        W(SCF_SUMMERTIME_PRINTF) != SCF_SUMMERTIME_PRINTF_WORD) {
        LOG("[gt3hooks] console: printf calls hold %08X %08X, expected %08X %08X - "
            "Timezone/SummerTime lines NOT silenced\n",
            W(SCF_TIMEZONE_PRINTF), W(SCF_SUMMERTIME_PRINTF),
            SCF_TIMEZONE_PRINTF_WORD, SCF_SUMMERTIME_PRINTF_WORD);
        return;
    }
    NOP(SCF_TIMEZONE_PRINTF);
    NOP(SCF_SUMMERTIME_PRINTF);
    LOG("[gt3hooks] console: Timezone/SummerTime lines silenced\n");
}
#endif

void init(void)
{
    /*
        Reserve the plugin's memory from the engine's pool.

        GT3 has one heap, built lazily on the first malloc/free rather than at
        startup, whose base and limit are two constant words in .sdata. The
        plugin is linked at 128-aligned _end, i.e. at the pool's base, so the
        base is raised past it here. The pool then begins above the plugin and
        the allocator can never hand the region out.

        This is Tourist Trophy's trick, and it is easier here: TT's base is an
        immediate in code, GT3's is a writable data word.

        There is deliberately NO nop of the pool's memset. Raising the base
        already moves the clear above the plugin; suppressing it would only rob
        the game of its zeroed pool. crt0 clears .bss up to _end, but the pool
        runs from _end to the top of RAM and nothing else clears that.

        POOL_BASE_GLOBAL only exists in RAM at all if the base image was built
        with tools/gt3/gt3pack.py - see the note in Targets/SCUS-97102.h.
    */
    PATCH_INT(POOL_BASE_GLOBAL, POOL_BASE_NEW);

    /*
        newlib's sbrk break pointer ships holding _end too. sbrk has one caller
        and is the only user of syscall 62, so malloc almost certainly cannot
        reach the plugin either way - but this costs one store and leaves
        nothing pointing into the reserved block.
    */
    PATCH_INT(SBRK_BREAK_GLOBAL, POOL_BASE_NEW);

    LOG("[gt3hooks] " GAME_LONGNAME " (" BUILD_NAME ", " REGION_NAME ")\n");
    LOG("[gt3hooks] plugin %08X..%08X, pool base %08X -> %08X\n",
        PLUGIN_BASE_ADDRESS,
        PLUGIN_BASE_ADDRESS + PLUGIN_RESERVE_BYTES,
        POOL_BASE_VALUE,
        POOL_BASE_NEW);

    Pod_InstallHooks();

#if GBX_ENABLE || HYB_ENABLE
    Drive_InstallHooks();           /* gearbox types, Honda Dualnote */
#endif

#if MOVIE_FRAME_MODE
    Movie_FrameMode();              /* every movie in frame mode */
#endif

#if QUIET_TIME_LOG
    Quiet_TimeLog();                /* no per-frame Timezone/SummerTime lines */
#endif

    /*
        Every hook above was written into the game's code through the data
        cache, and the EE fetches instructions from memory, not from the data
        cache - so on a console a patched word can still execute in its old
        form until its cache line happens to be written back. Write the data
        cache back, then drop anything the instruction cache holds. PCSX2 does
        not model this; hardware does.
    */
    FlushCache(0);
    FlushCache(2);
}

int main(void)
{
    return 0;
}
