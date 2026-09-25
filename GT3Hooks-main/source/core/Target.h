#pragma once

/*
    The seam between shared code and a particular game build.

    Every address literal in this project lives in exactly one place: a
    per-build header under source/games/<game>/Targets/. This file is how
    everything else reaches those literals without naming a game.

    The makefile supplies both halves:

        -DTARGET_HEADER='"SCUS-97436.h"'
        -Isource/games/gt4o/Targets

    so the include below resolves to the selected build's header and nothing
    else needs to know which game or region is being built. Adding a region is
    a new header plus a line in that game's game.mk - no C changes anywhere.
*/

#ifndef TARGET_HEADER
#error "TARGET_HEADER is not defined. Build through build.bat or the root makefile, which set it from the selected game and region."
#endif

#include TARGET_HEADER
