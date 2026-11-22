#pragma once
#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>
typedef struct _IO_FILE FILE;
#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define FILENAME_MAX 4096
#ifdef __cplusplus
extern "C" {
#endif
#define stdin  ((FILE *)0)
#define stdout ((FILE *)1)
#define stderr ((FILE *)2)
FILE *fopen(const char *path, const char *mode);
int fclose(FILE *fp);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fflush(FILE *stream);
int fprintf(FILE *stream, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list ap);
int printf(const char *format, ...);
int vprintf(const char *format, va_list ap);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int sscanf(const char *str, const char *format, ...);
int fscanf(FILE *stream, const char *format, ...);
int fgetc(FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int fputc(int c, FILE *stream);
int fputs(const char *s, FILE *stream);
int putchar(int c);
int puts(const char *s);
int remove(const char *pathname);
int rename(const char *oldpath, const char *newpath);
int feof(FILE *stream);
int ferror(FILE *stream);
void perror(const char *s);
int fileno(FILE *stream);
#ifdef __cplusplus
}
#endif
