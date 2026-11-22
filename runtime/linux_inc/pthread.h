#pragma once
#include <sys/types.h>
typedef unsigned long pthread_t;
#ifdef __cplusplus
extern "C" {
#endif
int pthread_create(pthread_t *thread, const void *attr, void *(*start_routine)(void *), void *arg);
int pthread_detach(pthread_t thread);
void pthread_exit(void *retval) __attribute__((noreturn));
#ifdef __cplusplus
}
#endif
