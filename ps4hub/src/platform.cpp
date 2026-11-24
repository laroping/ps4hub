#include "platform.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <orbis/_types/bgft.h>
#include <orbis/libkernel.h>

extern "C" {
#include "GoldHEN.h"
int _fstatfs(int fd, void *buf); // libkernel export (native FreeBSD fstatfs)
int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
                 const void *newp, size_t newlen);
int sysctl(const int *name, unsigned namelen, void *oldp, size_t *oldlenp,
           const void *newp, size_t newlen);
int sceKernelGetCpuTemperature(int *celsius); // libkernel export
}

namespace Platform {
namespace {

Status g_status;
BgftApi g_bgftApi{};
AppInstApi g_appInst{};
jailbreak_backup g_jbBackup{};
void *g_bgftHeap = nullptr;
constexpr size_t kBgftHeapSize = 1024 * 1024;

int32_t (*p_appInstInit)() = nullptr;
int32_t (*p_bgftInit)(OrbisBgftInitParams *) = nullptr;

std::string hex(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "0x%08X", v);
  return b;
}

// Loads a system module (optionally through SceSysmodule first) and returns a
// handle usable with sceKernelDlsym, or a negative value.
int32_t openModule(const char *path, int sysmoduleId, std::string &err) {
  if (sysmoduleId) {
    uint32_t rc =
        sceSysmoduleLoadModuleInternal((OrbisSysModuleInternal)sysmoduleId);
    if (rc != 0)
      std::printf("[PS4Hub] sysmodule %s: %s\n", path, hex(rc).c_str());
    // Not fatal by itself: LoadStartModule below is the real check.
  }
  int32_t h =
      (int32_t)sceKernelLoadStartModule(path, 0, nullptr, 0, nullptr, nullptr);
  if (h < 0)
    err = std::string("load ") + path + " failed " + hex((uint32_t)h);
  return h;
}

template <typename Fn>
bool resolve(int32_t handle, const char *name, Fn &out, std::string &err) {
  void *p = nullptr;
  if (sceKernelDlsym(handle, name, &p) != 0 || !p) {
    err = std::string("missing symbol ") + name;
    out = nullptr;
    return false;
  }
  out = reinterpret_cast<Fn>(p);
  return true;
}

// Optional entry point: null when absent, never an error.
template <typename Fn>
void resolveOptional(int32_t handle, const char *name, Fn &out) {
  std::string ignored;
  resolve(handle, name, out, ignored);
}

void initAppInstUtil() {
  std::string &err = g_status.appInstError;
  int32_t h = openModule("/system/common/lib/libSceAppInstUtil.sprx",
                         ORBIS_SYSMODULE_INTERNAL_APP_INST_UTIL, err);
  if (h < 0)
    return;
  if (!resolve(h, "sceAppInstUtilInitialize", p_appInstInit, err) ||
      !resolve(h, "sceAppInstUtilAppUnInstall", g_appInst.uninstall, err))
    return;
  resolveOptional(h, "sceAppInstUtilAppUnInstallPat", g_appInst.uninstallPatch);
  resolveOptional(h, "sceAppInstUtilAppUnInstallAddcont",
                  g_appInst.uninstallAddcont);
  resolveOptional(h, "sceAppInstUtilAppUnInstallTheme",
                  g_appInst.uninstallTheme);
  resolveOptional(h, "sceAppInstUtilAppUnInstallByUser",
                  g_appInst.uninstallByUser);
  resolveOptional(h, "sceAppInstUtilAppUnInstallTypes",
                  g_appInst.uninstallTypes);
  resolveOptional(h, "sceAppInstUtilAppExists", g_appInst.exists);
  resolveOptional(h, "sceAppInstUtilAppGetSize", g_appInst.getSize);
  resolveOptional(h, "sceAppInstUtilAppRecoverApp", g_appInst.recoverApp);
  resolveOptional(h, "sceAppInstUtilAppInstallPkg", g_appInst.installPkg);
  int32_t rc = p_appInstInit();
  if (rc != 0) {
    err = "sceAppInstUtilInitialize " + hex((uint32_t)rc);
    return;
  }
  g_status.appInstUtil = true;
}

void initBgft() {
  std::string &err = g_status.bgftError;
  int32_t h = openModule("/system/common/lib/libSceBgft.sprx",
                         ORBIS_SYSMODULE_INTERNAL_BGFT, err);
  if (h < 0)
    return;
  // Same ServiceInt entry points as RPI NextGen (registration through
  // RegisterTask with a manifest URL; patches via DebugDownloadRegisterPkg).
  if (!resolve(h, "sceBgftServiceIntInit", p_bgftInit, err) ||
      !resolve(h, "sceBgftServiceIntDownloadRegisterTask",
               g_bgftApi.registerTask, err) ||
      !resolve(h, "sceBgftServiceIntDownloadStartTask", g_bgftApi.startTask,
               err) ||
      !resolve(h, "sceBgftServiceDownloadGetProgress", g_bgftApi.getProgress,
               err))
    return;
  resolveOptional(h, "sceBgftServiceIntDebugDownloadRegisterPkg",
                  g_bgftApi.debugRegisterPkg);
  resolveOptional(h, "sceBgftServiceIntDownloadStopTask", g_bgftApi.stopTask);
  resolveOptional(h, "sceBgftServiceIntDownloadPauseTask", g_bgftApi.pauseTask);
  resolveOptional(h, "sceBgftServiceIntDownloadResumeTask",
                  g_bgftApi.resumeTask);
  resolveOptional(h, "sceBgftServiceIntDownloadUnregisterTask",
                  g_bgftApi.unregisterTask);
  resolveOptional(h, "sceBgftServiceDownloadFindTaskByContentId",
                  g_bgftApi.findTaskByContentId);

  g_bgftHeap = std::calloc(1, kBgftHeapSize);
  if (!g_bgftHeap) {
    err = "out of memory";
    return;
  }
  OrbisBgftInitParams p{};
  p.heap = g_bgftHeap;
  p.heapSize = kBgftHeapSize;
  int32_t rc = p_bgftInit(&p);
  if (rc != 0) {
    err = "sceBgftServiceIntInit " + hex((uint32_t)rc);
    std::free(g_bgftHeap);
    g_bgftHeap = nullptr;
    return;
  }
  g_status.bgft = true;
}

} // namespace

