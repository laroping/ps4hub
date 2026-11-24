#pragma once
#include <string>

namespace YtAdBlock {

constexpr int kCategoryCount = 12;
struct Category {
  const char *id;
  const char *label;
};
extern const Category kCategories[kCategoryCount];

struct Settings {
  bool selected[kCategoryCount] = {};
  bool autoSkip = false; // false = ask for confirmation
  bool fileExists = false;
};

Settings loadSettings();
bool saveSettings(const Settings &s, std::string &err);

bool pluginInstalled();
bool iniHasEntry();

bool startInstall(const Settings &s);

bool uninstall(std::string &err);

} // namespace YtAdBlock
