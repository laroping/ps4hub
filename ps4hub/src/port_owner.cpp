#include "port_owner.h"

#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "catalog_db.h"
#include "config.h"

extern "C" {
#include "GoldHEN.h"
// libkernel exports; the toolchain ships no <sys/sysctl.h>.
int sysctl(const int *name, unsigned namelen, void *oldp, size_t *oldlenp,
           const void *newp, size_t newlen);
}

namespace {

// FreeBSD sysctl MIB numbers (stable ABI).
constexpr int CTL_KERN_ = 1;
constexpr int KERN_PROC_ = 14;
constexpr int KERN_PROC_PROC_ = 8;
constexpr int KERN_PROC_FILEDESC_ = 33;

// struct kinfo_proc: ki_structsize @0, ki_pid @72.
constexpr size_t kKiPidOffset = 72;
// struct kinfo_file: kf_structsize @0, kf_type @4, kf_sock_domain @36,
// kf_sock_type @40, kf_sa_local (sockaddr_storage) @48.
constexpr int kKfTypeSocket = 2;
constexpr size_t kKfMinSize = 48 + 16;

template <typename T> T rd(const unsigned char *p, size_t off) {
  T v;
  std::memcpy(&v, p + off, sizeof(T));
  return v;
}

bool fetch(const int *mib, unsigned len, std::vector<unsigned char> &buf) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    size_t size = 0;
    if (sysctl(mib, len, nullptr, &size, nullptr, 0) != 0 || size == 0)
      return false;
    size += size / 4 + 4096; // the table can grow between the two calls
    buf.resize(size);
    if (sysctl(mib, len, buf.data(), &size, nullptr, 0) == 0) {
      buf.resize(size);
      return true;
    }
  }
  return false;
}

// struct kinfo_proc (FreeBSD 9 amd64): ki_comm[20] @447.
constexpr size_t kKiCommOffset = 447;
constexpr size_t kKiCommLen = 20;
constexpr int KERN_PROC_PID_ = 1;

struct ProcEntry {
  int pid = -1;
  std::string comm; // process name from the kernel's process table
  std::string raw;  // printable text of the whole record (fallback matching)
};

ProcEntry parseKinfo(const unsigned char *rec, size_t size) {
  ProcEntry e;
  e.pid = rd<int>(rec, kKiPidOffset);
  if (size >= kKiCommOffset + kKiCommLen) {
    const char *c = reinterpret_cast<const char *>(rec + kKiCommOffset);
    size_t n = 0;
    while (n < kKiCommLen && c[n] >= 0x20 && c[n] < 0x7F)
      ++n;
    if (n < kKiCommLen && c[n] == '\0')
      e.comm.assign(c, n);
  }
  e.raw.reserve(size);
  for (size_t i = 0; i < size; ++i)
    e.raw.push_back(rec[i] >= 0x20 && rec[i] < 0x7F ? (char)rec[i] : '\0');
  return e;
}

std::vector<ProcEntry> listProcs() {
  std::vector<ProcEntry> out;
  int mib[3] = {CTL_KERN_, KERN_PROC_, KERN_PROC_PROC_};
  std::vector<unsigned char> buf;
  if (fetch(mib, 3, buf)) {
    for (size_t off = 0; off + kKiPidOffset + 4 <= buf.size();) {
      int size = rd<int>(buf.data(), off);
      if (size < (int)(kKiPidOffset + 4) || size > 8192 ||
          off + (size_t)size > buf.size())
        break;
      out.push_back(parseKinfo(buf.data() + off, (size_t)size));
      off += (size_t)size;
    }
  }
  if (out.empty()) { // fallback: ask for each plausible pid on its own
    for (int p = 1; p < 2048; ++p) {
      int m[4] = {CTL_KERN_, KERN_PROC_, KERN_PROC_PID_, p};
      std::vector<unsigned char> one;
      if (!fetch(m, 4, one) || one.size() < kKiPidOffset + 4)
        continue;
      out.push_back(parseKinfo(one.data(), one.size()));
    }
  }
  return out;
}

std::vector<int> listPids() {
  std::vector<int> pids;
  for (auto &e : listProcs())
    pids.push_back(e.pid);
  return pids;
}

