#pragma once
typedef void (*sighandler_t)(int);
#define SIGINT 2
#define SIGTERM 15
#ifdef __cplusplus
extern "C" {
#endif
sighandler_t signal(int signum, sighandler_t handler);
#ifdef __cplusplus
}
#endif
