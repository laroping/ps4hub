#include "ntfs_manager.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <orbis/libkernel.h>

#include "network_service.h"

extern "C" {
#include "ntfs-3g/attrib.h"
#include "ntfs-3g/device.h"
#include "ntfs-3g/dir.h"
#include "ntfs-3g/inode.h"
#include "ntfs-3g/logging.h"
#include "ntfs-3g/unistr.h"
#include "ntfs-3g/volume.h"
}

// Diagnostic logging - disabled in release; flip the #define to re-enable
// if NTFS mount / listing ever regresses.
#define NTFS_DIAG 0
namespace {
inline void ntfsLog(const std::string &s) {
#if NTFS_DIAG
  try {
    NetworkService::instance().debugLog("[ntfs] " + s);
  } catch (...) {
  }
#else
  (void)s;
#endif
}
} // namespace

// Custom ntfs_device backed by sceKernelOpen so we can read /dev/da*s* on
// PS4 even when libc open() is rejected with "device file is not supported".
namespace {

struct PS4DevPriv {
  int fd;
  int64_t pos;
  int64_t size; // from OrbisKernelLseek SEEK_END, or 0 if unknown
};

int ps4_dev_open(struct ntfs_device *dev, int flags) {
  PS4DevPriv *p = (PS4DevPriv *)dev->d_private;
  if (!p)
    return -1;
  // Strip any flags that can silently fail on PS4 block devices:
  //   O_EXCL     rejected on shared /dev/da* nodes
  //   O_NONBLOCK FreeBSD returns EINVAL for block char devices
  //   O_DIRECT   not implemented on Orbis, read() returns 0
  int clean = flags & ~(O_EXCL | O_NONBLOCK);
#ifdef O_DIRECT
  clean &= ~O_DIRECT;
#endif
  // Force read-only regardless of what the caller asked for: we never
  // write to the mounted volume and O_RDWR often triggers the shell's
  // "unsupported filesystem" notification on PS4.
  clean = (clean & ~O_ACCMODE) | O_RDONLY;
  int fd = sceKernelOpen(dev->d_name, clean, 0);
  int errSce = fd < 0 ? errno : 0;
  if (fd < 0)
    fd = ::open(dev->d_name, clean);
  int errPosix = fd < 0 ? errno : 0;
  if (fd < 0) {
    char b[160];
    std::snprintf(b, sizeof(b),
                  "open %s failed (sceKernelOpen errno=%d, open errno=%d)",
                  dev->d_name, errSce, errPosix);
    ntfsLog(b);
    return -1;
  }
  p->fd = fd;
  p->pos = 0;
  p->size = (int64_t)lseek(fd, 0, SEEK_END);
  lseek(fd, 0, SEEK_SET);
  NDevSetOpen(dev);
  NDevSetBlock(dev);
  return 0;
}

int ps4_dev_close(struct ntfs_device *dev) {
  PS4DevPriv *p = (PS4DevPriv *)dev->d_private;
  if (!p)
    return -1;
  if (p->fd >= 0)
    ::close(p->fd);
  p->fd = -1;
  NDevClearOpen(dev);
  return 0;
}

s64 ps4_dev_seek(struct ntfs_device *dev, s64 offset, int whence) {
  PS4DevPriv *p = (PS4DevPriv *)dev->d_private;
  off_t r = lseek(p->fd, (off_t)offset, whence);
  if (r == (off_t)-1)
    return -1;
  p->pos = (int64_t)r;
  return (s64)r;
}

// FreeBSD / Orbis block devices require I/O aligned to a 512-byte sector.
// Unaligned read returns EINVAL. ntfs-3g asks for arbitrary offsets and
// sizes, so we bounce through an aligned temp buffer and copy out the slice
// the caller actually wanted.
constexpr int64_t kSector = 512;

s64 ps4_dev_aligned_pread(int fd, void *buf, s64 count, s64 offset) {
  if (count <= 0)
    return 0;

  int64_t alignOff = offset & ~(kSector - 1);
  int64_t headPad = offset - alignOff;
  int64_t endOff = offset + count;
  int64_t alignEnd = (endOff + kSector - 1) & ~(kSector - 1);
  int64_t blockLen = alignEnd - alignOff;

  // Fast path: already aligned, no bounce needed.
  if (headPad == 0 && (count & (kSector - 1)) == 0) {
    if (lseek(fd, (off_t)offset, SEEK_SET) == (off_t)-1)
      return -1;
    char *out = (char *)buf;
    s64 total = 0;
    while (total < count) {
      ssize_t got = ::read(fd, out + total, (size_t)(count - total));
      if (got == 0)
        break;
      if (got < 0) {
        if (errno == EINTR)
          continue;
        return total > 0 ? total : -1;
      }
      total += got;
    }
    return total;
  }

  // Slow path: aligned bounce buffer.
  void *bounce = nullptr;
  if (posix_memalign(&bounce, 4096, (size_t)blockLen) != 0 || !bounce) {
    bounce = std::malloc((size_t)blockLen);
    if (!bounce)
      return -1;
  }
  if (lseek(fd, (off_t)alignOff, SEEK_SET) == (off_t)-1) {
    std::free(bounce);
    return -1;
  }
  char *bp = (char *)bounce;
  int64_t have = 0;
  while (have < blockLen) {
    ssize_t got = ::read(fd, bp + have, (size_t)(blockLen - have));
    if (got == 0)
      break;
    if (got < 0) {
      if (errno == EINTR)
        continue;
      if (have == 0) {
        std::free(bounce);
        return -1;
      }
      break;
    }
    have += got;
  }
  int64_t useful = have - headPad;
  if (useful < 0)
    useful = 0;
  if (useful > count)
    useful = count;
  std::memcpy(buf, bp + headPad, (size_t)useful);
  std::free(bounce);
  return (s64)useful;
}

s64 ps4_dev_read(struct ntfs_device *dev, void *buf, s64 count) {
  PS4DevPriv *p = (PS4DevPriv *)dev->d_private;
  s64 got = ps4_dev_aligned_pread(p->fd, buf, count, p->pos);
  if (got > 0) {
    lseek(p->fd, (off_t)(p->pos + got), SEEK_SET);
    p->pos += got;
  }
  return got;
}

s64 ps4_dev_write(struct ntfs_device *, const void *, s64) {
  errno = EROFS;
  return -1; // we open NTFS read-only; writes refused
}

s64 ps4_dev_pread(struct ntfs_device *dev, void *buf, s64 count, s64 offset) {
  PS4DevPriv *p = (PS4DevPriv *)dev->d_private;
  s64 got = ps4_dev_aligned_pread(p->fd, buf, count, offset);
  if (got > 0) {
    p->pos = offset + got;
    lseek(p->fd, (off_t)p->pos, SEEK_SET);
  }
  return got;
}

s64 ps4_dev_pwrite(struct ntfs_device *, const void *, s64, s64) {
  errno = EROFS;
  return -1;
}

int ps4_dev_sync(struct ntfs_device *) { return 0; }

int ps4_dev_stat(struct ntfs_device *dev, struct stat *sb) {
  PS4DevPriv *p = (PS4DevPriv *)dev->d_private;
  std::memset(sb, 0, sizeof(*sb));
  sb->st_mode = S_IFBLK | 0444;
  sb->st_size = p ? (off_t)p->size : 0;
  sb->st_blksize = 512;
  return 0;
}

int ps4_dev_ioctl(struct ntfs_device *, unsigned long, void *) {
  errno = ENOTTY;
  return -1;
}

ntfs_device_operations g_ps4DevOps = {
    ps4_dev_open,  ps4_dev_close,  ps4_dev_seek, ps4_dev_read, ps4_dev_write,
    ps4_dev_pread, ps4_dev_pwrite, ps4_dev_sync, ps4_dev_stat, ps4_dev_ioctl,
};

} // namespace