const Status &status() { return g_status; }
const BgftApi *bgft() { return g_status.bgft ? &g_bgftApi : nullptr; }
const AppInstApi *appInst() {
  return g_status.appInstUtil ? &g_appInst : nullptr;
}

void initCore() {
  sceUserServiceInitialize(nullptr);
  int32_t uid = -1;
  if (sceUserServiceGetInitialUser(&uid) == 0)
    g_status.userId = uid;

  // GoldHEN implements syscall 500. On anything else the kernel would raise
  // SIGSYS, so ignore it and let the call fail with ENOSYS instead.
  signal(SIGSYS, SIG_IGN);
  if (sys_sdk_version() > 0 && sys_sdk_jailbreak(&g_jbBackup) == 0)
    g_status.jailbroken = true;
  std::printf("[PS4Hub] user %d, jailbreak %s\n", g_status.userId,
              g_status.jailbroken ? "ok" : "unavailable");
}

void initServices() {
  initAppInstUtil();
  initBgft();
  std::printf("[PS4Hub] AppInstUtil %s %s, BGFT %s %s\n",
              g_status.appInstUtil ? "ok" : "off",
              g_status.appInstError.c_str(), g_status.bgft ? "ok" : "off",
              g_status.bgftError.c_str());
}

// Called right before leaving. Deliberately minimal: BGFT / AppInstUtil are
// not terminated (the process is about to be torn down by the system and
// their teardown is a crash risk), only the elevated credentials are dropped.
void shutdown() {
  if (g_status.jailbroken) {
    sys_sdk_unjailbreak(&g_jbBackup);
    g_status.jailbroken = false;
  }
}

// Local IPv4 of the default route: connect() a UDP socket (no packet is sent)
// and read back the bound address. Avoids a dependency on libSceNetCtl.
std::string localIp() {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0)
    return "";
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(53);
  dst.sin_addr.s_addr = htonl(0x08080808);
  std::string ip;
  if (connect(fd, (sockaddr *)&dst, sizeof(dst)) == 0) {
    sockaddr_in self{};
    socklen_t len = sizeof(self);
    char buf[INET_ADDRSTRLEN];
    if (getsockname(fd, (sockaddr *)&self, &len) == 0 &&
        self.sin_addr.s_addr != 0 &&
        inet_ntop(AF_INET, &self.sin_addr, buf, sizeof(buf)))
      ip = buf;
  }
  close(fd);
  return ip;
}

