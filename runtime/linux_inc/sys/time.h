#pragma once
#include <sys/types.h>
struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};
struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};
#ifdef __cplusplus
extern "C" {
#endif
int gettimeofday(struct timeval *tv, void *tz);
#ifdef __cplusplus
}
#endif
