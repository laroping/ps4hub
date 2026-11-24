#pragma once
// Platform - thin wrapper over the PS4 system services the app depends on.
//
// Privileged modules (libSceBgft, libSceAppInstUtil) are NOT linked as
// imports: calling an import whose module never loaded jumps to an unresolved
// NID and kills the process (CE-34878-0). They are loaded at runtime and
// their entry points resolved with sceKernelDlsym; if anything fails the
// feature is reported as unavailable instead.
#include <cstdint>
#include <functional>
#include <string>

#include "bgft_api.h"

namespace Platform {

struct Status {
  bool jailbroken = false; // GoldHEN SDK jailbreak (needed for /system_data)
  bool appInstUtil = false;
  bool bgft = false;
  int userId = -1;
  std::string bgftError; // why BGFT is unavailable (for the UI)
  std::string appInstError;
};

using BgftApi = Bgft::Api;

// SceAppInstUtil entry points resolved at runtime; optional ones may be null.
struct AppInstApi {
  int32_t (*uninstall)(const char *titleId);
  int32_t (*uninstallPatch)(const char *titleId);
  int32_t (*uninstallAddcont)(const char *titleId, const char *label);
  int32_t (*uninstallTheme)(const char *contentId);
  int32_t (*uninstallByUser)(const char *titleId,
                             int32_t userId); // optional (per-user path)
  int32_t (*uninstallTypes)(const char *titleId, uint32_t deleteTypes);
  int32_t (*exists)(const char *titleId, int32_t *exists);
  int32_t (*getSize)(const char *titleId, uint64_t *size);
  int32_t (*recoverApp)(const char *titleId); // optional (re-registers an app)
  int32_t (*installPkg)(const char *pkgPath,
                        void *reserved); // optional (direct install)
};
const AppInstApi *appInst(); // nullptr when unavailable

void initCore();
void initServices();
void shutdown();

const Status &status();
const BgftApi *bgft(); // nullptr when unavailable

inline bool bgftReady() { return status().bgft; }
inline int userId() { return status().userId; }

std::string localIp();

struct Storage {
  bool ok = false;
  uint64_t totalBytes = 0;
  uint64_t freeBytes = 0;
};
Storage queryStorage(const char *mountPoint);

int32_t uninstallTitle(const std::string &titleId);

[[noreturn]] void exitToShell();

// Restarts the console (SceSystemStateMgr, resolved at runtime). Returns
// false if the call is unavailable; on success the system takes over.
bool reboot();

// Console-wide memory and uptime for the status bar (zero when unknown).
struct SystemStats {
  uint64_t ramTotal = 0;
  uint64_t ramUsed = 0;
  uint64_t uptimeSec = 0;
  int cpuTempC = -1; // CPU temperature in degrees C, -1 when unknown
};
SystemStats querySystemStats();

void notify(const std::string &message);

std::string appRoot();
std::string appPath(const std::string &rel);
bool readAppFile(const std::string &relPath, std::string &out);
bool copyAppFile(const std::string &relPath, const std::string &dstPath);

// Temporarily restores sandbox credentials, runs fn(), and returns to
// jailbreak.
void withSandbox(const std::function<void()> &fn);

} // namespace Platform
