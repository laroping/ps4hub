#pragma once
// Workarounds for ABI mismatches between the OpenOrbis musl headers and the
// PS4 (FreeBSD 9) kernel.
//
// struct stat: the headers declare mode_t as 32-bit (128-byte struct), the
// kernel fills the FreeBSD 9 layout with a 16-bit mode_t (120 bytes). Every
// field after st_mode is read from the wrong offset; st_size actually picks
// up st_blocks. These wrappers call the kernel with a raw buffer and convert.
#include <sys/stat.h>

namespace Compat {

int stat(const char *path,
         struct stat *out); // 0 / -1 (errno set by the kernel)
int fstat(int fd, struct stat *out);

} // namespace Compat
