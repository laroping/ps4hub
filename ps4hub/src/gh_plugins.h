#pragma once
// GhPlugins - GoldHEN plugin helpers for the Plugins tab:
//   - HTTPS downloads from GitHub (system sceHttp / sceSsl stack)
//   - reading .zip archives (stored / deflate)
//   - adding / removing lines in /data/GoldHEN/plugins.ini
//   - a background job runner (one job at a time)
//   - No Share Watermark & Blocks (no_share_watermark.prx, all games)
//   - Game patches: game_patch.prx + the GoldHEN patch database, and the
//     per-patch on/off files the plugin reads (patches/settings/0x<hash>.txt)
//   - Autoloader: copies the bundled exploit host from /app0 to a writable
//     path so NetworkService can serve it without re-caching every boot.
// Plugins come from the official GoldHEN_Plugins_Repository release (GPL-3.0)
// and are downloaded on the console, never bundled.
#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace GhPlugins {

constexpr const char *kGoldhenDir = "/data/GoldHEN";
constexpr const char *kPluginDir = "/data/GoldHEN/plugins";
constexpr const char *kIniPath = "/data/GoldHEN/plugins.ini";

bool httpsGet(const std::string &url, std::string &body, size_t maxBytes,
              std::string &err);
bool httpsGetToFile(
    const std::string &url, const std::string &path, std::string &err,
    const std::function<void(uint64_t bytes)> &onProgress = nullptr);
bool unzip(const std::string &zip,
           const std::function<bool(const std::string &)> &want,
           std::map<std::string, std::string> &out, std::string &err);
bool writeFileAtomic(const std::string &path, const std::string &data,
                     std::string &err, int mode = 0666);
// Same way as the original installer: open in place, force the mode with
// fchmod (plugins need 0777 or GoldHEN cannot load them into a game),
// truncate, write, fsync.
bool writeFileInPlace(const std::string &path, const std::string &data,
                      int mode, std::string &err);

void repairPermissions(); // plugins 0777, plugins.ini / config 0666

bool iniHas(const std::string &section,
            const std::string &line); // section like "[default]"
bool iniAdd(const std::string &section, const std::string &line,
            std::string &err);
bool iniRemove(const std::string &section, const std::string &line,
               std::string &err);

class Jobs {
public:
  enum class State { Idle, Running, Done, Failed };
  static Jobs &instance();
  // fn(progress, err) runs on a worker thread; progress() updates the text.
  bool start(const std::string &name,
             std::function<
                 bool(const std::function<void(const std::string &)> &progress,
                      std::string &result)>
                 fn);
  State state() const { return state_.load(); }
  bool running() const { return state_.load() == State::Running; }
  int serial() const { return serial_.load(); }
  std::string message();
  std::string name();

private:
  static void *entry(void *self);
  std::function<bool(const std::function<void(const std::string &)> &,
                     std::string &)>
      fn_;
  pthread_t thread_{};
  bool threadStarted_ = false;
  pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
  std::string message_, name_;
  std::atomic<State> state_{State::Idle};
  std::atomic<int> serial_{0};
};

bool shareEnabled();
bool startShareInstall();
bool disableShare(std::string &err);

bool patchSupportInstalled();
std::string patchDatabaseVersion();
bool startPatchSupportInstall();
bool removePatchSupport(std::string &err);

// Copies the bundled exploit host from /app0/assets/autoloader to
// /user/data/ps4hub/autoloader so NetworkService can serve it persistently.
bool autoloaderInstalled();
bool startAutoloaderInstall(); // job: copy from /app0 (may fail on PS4 - /app0
                               // not visible from worker thread)
// Synchronous install: must be called from the main thread because /app0 is
// not visible from worker threads under some PS4 jailbreaks.  Blocks UI for
// a few seconds while 96 files are copied.  Returns status string in `result`.
bool installAutoloaderSync(std::string &result);
bool removeAutoloader(std::string &err);

// Scan known PS4 locations for browser bookmark / history storage.
// Writes a report to /user/data/ps4hub/bookmark_scan.txt and returns a
// short summary line.
std::string scanBookmarkLocations();

// Delete harmless temporary files left by our own debug tools and common
// scratch output.  Never touches browser data, cookies, cache, savedata
// or other user content.  Returns a status string.
std::string cleanTemporaryFiles();

struct GamePatch {
  std::string title, name, note, author, appVer, appElf;
  uint64_t hash = 0;
  bool enabled = false;
};
bool hasPatches(const std::string &titleId);
std::vector<GamePatch> patchesFor(const std::string &titleId);
bool setPatchEnabled(const GamePatch &p, bool on, std::string &err);

} // namespace GhPlugins
