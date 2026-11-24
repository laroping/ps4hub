#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "config.h"
#define PS4HUB_WORK_DIR APP_WORK_DIR

struct InstallResult {
  int32_t taskId = -1; // -1 with success = already installed
  std::string title;
  std::string contentId;
  std::string packageType; // PS4GD / PS4AC / PS4AL / PS4DP
  uint64_t packageSize = 0;
  bool isPatch = false;
  bool alreadyInstalled = false;
  std::string iconPath;  // icon0.png extracted from the package ("" if none)
  int32_t bgftError = 0; // set when BGFT rejected the request
  std::string error;     // transport / format problem
};

namespace PkgInstaller {

bool installDirect(const std::vector<std::string> &urls,
                   const std::string &serverIp, int serverPort, int userId,
                   InstallResult &out);

// Reference manifest (PS4 PKG Linker style json listing
// "pieces":[{"url":...}]).
bool installRef(const std::string &manifestUrl, const std::string &serverIp,
                int serverPort, int userId, InstallResult &out);

// Install from a PKG file already on disk.  Reads headers directly from the
// file (no HTTP round-trip), creates a BGFT manifest pointing to serveUrl
// (which must be served by NetworkService), and starts the BGFT download.
bool installFromFile(const std::string &localPath, const std::string &serveUrl,
                     const std::string &serverIp, int serverPort, int userId,
                     InstallResult &out);

// Name is a plain file inside WORK_DIR (no separators, no "..").
bool isServableName(const std::string &name);

} // namespace PkgInstaller
