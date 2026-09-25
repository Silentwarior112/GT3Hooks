#include "Sio.h"

/*
    EE SIO transmit FIFO.

    PCSX2 buffers these writes and prints a line to its console on newline.
*/
#define EE_SIO_TX (*(volatile unsigned char*)0x1000F180)

void Sio_Puts(const char* s)
{
    if (!s)
        return;

    while (*s)
        EE_SIO_TX = (unsigned char)*s++;
}