NtfsManager &NtfsManager::instance() {
  static NtfsManager inst;
  return inst;
}

bool NtfsManager::mounted() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return vol_ != nullptr || useShell_;
}

bool NtfsManager::isShellMount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return useShell_;
}

bool NtfsManager::detectDevice() {
  static const char *kDevices[] = {"/dev/da0s1", "/dev/da1s1", "/dev/da0",
                                   "/dev/da1",   "/dev/da0s2", "/dev/da1s2"};
  for (auto dev : kDevices) {
    // PS4 kernel refuses POSIX open() on /dev/da* from user processes
    // ("device file is not supported" in klog). sceKernelOpen is a
    // thinner wrapper that sometimes succeeds when the jailbreak has
    // granted us the authid/sandbox exception required for raw block
    // device access.
    int fd = sceKernelOpen(dev, 0 /*O_RDONLY*/, 0);
    if (fd < 0)
      fd = open(dev, O_RDONLY);
    if (fd >= 0) {
      close(fd);
      devPath_ = dev;
      return true;
    }
  }
  return false;
}

bool NtfsManager::detectShellMount() {
  // The PS4 shell auto-mounts USB mass storage at these paths when the
  // filesystem (usually exFAT or FAT32) is recognised natively.
  static const char *kMounts[] = {
      "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/ext0", "/mnt/ext1",
  };
  for (auto path : kMounts) {
    DIR *d = opendir(path);
    if (!d)
      continue;
    bool hasEntry = false;
    while (struct dirent *e = readdir(d)) {
      if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
        continue;
      hasEntry = true;
      break;
    }
    closedir(d);
    if (!hasEntry)
      continue;
    rootPath_ = path;
    devPath_ = path;
    return true;
  }
  return false;
}

