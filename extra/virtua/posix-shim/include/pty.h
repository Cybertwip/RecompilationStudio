#pragma once

#include <sys/ioctl.h>
#include <termios.h>

#ifdef __cplusplus
extern "C" {
#endif

int openpty(int* amaster,
            int* aslave,
            char* name,
            const struct termios* termp,
            const struct winsize* winp);

#ifdef __cplusplus
}
#endif
