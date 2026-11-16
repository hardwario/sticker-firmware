#ifndef _KERNEL_VERSION_H_
#define _KERNEL_VERSION_H_

/* The template values come from cmake/modules/version.cmake
 * BUILD_VERSION related template values will be 'git describe',
 * alternatively user defined BUILD_VERSION.
 */

#define ZEPHYR_VERSION_CODE 263424
#define ZEPHYR_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))

#define KERNELVERSION                   0x4050000
#define KERNEL_VERSION_NUMBER           0x40500
#define KERNEL_VERSION_MAJOR            4
#define KERNEL_VERSION_MINOR            5
#define KERNEL_PATCHLEVEL               0
#define KERNEL_TWEAK                    0
#define KERNEL_EXTRAVERSION             
#define KERNEL_VERSION_STRING           "4.5.0"
#define KERNEL_VERSION_EXTENDED_STRING  "4.5.0+0"
#define KERNEL_VERSION_TWEAK_STRING     "4.5.0+0"

#define BUILD_VERSION v4.5.0-13-g4aae9324d33f


#endif /* _KERNEL_VERSION_H_ */
