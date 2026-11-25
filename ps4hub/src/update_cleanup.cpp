#include "update_cleanup.h"
#include "sfo_parser.h"
#include <cstdio>
#include <vector>
#ifdef _WIN32
#include <filesystem>
#else
#include <dirent.h>
#endif

namespace UpdateCleanup {
namespace {
bool versionParts(const std::string &version, std::vector<std::string> &parts) {
  if (version.empty() || version.find_first_not_of("0123456789.") != std::string::npos)
    return false;
  size_t start = 0;
  while (start <= version.size()) {
    const size_t end = version.find('.', start);
    std::string part = version.substr(start, end == std::string::npos ?
                                              std::string::npos : end - start);
    if (part.empty()) return false;
    const size_t nonzero = part.find_first_not_of('0');
    parts.push_back(nonzero == std::string::npos ? "0" : part.substr(nonzero));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return true;
}
bool noNewer(const std::vector<std::string> &candidate,
             const std::vector<std::string> &running) {
  const size_t count = candidate.size() > running.size() ? candidate.size() : running.size();
  for (size_t i = 0; i < count; ++i) {
    const std::string a = i < candidate.size() ? candidate[i] : "0";
    const std::string b = i < running.size() ? running[i] : "0";
    if (a.size() != b.size()) return a.size() < b.size();
    if (a != b) return a < b;
  }
  return true;
}
}
size_t removeInstalledPackages(const std::string &directory,
                               const std::string &runningVersion,
                               const std::string &titleId) {
  std::vector<std::string> running;
  if (!versionParts(runningVersion, running)) return 0;
  std::vector<std::string> names;
#ifdef _WIN32
  std::error_code error;
  std::filesystem::directory_iterator entries(directory, error);
  if (error) return 0;
  for (const auto &entry : entries) {
    if (entry.is_regular_file() && !entry.is_symlink())
      names.push_back(entry.path().filename().string());
  }
#else
  DIR *dir = opendir(directory.c_str());
  if (!dir) return 0;
  while (dirent *entry = readdir(dir)) {
    if (entry->d_type == DT_REG || entry->d_type == DT_UNKNOWN)
      names.emplace_back(entry->d_name);
  }
  closedir(dir);
#endif
  size_t removed = 0;
  const std::string prefix = "PS4Hub-v", suffix = ".pkg";
  for (const auto &name : names) {
    if (name.size() <= prefix.size() + suffix.size() ||
        name.compare(0, prefix.size(), prefix) != 0 ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
      continue;
    const std::string version = name.substr(prefix.size(),
        name.size() - prefix.size() - suffix.size());
    std::vector<std::string> candidate;
    if (!versionParts(version, candidate) || !noNewer(candidate, running)) continue;
    const std::string path = directory + "/" + name;
    SfoFile sfo;
    if (!sfo.loadFromPkg(path) || sfo.str("TITLE_ID") != titleId ||
        sfo.str("APP_VER") != version)
      continue;
    if (std::remove(path.c_str()) == 0) ++removed;
    else std::printf("[Updater] Cannot remove installed update package: %s\n", path.c_str());
  }
  return removed;
}
}
