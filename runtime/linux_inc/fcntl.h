#pragma once
#include <sys/types.h>
#define O_RDONLY 00
#define O_WRONLY 01
#define O_RDWR   02
#define O_CREAT  0100
#define O_EXCL   0200
#define O_TRUNC  01000
#define O_APPEND 02000
#define O_NONBLOCK 04000
#ifdef __cplusplus
extern "C" {
#endif
int open(const char *pathname, int flags, ...);
#ifdef __cplusplus
}
#endif
