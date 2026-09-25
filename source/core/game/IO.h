#pragma once

#include "core/Target.h"

/*
    Calls into the game's own libc.

    The plugin links against nothing from the SDK: every libc call it makes goes
    to the game's copy through a function pointer, so the only thing the linker
    needs is the linkfile. See the GT4Hooks README.

    GT3 binds far less than the GT4 family does, because so far only logging
    needs it. Note two absences that matter if this grows (FINDINGS.md section 6):

      * There is NO standalone strlen in any GT3 build - strcat inlines its own
        NUL scan - so there is nothing to bind a __strlen to.
      * There is no snprintf either, and GT3's sprintf sink is an unbounded
        `*p++ = c`. Every format here has to fit its buffer by construction.
*/

extern void (*_sprintf)(char* dest, const char* format, ...);
