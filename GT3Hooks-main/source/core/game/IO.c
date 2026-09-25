#include "IO.h"

void (*_sprintf)(char* dest, const char* format, ...) = (void*)ADDR_sprintf;