// statvfs() in the OpenOrbis libc never fills its output (it calls statfs into
// a scratch buffer and drops the result), so the old code read stack garbage
// ("250 TB"). Ask the kernel directly and decode FreeBSD's struct statfs; the
// numeric fields sit at the same offsets in every FreeBSD revision:
//   0 f_version  16 f_bsize  32 f_blocks  48 f_bavail
Storage queryStorage(const char *mountPoint) {
  Storage s;
  int fd = open(mountPoint, O_RDONLY);
  if (fd < 0)
    return s;
  alignas(8) unsigned char buf[4096];
  std::memset(buf, 0, sizeof(buf));
  int rc = _fstatfs(fd, buf);
  close(fd);
  if (rc != 0)
    return s;

  auto rd32 = [&](size_t off) {
    uint32_t v;
    std::memcpy(&v, buf + off, 4);
    return v;
  };
  auto rd64 = [&](size_t off) {
    uint64_t v;
    std::memcpy(&v, buf + off, 8);
    return v;
  };
  constexpr uint32_t kStatfsVersion = 0x20030518;
  if (rd32(0) != kStatfsVersion) {
    std::printf("[PS4Hub] statfs: unexpected version 0x%08X\n", rd32(0));
    return s;
  }
  uint64_t bsize = rd64(16);
  uint64_t blocks = rd64(32);
  int64_t bavail = (int64_t)rd64(48);
  s.totalBytes = blocks * bsize;
  s.freeBytes = bavail > 0 ? (uint64_t)bavail * bsize : 0;

  // Anything outside what a PS4 drive can hold means we decoded it wrong.
  constexpr uint64_t kMaxPlausible = 64ull << 40; // 64 TB
  s.ok = bsize > 0 && s.totalBytes > 0 && s.totalBytes <= kMaxPlausible &&
         s.freeBytes <= s.totalBytes;
  return s;
}

int32_t uninstallTitle(const std::string &titleId) {
  if (!g_status.appInstUtil)
    return -1;
  // The uninstall paths must run WITHOUT the GoldHEN jailbreak's elevated
  // credentials: with them, the shell's bookkeeping walks system tables
  // instead of per-user ones and can trash /system_data/priv/mms/app.db
  // (observed on Orbis as "database corrupted" + every tile vanishing).
  bool droppedJb = false;
  if (g_status.jailbroken) {
    sys_sdk_unjailbreak(&g_jbBackup);
    droppedJb = true;
  }

  int32_t rc = -1;

  // Preferred path: UnInstallByUser scoped to the signed-in user. The
  // shell updates that user's catalog and leaves the system tables alone.
  if (g_appInst.uninstallByUser && g_status.userId >= 0)
    rc = g_appInst.uninstallByUser(titleId.c_str(), g_status.userId);

  // Fallback to the plain UnInstall if ByUser is missing or refused the
  // request (older firmwares, or titles the per-user call does not own).
  if (rc != 0 && g_appInst.uninstall)
    rc = g_appInst.uninstall(titleId.c_str());

  if (droppedJb)
    sys_sdk_jailbreak(&g_jbBackup);
  return rc;
}

bool reboot() {
  std::string err;
  int32_t h =
      sceKernelLoadStartModule("/system/common/lib/libSceSystemStateMgr.sprx",
                               0, nullptr, 0, nullptr, nullptr);
  if (h < 0)
    return false;
  int32_t (*fn)() = nullptr;
  if (!resolve(h, "sceSystemStateMgrReboot", fn, err))
    return false;
  return fn() == 0;
}

namespace {
template <typename T> bool sysctlValue(const char *name, T &out) {
  size_t len = sizeof(T);
  T v{};
  if (sysctlbyname(name, &v, &len, nullptr, 0) != 0)
    return false;
  out = v;
  return true;
}
} // namespace

