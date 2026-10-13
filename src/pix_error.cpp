#include <stdio.h>
#include <stdarg.h>
#include "pix_error.h"

using namespace std;

static char lastError[512] = "";

//----------------------------------------------------------------------------------------------------------------------
void setPixError(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(lastError, sizeof(lastError), format, args);
    va_end(args);
}

//----------------------------------------------------------------------------------------------------------------------
const char *getPixError()
{
    return lastError;
}
