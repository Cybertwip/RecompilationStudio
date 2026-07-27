/*
 * MVII baremetal stdio shim — declarations missing from llvm-libc baremetal.
 *
 * llvm-libc baremetal ships fread/fwrite/feof/ferror/fgetc/fputc/printf etc.
 * but does NOT ship fopen/fclose/fseek/ftell/fflush/clearerr/ungetc/perror or
 * stdin/stdout/stderr globals. We declare them here and supply trivial
 * implementations in posix_fileio.c (failure-returning stubs unless wired
 * to a backend such as FATFs).
 *
 * This file is force-included by the kernel CMake AFTER <stdio.h>.
 */
#ifndef _POSIX_SHIM_STDIO_EXT_H
#define _POSIX_SHIM_STDIO_EXT_H

#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>
#include <errno.h>

/* llvm-libc's bare-metal <errno.h> is minimal on some targets (e.g. armv7 has
 * no ENOSYS). Provide the POSIX values the shim/kernel reference as guarded
 * fallbacks (no-ops where the sysroot already defines them). */
#ifndef ENOSYS
#define ENOSYS 38
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#ifndef EOF
#define EOF (-1)
#endif

#ifndef BUFSIZ
#define BUFSIZ 1024
#endif

#ifndef FILENAME_MAX
#define FILENAME_MAX 4096
#endif

#ifndef FOPEN_MAX
#define FOPEN_MAX 20
#endif

#ifndef TMP_MAX
#define TMP_MAX 10000
#endif

#ifndef L_tmpnam
#define L_tmpnam 4096
#endif

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
FILE *fdopen(int fd, const char *mode);
FILE *popen(const char *command, const char *mode);
int   fclose(FILE *stream);
int   pclose(FILE *stream);
int   fflush(FILE *stream);
int   fseek(FILE *stream, long offset, int whence);
long  ftell(FILE *stream);
int   fseeko(FILE *stream, off_t offset, int whence);
off_t ftello(FILE *stream);
void  rewind(FILE *stream);
int   getc(FILE *stream);
int   fgetc(FILE *stream);
int   getchar(void);
char *fgets(char *s, int size, FILE *stream);
int   fputc(int c, FILE *stream);
int   ungetc(int c, FILE *stream);
void  clearerr(FILE *stream);
int   fileno(FILE *stream);
void  perror(const char *s);
void  setbuf(FILE *stream, char *buf);
int   setvbuf(FILE *stream, char *buf, int mode, size_t size);
int   sprintf(char *s, const char *format, ...);

#ifdef __cplusplus
}
#endif

#endif /* _POSIX_SHIM_STDIO_EXT_H */
