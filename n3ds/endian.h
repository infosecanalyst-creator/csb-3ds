/* csb-3ds: newlib on devkitARM has no <endian.h>. CSBTypes.h only needs the
 * BYTE_ORDER / LITTLE_ENDIAN / BIG_ENDIAN macros; the 3DS ARM11 is little-endian. */
#ifndef CSB3DS_ENDIAN_H
#define CSB3DS_ENDIAN_H
#include <machine/endian.h>
#ifndef LITTLE_ENDIAN
#define LITTLE_ENDIAN 1234
#endif
#ifndef BIG_ENDIAN
#define BIG_ENDIAN 4321
#endif
#ifndef BYTE_ORDER
#define BYTE_ORDER LITTLE_ENDIAN
#endif
#endif
