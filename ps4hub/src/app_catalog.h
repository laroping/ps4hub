#pragma once
#include <string>
#include <vector>

namespace AppCatalog {

struct App {
  std::string name;
  std::string titleId;
  std::string key;
  std::string version;
  std::string pageUrl;
  std::string installedVer;

  enum class Source { PkgZone, GitHub };
  Source source = Source::PkgZone;
  std::string githubRepo;

  // Alternative title IDs that count as "installed" too (same app under a
  // different region code - e.g. YouTube is CUSA01116 in US, CUSA01015 in EU).
  std::vector<std::string> aliasTitleIds;
};

const std::vector<App> &entries();

bool refreshVersions(std::string &err);

void refreshInstalled();

bool startInstall(const std::string &titleId, std::string &err);

void startAutoRefresh();
void stopAutoRefresh();

} // namespace AppCatalog