// Checks whether a block device starts with the NTFS magic "NTFS    "
// at offset 3 in its boot sector. Returns true only for real NTFS volumes;
// exFAT, FAT, GPT partition tables etc. are rejected.
static bool looksLikeNtfs(const char *devPath) {
  int fd = sceKernelOpen(devPath, 0, 0);
  if (fd < 0)
    fd = ::open(devPath, O_RDONLY);
  if (fd < 0)
    return false;
  char buf[512];
  // Must read the whole sector (block devices need 512-aligned IO).
  if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
    close(fd);
    return false;
  }
  ssize_t got = ::read(fd, buf, sizeof(buf));
  close(fd);
  if (got < 11)
    return false;
  // NTFS boot sector OEM ID = "NTFS    " at offset 3..10.
  return std::memcmp(buf + 3, "NTFS    ", 8) == 0;
}

std::vector<NtfsDeviceInfo> NtfsManager::scan() {
  std::vector<NtfsDeviceInfo> out;

  // 1. Shell mounts (exFAT/FAT auto-mounted by PS4). Each one corresponds
  // 1:1 to a /dev/daX bus number - remember which buses are already
  // handled so we don't offer the same disk twice.
  bool shellTaken[4] = {false, false, false, false};
  static const char *kMounts[] = {
      "/mnt/usb0", "/mnt/usb1", "/mnt/usb2", "/mnt/ext0", "/mnt/ext1",
  };
  for (size_t idx = 0; idx < sizeof(kMounts) / sizeof(kMounts[0]); ++idx) {
    const char *path = kMounts[idx];
    DIR *d = opendir(path);
    if (!d)
      continue;
    bool hasEntry = false;
    while (struct dirent *e = readdir(d)) {
      if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
        continue;
      hasEntry = true;
      break;
    }
    closedir(d);
    if (!hasEntry)
      continue;
    // Mark /dev/daX as "handled by shell" so step 2 doesn't duplicate it.
    if (idx < 4)
      shellTaken[idx] = true;
    NtfsDeviceInfo d_;
    d_.path = path;
    d_.label = std::string("USB (") + path + ") - exFAT/FAT";
    d_.shellMount = true;
    out.push_back(std::move(d_));
  }

  // 2. Raw NTFS block devices. Only partitions (/dev/daXsY) with a valid
  // NTFS boot sector are listed - everything else is noise / duplicates.
  static const char *kDevices[] = {
      "/dev/da0s1", "/dev/da0s2", "/dev/da0s3", "/dev/da0s4",
      "/dev/da1s1", "/dev/da1s2", "/dev/da1s3", "/dev/da1s4",
      "/dev/da2s1", "/dev/da2s2", "/dev/da2s3", "/dev/da2s4",
      "/dev/da3s1", "/dev/da3s2", "/dev/da3s3", "/dev/da3s4",
  };
  for (auto dev : kDevices) {
    // Extract bus number from "/dev/daN..." - skip if the shell already
    // owns this bus (exFAT/FAT drive mounted at /mnt/usbN).
    int bus = (dev[7] >= '0' && dev[7] <= '9') ? (dev[7] - '0') : -1;
    if (bus >= 0 && bus < 4 && shellTaken[bus])
      continue;

    if (!looksLikeNtfs(dev))
      continue; // only real NTFS volumes

    int fd = sceKernelOpen(dev, 0, 0);
    if (fd < 0)
      fd = ::open(dev, O_RDONLY);
    if (fd < 0)
      continue;
    int64_t sz = (int64_t)lseek(fd, 0, SEEK_END);
    close(fd);
    NtfsDeviceInfo d_;
    d_.path = dev;
    char b[96];
    std::snprintf(b, sizeof(b), "NTFS (%s)  %.1f GB", dev,
                  sz / (1024.0 * 1024.0 * 1024.0));
    d_.label = b;
    d_.shellMount = false;
    d_.totalSize = sz > 0 ? (uint64_t)sz : 0;
    out.push_back(std::move(d_));
  }
  return out;
}

