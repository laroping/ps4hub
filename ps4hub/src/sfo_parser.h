#pragma once
#include <cstdint>
#include <map>
#include <string>

class SfoFile {
public:
  bool parse(const std::string &bytes);
  bool load(const std::string &path);
  bool loadFromPkg(const std::string &pkgPath); // PARAM.SFO inside a .pkg

  std::string str(const char *key) const;         // "" if absent
  bool num(const char *key, uint32_t &out) const; // integer entries

  bool empty() const { return strings_.empty() && ints_.empty(); }

private:
  std::map<std::string, std::string> strings_;
  std::map<std::string, uint32_t> ints_;
};

namespace Sfo {
// Title without TM / SM signs and stray '?' (shown as '?' otherwise).
std::string cleanTitle(const std::string &title);
// PARAM.SFO of an installed title: the base game (patch = false) or its
// update (patch = true), loose file or inside app.pkg / patch.pkg.
bool loadInstalled(const std::string &titleId, bool patch, SfoFile &out);
// 0x05050000 -> "5.05"
std::string firmwareString(uint32_t v);
// "gd" -> "Game", "gp" -> "Patch", "ac" -> "Add-on", ...
const char *categoryName(const std::string &category);
} // namespace Sfo
