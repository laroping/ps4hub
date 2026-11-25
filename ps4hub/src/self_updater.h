#pragma once

#include <cstdint>
#include <string>

// Downloads and verifies ps4hub.pkg, then guides manual installation in GoldHEN.
// On startup, removes this app's downloaded PKGs at the running version or older.
class SelfUpdater {
public:
  enum class State {
    Idle,
    Checking,
    UpToDate,
    Available,
    Downloading,
    Verifying,
    ReadyToInstall,
    Failed
  };

  struct ReleaseInfo {
    std::string tag;
    std::string version;
    std::string assetName;
    std::string downloadUrl;
    std::string sha256;
    uint64_t sizeBytes = 0;
  };

  struct Snapshot {
    State state = State::Idle;
    bool busy = false;
    bool updateAvailable = false;
    ReleaseInfo release;
    uint64_t bytesDownloaded = 0;
    uint64_t totalBytes = 0;
    std::string savedPkgPath;
    std::string message;
    unsigned eventSerial = 0;
    bool eventError = false;
    std::string eventMessage;
  };

  static SelfUpdater &instance();

  // Runs a quiet background check once on startup; notifies only if a newer
  // release is found.
  void startAutoCheck();

  bool startCheck();
  bool startUpdate();
  static std::string manualInstallInstructions(const std::string &path);

  Snapshot snapshot() const;

  static int compareVersions(const std::string &a, const std::string &b);
  static bool sha256File(const std::string &path, std::string &hexOut,
                         std::string &err);

private:
  SelfUpdater() = default;

  enum class Action {
    QuietCheck,
    ManualCheck,
    Download
  };
  bool launch(Action action);
  static void *threadEntry(void *arg);

  void runCheck(bool quiet);
  void runUpdate();
  void setProgress(State st, const std::string &msg, uint64_t dlBytes = 0,
                   uint64_t totalBytes = 0);
  void finishEvent(State st, const std::string &msg, bool error,
                   const std::string &savedPath = std::string());
};
