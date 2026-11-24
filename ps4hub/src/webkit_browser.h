#pragma once
#include <string>

namespace WebkitBrowser {

std::string resolvedTitleId();

bool backupExists();

bool isInstalled();

bool startInstall(const std::string &autoloaderUrl);

bool startRestore();

std::string addLoaderBookmark(const std::string &url);

std::string removeLoaderBookmark();

} // namespace WebkitBrowser
