#include "app_catalog.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>

#include <SDL2/SDL.h>

#include "gh_plugins.h"
#include "network_service.h"
#include "pkg_installer.h"
#include "platform.h"
#include "sfo_parser.h"

namespace AppCatalog {

namespace {

constexpr const char *kManifestUrl = "https://pastebin.com/raw/eknpPiVc";
constexpr const char *kPkgUrlFmt = "https://pkg-zone.com/download/ps4/";
constexpr const char *kWorkDir = "/user/data/ps4hub";
constexpr int kAutoRefreshSec = 15;

// Strip leading zeros from each dot-separated segment: "01.53" -> "1.53"
std::string normalizeVersion(const std::string &v) {
  std::string out;
  size_t pos = 0;
  while (pos < v.size()) {
    size_t dot = v.find('.', pos);
    std::string seg =
        v.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
    // Strip leading zeros but keep at least one digit.
    size_t first = seg.find_first_not_of('0');
    if (first == std::string::npos)
      seg = "0";
    else
      seg = seg.substr(first);
    if (!out.empty())
      out += '.';
    out += seg;
    pos = dot == std::string::npos ? v.size() : dot + 1;
  }
  return out;
}

std::atomic<bool> g_autoRun{false};
pthread_t g_autoThread{};

std::mutex g_mutex;
std::vector<App> g_apps = {
    {"YouTube",
     "CUSA01116",
     "youtube",
     "",
     "https://pkg-zone.com/details/CUSA01116",
     "",
     App::Source::PkgZone,
     "",
     // Other regional YouTube SKUs we treat as "YouTube installed too":
     //   CUSA01015 - EU/legacy YouTube on PS4
     //   CUSA00970 - JP YouTube
     {"CUSA01015", "CUSA00970"}},
    {"Netflix",
     "CUSA00127",
     "netflix",
     "",
     "https://pkg-zone.com/details/CUSA00127",
     "",
     App::Source::PkgZone,
     "",
     // Netflix ships under multiple SKUs by region: CUSA00129 (EU), CUSA00130
     // (JP).
     {"CUSA00129", "CUSA00130"}},
    {"PS4 Explorer", "LAPY20009", "xplorer", "",
     "https://pkg-zone.com/details/LAPY20009", "", App::Source::PkgZone, ""},
    {"Apollo Saves", "APOL00004", "apollo", "",
     "https://pkg-zone.com/details/APOL00004", "", App::Source::PkgZone, ""},
    {"PSBox Cloud", "PSBX00001", "psbox", "",
     "https://github.com/RafaelNGP/psbox-cloud-gaming/releases", "",
     App::Source::GitHub, "RafaelNGP/psbox-cloud-gaming"},
    {"Moonlight", "MLST00001", "moonlight", "",
     "https://github.com/JaimeJimenezG/Moonlight-ps4/releases", "",
     App::Source::GitHub, "JaimeJimenezG/Moonlight-ps4"},
    {"ScummVM", "SCVM00001", "scummvm", "",
     "https://github.com/NichlasEk/ScummVM-PS4/releases", "",
     App::Source::GitHub, "NichlasEk/ScummVM-PS4"},
    {"Harissa64", "HRSA00001", "harissa", "",
     "https://github.com/kernel64/Harissa64-PS4/releases", "",
     App::Source::GitHub, "kernel64/Harissa64-PS4"},
};

int applyManifest(const std::string &body) {
  std::lock_guard<std::mutex> lock(g_mutex);
  int updates = 0;
  size_t pos = 0;
  while (pos < body.size()) {
    size_t nl = body.find('\n', pos);
    std::string line = body.substr(
        pos, nl == std::string::npos ? std::string::npos : nl - pos);
    pos = nl == std::string::npos ? body.size() : nl + 1;
    while (!line.empty() &&
           (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
      line.pop_back();
    size_t eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);
    if (val.empty())
      continue;
    for (auto &a : g_apps) {
      if (a.key == key) {
        a.version = normalizeVersion(val);
        ++updates;
        break;
      }
    }
  }
  return updates;
}

std::string extractJsonString(const std::string &json, const std::string &key) {
  std::string needle = "\"" + key + "\"";
  size_t p = json.find(needle);
  if (p == std::string::npos)
    return "";
  p = json.find(':', p + needle.size());
  if (p == std::string::npos)
    return "";
  p = json.find('"', p + 1);
  if (p == std::string::npos)
    return "";
  ++p;
  size_t e = json.find('"', p);
  if (e == std::string::npos)
    return "";
  return json.substr(p, e - p);
}

bool refreshGitHub(App &app, std::string &err) {
  std::string apiUrl =
      "https://api.github.com/repos/" + app.githubRepo + "/releases/latest";
  std::string json;
  if (!GhPlugins::httpsGet(apiUrl, json, 256 * 1024, err))
    return false;

  std::string tag = extractJsonString(json, "tag_name");
  if (tag.empty()) {
    err = "no tag_name in release";
    return false;
  }
  while (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
    tag.erase(0, 1);

  std::string downloadUrl;
  size_t searchPos = 0;
  while (searchPos < json.size()) {
    size_t urlPos = json.find("\"browser_download_url\"", searchPos);
    if (urlPos == std::string::npos)
      break;
    std::string url =
        extractJsonString(json.substr(urlPos), "browser_download_url");
    if (url.find(".zip") != std::string::npos ||
        url.find(".pkg") != std::string::npos) {
      downloadUrl = url;
      break;
    }
    searchPos = urlPos + 30;
  }

  std::lock_guard<std::mutex> lock(g_mutex);
  app.version = normalizeVersion(tag);
  if (!downloadUrl.empty())
    app.pageUrl = downloadUrl;
  return true;
}

bool downloadAndInstallPkgZone(
    const App &app, const std::function<void(const std::string &)> &progress,
    std::string &result) {
  mkdir(kWorkDir, 0777);
  std::string url = std::string(kPkgUrlFmt) + app.titleId + "/" + app.version;
  std::string localFile = std::string(kWorkDir) + "/" + app.titleId + ".pkg";
  std::remove(localFile.c_str());

  progress("Downloading " + app.name + " v" + app.version + "...");
  std::string err;
  if (!GhPlugins::httpsGetToFile(url, localFile, err, [&](uint64_t bytes) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
        progress("Downloading " + app.name + "... " + buf);
      })) {
    result = "Download failed: " + err;
    return false;
  }

  progress("Installing " + app.name + "...");
  std::string ip = Platform::localIp();
  if (ip.empty())
    ip = "127.0.0.1";
  int port = NetworkService::instance().port();
  std::string serveUrl = "http://" + ip + ":" + std::to_string(port) +
                         "/static/" + app.titleId + ".pkg";

  InstallResult ir{};
  if (!PkgInstaller::installDirect({serveUrl}, ip, port, Platform::userId(),
                                   ir)) {
    result = app.name + " install failed: " +
             (ir.error.empty() ? std::string("unknown") : ir.error);
    return false;
  }
  if (ir.alreadyInstalled) {
    result = app.name + " v" + app.version + " already installed";
    return true;
  }
  result = app.name + " v" + app.version + " queued (BGFT task " +
           std::to_string(ir.taskId) + ")";
  return true;
}

bool downloadAndInstallGitHub(
    const App &app, const std::function<void(const std::string &)> &progress,
    std::string &result) {
  mkdir(kWorkDir, 0777);
  std::string apiUrl =
      "https://api.github.com/repos/" + app.githubRepo + "/releases/latest";
  std::string json, err;
  progress("Checking latest " + app.name + " release...");
  if (!GhPlugins::httpsGet(apiUrl, json, 256 * 1024, err)) {
    result = "GitHub API failed: " + err;
    return false;
  }

  std::string downloadUrl;
  bool isZip = false;
  size_t searchPos = 0;
  while (searchPos < json.size()) {
    size_t urlPos = json.find("\"browser_download_url\"", searchPos);
    if (urlPos == std::string::npos)
      break;
    std::string url =
        extractJsonString(json.substr(urlPos), "browser_download_url");
    if (url.find(".pkg") != std::string::npos) {
      downloadUrl = url;
      isZip = false;
      break;
    }
    if (url.find(".zip") != std::string::npos) {
      downloadUrl = url;
      isZip = true;
    }
    searchPos = urlPos + 30;
  }
  if (downloadUrl.empty()) {
    result = "No .pkg or .zip asset in release";
    return false;
  }

  std::string localPkg = std::string(kWorkDir) + "/" + app.titleId + ".pkg";
  std::remove(localPkg.c_str());

  if (isZip) {
    progress("Downloading " + app.name + " zip...");
    std::string zipData;
    if (!GhPlugins::httpsGet(downloadUrl, zipData, 512u << 20, err)) {
      result = "Download failed: " + err;
      return false;
    }
    progress("Extracting PKG from zip...");
    std::map<std::string, std::string> files;
    if (!GhPlugins::unzip(
            zipData,
            [](const std::string &name) {
              return name.find("[PS4]") != std::string::npos &&
                     name.find(".pkg") != std::string::npos;
            },
            files, err)) {
      if (!GhPlugins::unzip(
              zipData,
              [](const std::string &name) {
                return name.size() > 4 &&
                       name.compare(name.size() - 4, 4, ".pkg") == 0;
              },
              files, err)) {
        result = "No PKG in zip: " + err;
        return false;
      }
    }
    if (files.empty()) {
      result = "No PKG found in zip archive";
      return false;
    }
    const auto &pkg = files.begin()->second;
    if (!GhPlugins::writeFileAtomic(localPkg, pkg, err)) {
      result = "Cannot save PKG: " + err;
      return false;
    }
  } else {
    progress("Downloading " + app.name + " PKG...");
    if (!GhPlugins::httpsGetToFile(
            downloadUrl, localPkg, err, [&](uint64_t bytes) {
              char buf[64];
              std::snprintf(buf, sizeof(buf), "%.1f MB",
                            bytes / (1024.0 * 1024.0));
              progress("Downloading " + app.name + "... " + buf);
            })) {
      result = "Download failed: " + err;
      return false;
    }
  }

  progress("Installing " + app.name + "...");
  std::string ip = Platform::localIp();
  if (ip.empty())
    ip = "127.0.0.1";
  int port = NetworkService::instance().port();
  std::string serveUrl = "http://" + ip + ":" + std::to_string(port) +
                         "/static/" + app.titleId + ".pkg";

  InstallResult ir{};
  if (!PkgInstaller::installDirect({serveUrl}, ip, port, Platform::userId(),
                                   ir)) {
    result = app.name + " install failed: " +
             (ir.error.empty() ? std::string("unknown") : ir.error);
    return false;
  }
  if (ir.alreadyInstalled) {
    result = app.name + " v" + app.version + " already installed";
    return true;
  }
  result = app.name + " v" + app.version + " queued (BGFT task " +
           std::to_string(ir.taskId) + ")";
  return true;
}

} // namespace

const std::vector<App> &entries() { return g_apps; }

void refreshInstalled() {
  std::lock_guard<std::mutex> lock(g_mutex);
  auto tryRead = [](const std::string &titleId, std::string &outVer) -> bool {
    SfoFile sfo;
    if (!Sfo::loadInstalled(titleId, false, sfo))
      return false;
    std::string ver = sfo.str("APP_VER");
    if (ver.empty())
      ver = sfo.str("VERSION");
    outVer = ver;
    return true;
  };
  for (auto &a : g_apps) {
    std::string ver;
    bool found = tryRead(a.titleId, ver);
    // Fall back to known alternate region IDs (e.g. YouTube is CUSA01116
    // in some regions and CUSA01015 in others - the content is the same).
    if (!found) {
      for (const auto &alt : a.aliasTitleIds) {
        if (tryRead(alt, ver)) {
          found = true;
          break;
        }
      }
    }
    a.installedVer = found ? normalizeVersion(ver) : std::string();
  }
}

bool refreshVersions(std::string &err) {
  std::string body;
  if (!GhPlugins::httpsGet(kManifestUrl, body, 64 * 1024, err))
    return false;
  int n = applyManifest(body);

  for (auto &a : g_apps) {
    if (a.source == App::Source::GitHub && !a.githubRepo.empty()) {
      std::string ghErr;
      refreshGitHub(a, ghErr);
    }
  }

  refreshInstalled();

  if (n == 0 && g_apps.back().version.empty()) {
    err = "no versions parsed";
    return false;
  }
  return true;
}

bool startInstall(const std::string &titleId, std::string &err) {
  std::string name, version;
  App appCopy;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto &a : g_apps) {
      if (a.titleId == titleId) {
        appCopy = a;
        name = a.name;
        version = a.version;
        break;
      }
    }
  }
  if (name.empty()) {
    err = "Unknown title id";
    return false;
  }
  if (version.empty()) {
    err = "Refresh versions first";
    return false;
  }

