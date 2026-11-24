#pragma once
#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

struct TitleInfo {
  bool ready = false;
  bool hasSfo = false;
  std::string titleId;
  std::string title;
  std::string category;
  std::string appVer;     // APP_VER (patch level), e.g. "01.07"
  std::string version;    // VERSION (base), e.g. "01.00"
  std::string requiredFw; // from SYSTEM_VER, e.g. "5.05"
  std::string sdkVer;     // from PUBTOOLINFO sdk_ver, e.g. "5.05"
  uint64_t baseBytes = 0;
  uint64_t patchBytes = 0;
  uint64_t addonBytes = 0;
};

class TitleInspector {
public:
  static TitleInspector &instance();
  void start();
  void stop();

  TitleInfo get(const std::string &titleId);

  void invalidate(const std::string &titleId);

private:
  TitleInspector() = default;
  static void *entry(void *self);
  void worker();
  static TitleInfo inspect(const std::string &titleId);

  pthread_t thread_{};
  pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cond_ = PTHREAD_COND_INITIALIZER;
  std::atomic<bool> running_{false};
  bool started_ = false;
  std::map<std::string, TitleInfo> cache_;
  std::deque<std::string> queue_;
};

struct AddonEntry {
  bool isPatch = false;
  std::string
      label;        // DLC entitlement label (folder name), empty for the update
  std::string name; // DLC title or "Update"
  std::string version; // APP_VER of the update / add-on
  uint64_t bytes = 0;
};
namespace Addons {
std::vector<AddonEntry> list(const std::string &titleId);
bool uninstall(const std::string &titleId, const AddonEntry &e,
               std::string &err);
} // namespace Addons

namespace Fs {
// Recursive size of regular files below `path` (symlinks are not followed).
uint64_t treeSize(const std::string &path);
} // namespace Fs
