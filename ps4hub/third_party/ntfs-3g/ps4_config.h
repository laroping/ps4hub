/* Minimal config.h for ntfs-3g on PS4 (FreeBSD x86_64, OpenOrbis toolchain) */
#ifndef NTFS3G_PS4_CONFIG_H
#define NTFS3G_PS4_CONFIG_H

/* Pull in ptrdiff_t, size_t etc. so headers that don't include <stddef.h>
   themselves (support.h uses ptrdiff_t) still compile. */
#include <stddef.h>
#include <sys/types.h>

#define HAVE_STDLIB_H 1
#define HAVE_STDIO_H 1
#define HAVE_STRING_H 1
#define HAVE_STRINGS_H 1
#define HAVE_FCNTL_H 1
#define HAVE_UNISTD_H 1
#define HAVE_ERRNO_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_IOCTL_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDINT_H 1
#define HAVE_CTYPE_H 1
#define HAVE_SYSLOG_H 1
#define HAVE_INTTYPES_H 1
#define HAVE_TIME_H 1
#define HAVE_SYS_PARAM_H 1
#define HAVE_SYS_MOUNT_H 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_REALPATH 1
#define HAVE_STRSEP 1
#define HAVE_STRLCPY 1
#define HAVE_STRLCAT 1
/* #undef HAVE_MBSINIT - PS4 toolchain doesn't expose mbstate_t */

/* PS4 is 64-bit little-endian x86_64 */
#define WORDS_LITTLEENDIAN 1

/* stat has st_blocks and st_rdev on FreeBSD */
#define HAVE_STRUCT_STAT_ST_BLOCKS 1
#define HAVE_STRUCT_STAT_ST_RDEV 1

#define HAVE_SYS_SYSMACROS_H 1
#define MAJOR_IN_SYSMACROS 1

/* Not available on PS4 / FreeBSD */
/* #undef HAVE_LINUX_FD_H */
/* #undef HAVE_LINUX_FS_H */
/* #undef HAVE_LINUX_HDREG_H */
/* #undef HAVE_LINUX_MAJOR_H */
/* #undef HAVE_MNTENT_H */
/* #undef HAVE_ENDIAN_H */
/* #undef HAVE_GETMNTENT */
/* #undef HAVE_HASMNTOPT */
/* #undef HAVE_SETXATTR */
/* #undef HAVE_SYS_ENDIAN_H */

/* We only need read-only operations, no FUSE */
/* #undef ENABLE_EXTRAS */

#endif
