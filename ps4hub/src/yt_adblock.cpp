#include "yt_adblock.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>

#include "gh_plugins.h"
#include "libc_compat.h"

namespace YtAdBlock {
namespace {

const char *kConfigDir = "/data/youtube";
const char *kConfigPath = "/data/youtube/config.json";
const char *kPluginPath = "/data/GoldHEN/plugins/yt_adblock.prx";
const char *kSection = "[CUSA01015]"; // YouTube
const char *kPrxUrl =
    "https://github.com/FourZeroCode/yt_adblock_sponsorblock_ps4/releases/"
    "latest/download/yt_adblock.prx";

bool readFile(const char *path, std::string &out) {
  out.clear();
  FILE *f = std::fopen(path, "rb");
  if (!f)
    return false;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 &&
         out.size() < (1u << 20))
    out.append(buf, n);
  std::fclose(f);
  return true;
}

} // namespace

const Category kCategories[kCategoryCount] = {
    {"sponsor", "Sponsor"},
    {"selfpromo", "Unpaid / self-promotion"},
    {"interaction", "Interaction reminders"},
    {"intro", "Intro / intermission"},
    {"outro", "Endcards / credits"},
    {"preview", "Preview / recap"},
    {"music_offtopic", "Non-music section in music video"},
    {"filler", "Filler / tangents"},
    {"hook", "Hook / greeting"},
    {"poi_highlight", "Point of interest / highlight"},
    {"exclusive_access", "Exclusive access"},
    {"chapter", "Chapter"},
};

Settings loadSettings() {
  Settings s;
  std::string text;
  if (!readFile(kConfigPath, text))
    return s; // no file: nothing selected, confirm mode
  s.fileExists = true;
  size_t a = text.find("\"categories\"");
  size_t open = a == std::string::npos ? std::string::npos : text.find('[', a);
  size_t close =
      open == std::string::npos ? std::string::npos : text.find(']', open);
  if (close != std::string::npos) {
    std::string list = text.substr(open, close - open);
    for (int i = 0; i < kCategoryCount; ++i)
      s.selected[i] = list.find(std::string("\"") + kCategories[i].id + "\"") !=
                      std::string::npos;
  }
  size_t m = text.find("\"skipMode\"");
  if (m != std::string::npos) {
    size_t q1 = text.find('"', text.find(':', m) + 1);
    size_t q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
    if (q2 != std::string::npos)
      s.autoSkip = text.substr(q1 + 1, q2 - q1 - 1) == "auto";
  }
  return s;
}

bool saveSettings(const Settings &s, std::string &err) {
  if (mkdir(kConfigDir, 0777) != 0 && errno != EEXIST) {
    err = "cannot create /data/youtube";
    return false;
  }
  std::string json = "{\n  \"categories\": [\n";
  int count = 0, done = 0;
  for (int i = 0; i < kCategoryCount; ++i)
    if (s.selected[i])
      ++count;
  for (int i = 0; i < kCategoryCount; ++i) {
    if (!s.selected[i])
      continue;
    ++done;
    json += std::string("    \"") + kCategories[i].id + "\"" +
            (done < count ? "," : "") + "\n";
  }
  json += std::string("  ],\n  \"skipMode\": \"") +
          (s.autoSkip ? "auto" : "confirm") + "\"\n}\n";
  return GhPlugins::writeFileInPlace(kConfigPath, json, 0666,
                                     err); // as the original: 0666
}

bool pluginInstalled() {
  struct stat st;
  return Compat::stat(kPluginPath, &st) == 0;
}

bool iniHasEntry() { return GhPlugins::iniHas(kSection, kPluginPath); }

bool startInstall(const Settings &settings) {
  return GhPlugins::Jobs::instance().start(
      "YouTube AdBlock", [settings](const auto &progress, std::string &result) {
        std::string err;
        progress("Saving settings...");
        if (!saveSettings(settings, err)) {
          result = "Settings not saved: " + err;
          return false;
        }
        bool update = pluginInstalled();
        progress("Downloading the latest plugin from GitHub...");
        std::string prx;
        if (!GhPlugins::httpsGet(kPrxUrl, prx, 20u << 20, err)) {
          result = "Download failed: " + err;
          return false;
        }
        if (prx.size() < 64 * 1024) {
          result = "Download failed: unexpected plugin size";
          return false;
        }
        mkdir(GhPlugins::kGoldhenDir, 0777);
        mkdir(GhPlugins::kPluginDir, 0777);
        if (!GhPlugins::writeFileInPlace(kPluginPath, prx, 0777, err)) {
          result = err;
          return false;
        }
        progress("Adding the plugin to plugins.ini...");
        if (!GhPlugins::iniAdd(kSection, kPluginPath, err)) {
          result = "plugins.ini: " + err;
          return false;
        }
        result = update ? "YouTube AdBlock updated - restart YouTube to use it"
                        : "YouTube AdBlock installed - start YouTube to use it";
        return true;
      });
}

bool uninstall(std::string &err) {
  if (!GhPlugins::iniRemove(kSection, kPluginPath, err))
    return false;
  if (unlink(kPluginPath) != 0 && errno != ENOENT) {
    err = "cannot delete the plugin file";
    return false;
  }
  if (unlink(kConfigPath) != 0 && errno != ENOENT) {
    err = "cannot delete config.json";
    return false;
  }
  rmdir(kConfigDir); // only when empty
  return true;
}

} // namespace YtAdBlock
