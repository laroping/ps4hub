#pragma once
#include <cstddef>
#include <string>

namespace UpdateCleanup {
// Removes only updater-owned PS4Hub-v<version>.pkg files whose embedded title
// and version match, and whose version is no newer than the running app.
size_t removeInstalledPackages(const std::string &directory,
                               const std::string &runningVersion,
                               const std::string &titleId);
}