bool NtfsManager::mountDevice(const NtfsDeviceInfo &dev) {
  unmount();

  std::lock_guard<std::mutex> lk(mutex_);
  if (dev.shellMount) {
    useShell_ = true;
    rootPath_ = dev.path;
    devPath_ = dev.path;
    curDir_ = "/";
    ntfsLog("mountDevice shell " + dev.path);
    return true;
  }

  ntfs_log_set_handler(ntfs_log_handler_null);
  devPath_ = dev.path;
  auto *priv = new PS4DevPriv{-1, 0, 0};
  ntfs_device *d = ntfs_device_alloc(devPath_.c_str(), 0, &g_ps4DevOps, priv);
  if (!d) {
    delete priv;
    ntfsLog("mountDevice: device_alloc failed");
    return false;
  }
  ntfs_volume *v = ntfs_device_mount(d, NTFS_MNT_RDONLY);
  if (!v) {
    char b[128];
    std::snprintf(b, sizeof(b),
                  "mountDevice: ntfs_device_mount(%s) failed errno=%d",
                  dev.path.c_str(), errno);
    ntfsLog(b);
    ntfs_device_free(d);
    delete priv;
    return false;
  }
  vol_ = v;
  curDir_ = "/";
  ntfsLog("mountDevice: mounted " + dev.path);
  return true;
}

bool NtfsManager::mount() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (vol_ || useShell_)
    return true;

  // Prefer the shell's existing mount (exFAT/FAT USB on PS4): it works
  // without root-level device access and is what the user plugs in.
  if (detectShellMount()) {
    useShell_ = true;
    curDir_ = "/";
    ntfsLog("shell mount " + devPath_);
    return true;
  }

  if (!detectDevice()) {
    ntfsLog("no block device found in /dev/da*");
    return false;
  }
  ntfsLog("picked " + devPath_);

  ntfs_log_set_handler(ntfs_log_handler_null);

  auto *priv = new PS4DevPriv{-1, 0, 0};
  ntfs_device *d = ntfs_device_alloc(devPath_.c_str(), 0, &g_ps4DevOps, priv);
  if (!d) {
    delete priv;
    ntfsLog("device_alloc failed");
    return false;
  }

  ntfs_volume *v = ntfs_device_mount(d, NTFS_MNT_RDONLY);
  if (!v) {
    char b[96];
    std::snprintf(b, sizeof(b), "ntfs_device_mount failed, errno=%d", errno);
    ntfsLog(b);
    ntfs_device_free(d);
    delete priv;
    return false;
  }
  vol_ = v;
  curDir_ = "/";
  char b[96];
  std::snprintf(b, sizeof(b), "mounted OK, size=%lld", (long long)priv->size);
  ntfsLog(b);
  return true;
}

