#pragma once

/*
    Debug logging for every game.

    Why this does not use the game's printf
    ---------------------------------------
    The obvious thing is to call the game's own printf (core/game/IO.h _print).
    That works from the plugin's init function - the startup banner comes out -
    but the same call from inside a file device hook produces nothing, even
    though the hooks demonstrably run and the compiled call sequence is
    identical: both load the pointer from the same global and jalr through it.
    Something about the game's stdio state after startup swallows it.

    This was diagnosed on Tourist Trophy, but the Gran Turismo 4 game calls the
    same function through the same mechanism, so it has the same latent problem.

    So logging goes out the EE SIO port instead (core/ps2/Sio.h), which depends
    on nothing the game initialises and therefore works at any point in
    execution. Formatting still uses the game's sprintf, which is known good -
    it is what builds the "host:" paths the HostFS hook opens successfully.

    LOG is a macro so the varargs pass straight through at the call site, which
    avoids needing a va_list and a vsprintf address.
*/

#include "core/ps2/Sio.h"

/*
    A note on colour, since it is the obvious thing to reach for: there is none
    to be had. PCSX2 funnels every SIO byte into one fixed console channel -
    ConsoleLogFromVM<Color_Cyan> eeConsole (DebugTools/Debug.h), written from
    HwWrite.cpp - so everything the game prints is cyan. The colour is a
    compile-time template parameter, and the bytes pass through
    ShiftJIS_ConvertString on the way, which would mangle ANSI escapes into
    literal junk rather than colouring anything. Loud has to mean shape.
*/

#if HOSTFS_PRINT

#include "core/game/IO.h"

/*
    The buffer is a local, so keep this off deep or recursive paths. 256 bytes
    is comfortably more than any line here - the longest is a host path, which
    its callers cap at 128.
*/
#define LOG(...)                       \
    do {                               \
        char _log_buf[256];            \
        _sprintf(_log_buf, __VA_ARGS__); \
        Sio_Puts(_log_buf);            \
    } while (0)

#else
#define LOG(...) do { } while (0)
#endif

/*
    WARN is for the things you want to see even in a quiet build: right now,
    a streamed file that could not be served from the host and is therefore
    being read from the disc image instead. That is not an error - the game
    plays fine - but it silently makes an edited file do nothing, which cost
    a long debugging session once already. So it is deliberately separate from
    HOSTFS_PRINT and on by default.
*/
#ifndef HOSTFS_WARN
#define HOSTFS_WARN 1
#endif

#if HOSTFS_WARN

#include "core/game/IO.h"

#define WARN(...)                        \
    do {                                 \
        char _warn_buf[256];             \
        _sprintf(_warn_buf, __VA_ARGS__); \
        Sio_Puts(_warn_buf);             \
    } while (0)

#else
#define WARN(...) do { } while (0)
#endif
