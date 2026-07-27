#ifndef _POSIX_SHIM_SYS_PARAM_H
#define _POSIX_SHIM_SYS_PARAM_H

#ifndef MAXPATHLEN
#define MAXPATHLEN 4096
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif

#ifndef roundup
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#endif

#endif