SystemStats querySystemStats() {
  SystemStats s;
  static int source = -1; // which RAM source answered first (logged once)
  int used = 0;

  // 1) Orbis page-table statistics: the console-wide figure the system's
  //    own overlays use (CPU = main RAM, GPU = video memory). Units vary by
  //    firmware, so only the ratio is shown.
  int32_t cpuTotal = 0, cpuAvail = 0, gpuTotal = 0, gpuAvail = 0;
  if (sceKernelGetPageTableStats(&cpuTotal, &cpuAvail, &gpuTotal, &gpuAvail) ==
          0 &&
      cpuTotal > 0 && cpuAvail >= 0 && cpuAvail <= cpuTotal) {
    s.ramTotal = (uint64_t)cpuTotal;
    s.ramUsed = (uint64_t)(cpuTotal - cpuAvail);
    used = 1;
  }
  // 2) FreeBSD page counters (often hidden on retail kernels).
  uint32_t pageSize = 0, total = 0, freeP = 0, inactive = 0, cache = 0;
  if (!used && sysctlValue("hw.pagesize", pageSize) &&
      sysctlValue("vm.stats.vm.v_page_count", total) &&
      sysctlValue("vm.stats.vm.v_free_count", freeP) && total > 0) {
    sysctlValue("vm.stats.vm.v_inactive_count", inactive);
    sysctlValue("vm.stats.vm.v_cache_count", cache);
    uint64_t avail = (uint64_t)freeP + inactive + cache;
    s.ramTotal = (uint64_t)total * pageSize;
    s.ramUsed = avail < total ? ((uint64_t)total - avail) * pageSize : 0;
    used = 2;
  }
  // 3) vm.vmtotal {CTL_VM, VM_TOTAL}: struct vmtotal, t_free (pages) @44;
  //    total from hw.physmem {CTL_HW, HW_PHYSMEM}.
  if (!used) {
    unsigned char vt[64] = {};
    size_t vlen = sizeof(vt);
    int mibVt[2] = {2, 1};
    uint64_t physmem = 0;
    size_t plen = sizeof(physmem);
    int mibPm[2] = {6, 5};
    if (sysctl(mibVt, 2, vt, &vlen, nullptr, 0) == 0 && vlen >= 48 &&
        sysctl(mibPm, 2, &physmem, &plen, nullptr, 0) == 0 && physmem > 0) {
      uint32_t freePages = 0;
      std::memcpy(&freePages, vt + 44, 4);
      uint64_t freeBytes = (uint64_t)freePages * (pageSize ? pageSize : 16384u);
      if (freeBytes < physmem) {
        s.ramTotal = physmem;
        s.ramUsed = physmem - freeBytes;
        used = 3;
      }
    }
  }
  if (source != used) {
    source = used;
    std::printf("[PS4Hub] RAM stats source %d (%llu/%llu)\n", used,
                (unsigned long long)s.ramUsed, (unsigned long long)s.ramTotal);
  }

  struct {
    int64_t sec;
    int64_t usec;
  } boot{}; // struct timeval, amd64
  size_t len = sizeof(boot);
  if (sysctlbyname("kern.boottime", &boot, &len, nullptr, 0) == 0 &&
      boot.sec > 0) {
    int64_t now = (int64_t)std::time(nullptr);
    if (now > boot.sec)
      s.uptimeSec = (uint64_t)(now - boot.sec);
  }
  int temp = 0;
  if (sceKernelGetCpuTemperature(&temp) == 0 && temp > 0 && temp < 130)
    s.cpuTempC = temp;
  if (!s.uptimeSec) { // the TSC starts counting at power-on
    uint64_t f = sceKernelGetTscFrequency();
    if (f)
      s.uptimeSec = sceKernelReadTsc() / f;
  }
  return s;
}

void notify(const std::string &message) {
  OrbisNotificationRequest req;
  std::memset(&req, 0, sizeof(req));
  req.type = NotificationRequest;
  req.targetId = -1;
  req.useIconImageUri = 1;
  std::snprintf(req.message, sizeof(req.message), "%s", message.c_str());
  std::snprintf(req.iconUri, sizeof(req.iconUri), "%s",
                "cxml://psnotification/tex_icon_system");
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static bool fileReadableInternal(const std::string &path) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (f) {
    std::fclose(f);
    return true;
  }
  return false;
}