bool processHasPort(int pid, uint16_t port) {
  int mib[4] = {CTL_KERN_, KERN_PROC_, KERN_PROC_FILEDESC_, pid};
  std::vector<unsigned char> buf;
  if (!fetch(mib, 4, buf))
    return false;
  for (size_t off = 0; off + kKfMinSize <= buf.size();) {
    const unsigned char *kf = buf.data() + off;
    int size = rd<int>(kf, 0);
    if (size < (int)kKfMinSize || off + (size_t)size > buf.size())
      break;
    if (rd<int>(kf, 4) == kKfTypeSocket && rd<int>(kf, 36) == 2 /*AF_INET*/ &&
        rd<int>(kf, 40) == 1 /*SOCK_STREAM*/) {
      // FreeBSD sockaddr_in: sin_len, sin_family, sin_port (big endian)
      uint8_t family = kf[48 + 1];
      uint16_t p = (uint16_t)((kf[48 + 2] << 8) | kf[48 + 3]);
      if (family == 2 && p == port)
        return true;
    }
    off += (size_t)size;
  }
  return false;
}

// Name of a process from the kernel table. (GoldHEN's sys_sdk_proc_info
// only describes the calling process, so it cannot be used for others.)
void identify(PortOwnerInfo &p) {
  int m[4] = {CTL_KERN_, KERN_PROC_, KERN_PROC_PID_, p.pid};
  std::vector<unsigned char> one;
  if (fetch(m, 4, one) && one.size() >= kKiPidOffset + 4)
    p.name = parseKinfo(one.data(), one.size()).comm;
}

} // namespace

namespace PortOwner {

bool find(uint16_t port, PortOwnerInfo &out) {
  const int self = getpid();
  for (int pid : listPids()) {
    if (pid <= 0 || pid == self)
      continue;
    if (!processHasPort(pid, port))
      continue;
    out = PortOwnerInfo{};
    out.pid = pid;
    identify(out);
    return true;
  }
  return false;
}

bool isProtected(const PortOwnerInfo &p) {
  if (p.pid <= 1 || p.pid == getpid())
    return true;
  // Only application processes ("eboot.bin": games and homebrew) may be
  // stopped; system services (Sce*), daemons and anything unidentified
  // are always left alone.
  if (p.name != "eboot.bin")
    return true;
  if (!p.titleId.empty() &&
      (p.titleId.rfind("NPXS", 0) == 0 || p.titleId == APP_TITLE_ID))
    return true;
  return false;
}

bool terminate(const PortOwnerInfo &p) {
  if (isProtected(p))
    return false;
  return kill(p.pid, SIGKILL) == 0;
}

std::string describe(const PortOwnerInfo &p) {
  std::string s =
      !p.titleId.empty()
          ? p.titleId
          : (p.name.empty() ? std::string("unknown process") : p.name);
  if (!p.titleId.empty() && !p.name.empty())
    s += " (" + p.name + ")";
  return s + ", pid " + std::to_string(p.pid);
}

bool findProcess(const char *name, PortOwnerInfo &out) {
  const int self = getpid();
  std::vector<ProcEntry> procs = listProcs();
  // Exact name first, then the name anywhere in the record (in case this
  // firmware's kinfo_proc layout moves ki_comm).
  for (int pass = 0; pass < 2; ++pass)
    for (auto &e : procs) {
      if (e.pid <= 1 || e.pid == self)
        continue;
      bool hit =
          pass == 0 ? e.comm == name : e.raw.find(name) != std::string::npos;
      if (!hit)
        continue;
      out = PortOwnerInfo{};
      out.pid = e.pid;
      out.name = name;
      return true;
    }
  // Not found: leave a process list for diagnosis.
  if (FILE *f = std::fopen(APP_WORK_DIR "/processes.txt", "w")) {
    std::fprintf(f, "%zu processes\n", procs.size());
    for (auto &e : procs)
      std::fprintf(f, "%d\t%s\n", e.pid, e.comm.c_str());
    std::fclose(f);
  }
  return false;
}

} // namespace PortOwner
