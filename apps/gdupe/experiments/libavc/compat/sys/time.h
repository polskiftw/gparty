#ifndef GDUPE_LIBAVC_WINDOWS_SYS_TIME_H
#define GDUPE_LIBAVC_WINDOWS_SYS_TIME_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifndef _TIMEVAL_DEFINED
#define _TIMEVAL_DEFINED
struct timeval
{
    long tv_sec;
    long tv_usec;
};
#endif

static __inline int gettimeofday(struct timeval *tv, void *timezone_unused)
{
    FILETIME file_time;
    ULARGE_INTEGER ticks;
    unsigned __int64 microseconds;
    (void)timezone_unused;

    if(tv == 0)
        return -1;

    GetSystemTimeAsFileTime(&file_time);
    ticks.LowPart = file_time.dwLowDateTime;
    ticks.HighPart = file_time.dwHighDateTime;

    /* FILETIME is 100 ns since 1601-01-01; Unix epoch starts 1970-01-01. */
    microseconds = (ticks.QuadPart - 116444736000000000ULL) / 10ULL;
    tv->tv_sec = (long)(microseconds / 1000000ULL);
    tv->tv_usec = (long)(microseconds % 1000000ULL);
    return 0;
}

#endif
