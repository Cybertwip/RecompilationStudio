#pragma once

#include <errno.h>

typedef unsigned char cc_t;
typedef unsigned int speed_t;
typedef unsigned int tcflag_t;

#define NCCS 32

#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSTART 8
#define VSTOP 9
#define VSUSP 10

#define ECHO 0x00000008
#define ICANON 0x00000100

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_line;
    cc_t c_cc[NCCS];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

static inline int tcgetattr(int fd, struct termios* termios_p)
{
    (void)fd;
    if (!termios_p) {
        errno = EINVAL;
        return -1;
    }
    *termios_p = (struct termios){0};
    return 0;
}

static inline int tcsetattr(int fd, int optional_actions, const struct termios* termios_p)
{
    (void)fd;
    (void)optional_actions;
    (void)termios_p;
    return 0;
}

static inline void cfmakeraw(struct termios* termios_p)
{
    if (!termios_p) {
        return;
    }
    termios_p->c_iflag = 0;
    termios_p->c_oflag = 0;
    termios_p->c_lflag = 0;
    termios_p->c_cflag = 0;
}
