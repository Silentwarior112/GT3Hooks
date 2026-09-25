#pragma once

/*
    Raw debug output over the EE's SIO port.

    Writing a byte to the SIO transmit register emits one character. PCSX2
    collects those writes into its console; on real hardware they leave via the
    SIO port. Nothing the game sets up has to be working for this to function,
    which is the whole point - see core/Log.h.
*/

void Sio_Puts(const char* s);
