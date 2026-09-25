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
