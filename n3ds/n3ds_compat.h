/* csb-3ds: force-included into every 3DS translation unit (-include n3ds_compat.h).
 * glibc pulls these headers in transitively on Linux; newlib does not. */
#ifndef CSB3DS_COMPAT_H
#define CSB3DS_COMPAT_H
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#endif
