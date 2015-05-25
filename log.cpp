


#if defined(_WIN32) || defined(_WIN64) || defined(WIN32) || defined(WIN64)
#include "windows.h"
#endif

#include "log.h"

#include <stdio.h>
#include <stdarg.h>


extern "C" void DoLog( const char * fmt, ... )
{
    va_list args;
    va_start(args, fmt);
    char buf[64*1024];
    vsprintf_s(buf, sizeof(buf), fmt, args);
#if defined(_WIN32) || defined(_WIN64) || defined(WIN32) || defined(WIN64)
    //WINDOWS
    OutputDebugStringA( buf );
#else
    //NOT WINDOWS
    printf("%s",buf);
#endif //defined(_WIN32) || defined(_WIN64)
}
