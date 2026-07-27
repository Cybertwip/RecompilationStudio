#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "media.h"
#include <sys/types.h>  // For int, unsigned long, void*
#include <sysnum.h>     // For SYS_ioctl

int ioctl(int fd, unsigned long request, ...);

#ifdef __cplusplus
}
#endif