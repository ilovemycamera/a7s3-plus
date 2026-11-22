#pragma once
#include <stddef.h>
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 2147483647
#ifdef __cplusplus
extern "C" {
#endif
void *malloc(size_t size);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void abort(void);
void exit(int status);
int atexit(void (*function)(void));
int atoi(const char *nptr);
long atol(const char *nptr);
double atof(const char *nptr);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
int abs(int j);
long labs(long j);
char *getenv(const char *name);
int system(const char *command);
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
int rand(void);
void srand(unsigned int seed);
#ifdef __cplusplus
}
#endif