std::string appRoot() {
  static std::string cached;
  if (!cached.empty() &&
      fileReadableInternal(cached == "." ? "eboot.bin"
                                         : (cached + "/eboot.bin")))
    return cached;

  if (fileReadableInternal("eboot.bin") ||
      fileReadableInternal("./eboot.bin")) {
    cached = ".";
    return cached;
  }
  if (fileReadableInternal("/app0/eboot.bin")) {
    cached = "/app0";
    return cached;
  }

  static const char *kCandidates[] = {"/mnt/sandbox/pfsmnt/CPAN00001-app0",
                                      "/mnt/sandbox/CPAN00001_000/app0",
                                      "/mnt/sandbox/CPAN00001/app0",
                                      "/mnt/pfs/CPAN00001/app0",
                                      "app0",
                                      nullptr};
  for (int i = 0; kCandidates[i]; ++i) {
    if (fileReadableInternal(std::string(kCandidates[i]) + "/eboot.bin")) {
      cached = kCandidates[i];
      return cached;
    }
  }

  DIR *d = opendir("/mnt/sandbox");
  if (d) {
    while (struct dirent *e = readdir(d)) {
      if (e->d_name[0] == '.')
        continue;
      std::string p1 = std::string("/mnt/sandbox/") + e->d_name + "/app0";
      if (fileReadableInternal(p1 + "/eboot.bin")) {
        closedir(d);
        cached = p1;
        return cached;
      }
      std::string p2 = std::string("/mnt/sandbox/") + e->d_name;
      if (fileReadableInternal(p2 + "/eboot.bin")) {
        closedir(d);
        cached = p2;
        return cached;
      }
    }
    closedir(d);
  }

  cached = ".";
  return cached;
}

std::string appPath(const std::string &rel) {
  std::string clean = rel;
  if (clean.rfind("/app0/", 0) == 0)
    clean = clean.substr(6);
  else if (clean.rfind("app0/", 0) == 0)
    clean = clean.substr(5);
  else if (!clean.empty() && clean[0] == '/')
    clean = clean.substr(1);

  if (fileReadableInternal(clean))
    return clean;
  if (fileReadableInternal("./" + clean))
    return "./" + clean;

  std::string root = appRoot();
  if (!root.empty() && root != ".") {
    std::string cand = root + "/" + clean;
    if (fileReadableInternal(cand))
      return cand;
  }

  if (fileReadableInternal("/app0/" + clean))
    return "/app0/" + clean;

  return clean;
}

void withSandbox(const std::function<void()> &fn) {
  bool wasJb = g_status.jailbroken;
  if (wasJb) {
    sys_sdk_unjailbreak(&g_jbBackup);
    g_status.jailbroken = false;
  }
  try {
    fn();
  } catch (...) {
  }
  if (wasJb) {
    if (sys_sdk_jailbreak(&g_jbBackup) == 0)
      g_status.jailbroken = true;
  }
}

bool readAppFile(const std::string &relPath, std::string &out) {
  std::string path = appPath(relPath);
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f && g_status.jailbroken) {
    withSandbox([&] {
      std::string clean = relPath;
      if (clean.rfind("/app0/", 0) == 0)
        clean = clean.substr(6);
      else if (clean.rfind("app0/", 0) == 0)
        clean = clean.substr(5);
      else if (!clean.empty() && clean[0] == '/')
        clean = clean.substr(1);
      f = std::fopen(("/app0/" + clean).c_str(), "rb");
    });
  }
  if (!f)
    return false;

  std::fseek(f, 0, SEEK_END);
  long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (sz <= 0 || sz > 50 * 1024 * 1024) {
    std::fclose(f);
    return false;
  }
  out.resize((size_t)sz);
  bool ok = std::fread(&out[0], 1, (size_t)sz, f) == (size_t)sz;
  std::fclose(f);
  return ok;
}

bool copyAppFile(const std::string &relPath, const std::string &dstPath) {
  std::string data;
  if (!readAppFile(relPath, data))
    return false;

  std::string parent = dstPath.substr(0, dstPath.rfind('/'));
  if (!parent.empty()) {
    std::string cur;
    for (size_t i = 0; i <= parent.size(); ++i) {
      if (i == parent.size() || parent[i] == '/') {
        if (cur.size() > 1)
          mkdir(cur.c_str(), 0777);
      }
      if (i < parent.size())
        cur += parent[i];
    }
  }

  FILE *f = std::fopen(dstPath.c_str(), "wb");
  if (!f)
    return false;
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  std::fclose(f);
  return ok;
}

// The system-sanctioned way for an app to quit: SceSystemService tears the
// process down and returns to the home screen. Returning from main() instead
// runs the libc exit path, which Orbis reports as a crash (CE-34878-0).
[[noreturn]] void exitToShell() {
  sceSystemServiceLoadExec("exit", nullptr);
  for (;;)
    sceKernelUsleep(
        100 * 1000); // LoadExec does not return; never fall back into main
}

} // namespace Platform
