#include "libc_compat.h"

#include <cstdint>
#include <cstring>

namespace {

// FreeBSD 9 amd64 struct stat as the kernel writes it.
struct KernelStat {
  uint32_t st_dev;
  uint32_t st_ino;
  uint16_t st_mode;
  uint16_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint32_t st_rdev;
  struct timespec st_atim;
  struct timespec st_mtim;
  struct timespec st_ctim;
  int64_t st_size;
  int64_t st_blocks;
  uint32_t st_blksize;
  uint32_t st_flags;
  uint32_t st_gen;
  int32_t st_lspare;
  struct timespec st_birthtim;
};
static_assert(sizeof(KernelStat) == 120, "FreeBSD 9 struct stat is 120 bytes");

// The kernel writes 120 bytes; hand it a buffer at least as large as the
// header's struct so neither side can overrun.
union Buffer {
  KernelStat k;
  struct stat h;
  unsigned char raw[256];
};

void convert(const KernelStat &k, struct stat *out) {
  std::memset(out, 0, sizeof(*out));
  out->st_dev = k.st_dev;
  out->st_ino = k.st_ino;
  out->st_mode = k.st_mode;
  out->st_nlink = k.st_nlink;
  out->st_uid = k.st_uid;
  out->st_gid = k.st_gid;
  out->st_rdev = k.st_rdev;
  out->st_atim = k.st_atim;
  out->st_mtim = k.st_mtim;
  out->st_ctim = k.st_ctim;
  out->st_size = k.st_size;
  out->st_blocks = k.st_blocks;
  out->st_blksize = k.st_blksize;
}

} // namespace

namespace Compat {

int stat(const char *path, struct stat *out) {
  Buffer b;
  std::memset(&b, 0, sizeof(b));
  int rc = ::stat(path, &b.h); // kernel fills the FreeBSD layout
  if (rc == 0)
    convert(b.k, out);
  return rc;
}

int fstat(int fd, struct stat *out) {
  Buffer b;
  std::memset(&b, 0, sizeof(b));
  int rc = ::fstat(fd, &b.h);
  if (rc == 0)
    convert(b.k, out);
  return rc;
}

} // namespace Compat
