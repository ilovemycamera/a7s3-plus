#pragma once
#include <sys/types.h>
struct stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned int st_mode;
    unsigned int st_nlink;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned long st_rdev;
    unsigned long __pad1;
    long st_size;
    int st_blksize;
    int __pad2;
    long st_blocks;
    long st_atime;
    unsigned long st_atime_nsec;
    long st_mtime;
    unsigned long st_mtime_nsec;
    long st_ctime;
    unsigned long st_ctime_nsec;
    unsigned int __unused4;
    unsigned int __unused5;
};
#ifdef __cplusplus
extern "C" {
#endif
int mkdir(const char *pathname, mode_t mode);
int stat(const char *pathname, struct stat *statbuf);
#ifdef __cplusplus
}
#endif