  return GhPlugins::Jobs::instance().start(
      name, [appCopy](const auto &progress, std::string &result) {
        bool ok = false;
        if (appCopy.source == App::Source::GitHub)
          ok = downloadAndInstallGitHub(appCopy, progress, result);
        else
          ok = downloadAndInstallPkgZone(appCopy, progress, result);
        if (ok)
          refreshInstalled();
        return ok;
      });
}

void *autoRefreshLoop(void *) {
  try {
    std::string err;
    refreshVersions(err);
  } catch (...) {
  }
  while (g_autoRun) {
    for (int i = 0; i < kAutoRefreshSec * 10 && g_autoRun; ++i)
      SDL_Delay(100);
    if (!g_autoRun)
      break;
    try {
      std::string err;
      refreshVersions(err);
    } catch (...) {
      // keep the loop alive; a single failed refresh must not take
      // the whole app down.
    }
  }
  return nullptr;
}

void startAutoRefresh() {
  if (g_autoRun)
    return;
  // Populate installedVer synchronously from disk so the UI can show
  // "Installed X.YY" straight away - without waiting for the manifest
  // download. It's a handful of PARAM.SFO reads, always fast.
  try {
    refreshInstalled();
  } catch (...) {
  }
  g_autoRun = true;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  if (pthread_create(&g_autoThread, &attr, autoRefreshLoop, nullptr) != 0)
    g_autoRun = false;
  pthread_attr_destroy(&attr);
}

void stopAutoRefresh() {
  if (!g_autoRun)
    return;
  g_autoRun = false;
  pthread_join(g_autoThread, nullptr);
}

} // namespace AppCatalog