void NtfsManager::unmount() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (vol_) {
    ntfs_umount((ntfs_volume *)vol_, FALSE);
    vol_ = nullptr;
  }
  useShell_ = false;
  rootPath_.clear();
  devPath_.clear();
  entries_.clear();
  curDir_ = "/";
}

std::string NtfsManager::devicePath() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return devPath_;
}

std::string NtfsManager::currentDir() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return curDir_;
}

namespace {
struct FillCtx {
  std::vector<NtfsEntry> *out;
  int calls = 0;
  int skippedDos = 0;
  int skippedUcs = 0;
  int skippedDot = 0;
};

int fillDir(void *vctx, const ntfschar *name, const int nameLen,
            const int nameType, const s64 pos, const MFT_REF mref,
            const unsigned dt_type) {
  (void)pos;
  (void)mref;
  FillCtx *ctx = (FillCtx *)vctx;
  ctx->calls++;

  // Skip the pure-DOS short name - every entry with a long name also gets
  // one of these which would otherwise double every result.
  if (nameType == FILE_NAME_DOS) {
    ctx->skippedDos++;
    return 0;
  }

  char utf8[512];
  char *dst = utf8;
  int len = ntfs_ucstombs(name, nameLen, &dst, sizeof(utf8));
  if (len <= 0 || len >= (int)sizeof(utf8)) {
    ctx->skippedUcs++;
    return 0;
  }
  utf8[len] = '\0';

  if (std::strcmp(utf8, ".") == 0) {
    ctx->skippedDot++;
    return 0;
  }
  // $ files are NTFS metadata (MFT, LogFile, ...) - hide them from the UI.
  if (utf8[0] == '$') {
    ctx->skippedDot++;
    return 0;
  }

  NtfsEntry e;
  e.name = utf8;
  e.isDir = (dt_type == NTFS_DT_DIR);
  ctx->out->push_back(std::move(e));
  return 0;
}
} // namespace

bool NtfsManager::listDir(const std::string &path,
                          std::vector<NtfsEntry> &out) {
  std::lock_guard<std::mutex> lk(mutex_);

  if (useShell_) {
    std::string full = rootPath_ + (path == "/" ? "" : path);
    DIR *d = opendir(full.c_str());
    if (!d)
      return false;
    out.clear();
    while (struct dirent *e = readdir(d)) {
      if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
        continue;
      NtfsEntry ent;
      ent.name = e->d_name;
      std::string child = full + "/" + e->d_name;
      struct stat st;
      if (stat(child.c_str(), &st) == 0) {
        ent.isDir = S_ISDIR(st.st_mode);
        if (!ent.isDir)
          ent.size = (uint64_t)st.st_size;
      } else {
        ent.isDir = (e->d_type == DT_DIR);
      }
      out.push_back(std::move(ent));
    }
    closedir(d);
    std::sort(out.begin(), out.end(),
              [](const NtfsEntry &a, const NtfsEntry &b) {
                if (a.isDir != b.isDir)
                  return a.isDir > b.isDir;
                return a.name < b.name;
              });
    return true;
  }

  if (!vol_) {
    ntfsLog("listDir: not mounted");
    return false;
  }
  ntfs_inode *dir =
      ntfs_pathname_to_inode((ntfs_volume *)vol_, nullptr, path.c_str());
  if (!dir) {
    char b[96];
    std::snprintf(b, sizeof(b), "pathname_to_inode(%s) failed errno=%d",
                  path.c_str(), errno);
    ntfsLog(b);
    return false;
  }

  out.clear();
  FillCtx ctx;
  ctx.out = &out;
  s64 pos = 0;
  int rc = ntfs_readdir(dir, &pos, &ctx, fillDir);
  ntfs_inode_close(dir);
  char b[160];
  std::snprintf(
      b, sizeof(b),
      "readdir(%s) rc=%d entries=%zu calls=%d skip(dos=%d ucs=%d dot=%d)",
      path.c_str(), rc, out.size(), ctx.calls, ctx.skippedDos, ctx.skippedUcs,
      ctx.skippedDot);
  ntfsLog(b);
  if (rc < 0)
    return false;

  std::sort(out.begin(), out.end(), [](const NtfsEntry &a, const NtfsEntry &b) {
    if (a.isDir != b.isDir)
      return a.isDir > b.isDir;
    return a.name < b.name;
  });

  for (auto &e : out) {
    if (!e.isDir) {
      std::string full = (path == "/" ? "/" : path + "/") + e.name;
      ntfs_inode *ni =
          ntfs_pathname_to_inode((ntfs_volume *)vol_, nullptr, full.c_str());
      if (ni) {
        ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
        if (na) {
          e.size = na->data_size;
          ntfs_attr_close(na);
        }
        ntfs_inode_close(ni);
      }
    }
  }
  return true;
}

bool NtfsManager::navigate(const std::string &dir) {
  std::string newPath;
  if (dir == "..")
    return navigateUp();
  if (curDir_ == "/")
    newPath = "/" + dir;
  else
    newPath = curDir_ + "/" + dir;

  std::vector<NtfsEntry> tmp;
  if (!listDir(newPath, tmp))
    return false;
  curDir_ = newPath;
  entries_ = std::move(tmp);
  return true;
}

bool NtfsManager::navigateUp() {
  if (curDir_ == "/")
    return false;
  auto pos = curDir_.rfind('/');
  std::string parent = (pos == 0) ? "/" : curDir_.substr(0, pos);
  std::vector<NtfsEntry> tmp;
  if (!listDir(parent, tmp))
    return false;
  curDir_ = parent;
  entries_ = std::move(tmp);
  return true;
}

void NtfsManager::refresh() {
  std::vector<NtfsEntry> tmp;
  if (listDir(curDir_, tmp))
    entries_ = std::move(tmp);
}

int64_t NtfsManager::readFile(const std::string &path, uint64_t offset,
                              void *buf, uint64_t len) {
  std::lock_guard<std::mutex> lk(mutex_);

  if (useShell_) {
    std::string full =
        rootPath_ + (path.size() && path[0] == '/' ? path : "/" + path);
    int fd = open(full.c_str(), O_RDONLY);
    if (fd < 0)
      return -1;
    if (lseek(fd, (off_t)offset, SEEK_SET) == (off_t)-1) {
      close(fd);
      return -1;
    }
    ssize_t got = read(fd, buf, (size_t)len);
    close(fd);
    return got;
  }

  if (!vol_)
    return -1;
  ntfs_inode *ni =
      ntfs_pathname_to_inode((ntfs_volume *)vol_, nullptr, path.c_str());
  if (!ni)
    return -1;

  ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
  if (!na) {
    ntfs_inode_close(ni);
    return -1;
  }

  if ((int64_t)offset >= na->data_size) {
    ntfs_attr_close(na);
    ntfs_inode_close(ni);
    return 0;
  }
  if (offset + len > (uint64_t)na->data_size)
    len = (uint64_t)na->data_size - offset;

  s64 got = ntfs_attr_pread(na, (s64)offset, (s64)len, buf);
  ntfs_attr_close(na);
  ntfs_inode_close(ni);
  return got;
}

int64_t NtfsManager::fileSize(const std::string &path) {
  std::lock_guard<std::mutex> lk(mutex_);

  if (useShell_) {
    std::string full =
        rootPath_ + (path.size() && path[0] == '/' ? path : "/" + path);
    struct stat st;
    if (stat(full.c_str(), &st) != 0)
      return -1;
    return (int64_t)st.st_size;
  }

  if (!vol_)
    return -1;
  ntfs_inode *ni =
      ntfs_pathname_to_inode((ntfs_volume *)vol_, nullptr, path.c_str());
  if (!ni)
    return -1;

  ntfs_attr *na = ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0);
  if (!na) {
    ntfs_inode_close(ni);
    return -1;
  }

  int64_t sz = na->data_size;
  ntfs_attr_close(na);
  ntfs_inode_close(ni);
  return sz;
}
