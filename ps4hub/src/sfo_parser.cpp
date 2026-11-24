#include "sfo_parser.h"

#include <vector>

#include <cstdio>
#include <cstring>

namespace {

uint16_t le16(const unsigned char *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t le32(const unsigned char *p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

constexpr uint32_t kMagic = 0x46535000u; // "\0PSF"
constexpr uint16_t kFmtUtf8Special = 0x0004, kFmtUtf8 = 0x0204,
                   kFmtInt = 0x0404;

} // namespace

bool SfoFile::parse(const std::string &b) {
  strings_.clear();
  ints_.clear();
  const unsigned char *p = (const unsigned char *)b.data();
  if (b.size() < 0x14 || le32(p) != kMagic)
    return false;
  uint32_t keys = le32(p + 0x08), data = le32(p + 0x0C), count = le32(p + 0x10);
  if (count > 4096)
    return false;
  for (uint32_t i = 0; i < count; ++i) {
    size_t e = 0x14 + (size_t)i * 16;
    if (e + 16 > b.size())
      break;
    size_t ko = (size_t)keys + le16(p + e);
    uint16_t fmt = le16(p + e + 2);
    uint32_t len = le32(p + e + 4);
    size_t vo = (size_t)data + le32(p + e + 12);
    if (ko >= b.size() || vo + len > b.size())
      continue;
    std::string key((const char *)p + ko,
                    strnlen((const char *)p + ko, b.size() - ko));
    if (fmt == kFmtInt && len >= 4) {
      ints_[key] = le32(p + vo);
    } else if (fmt == kFmtUtf8 || fmt == kFmtUtf8Special) {
      std::string v((const char *)p + vo, len);
      size_t nul = v.find('\0');
      if (nul != std::string::npos)
        v.resize(nul);
      strings_[key] = v;
    }
  }
  return !empty();
}

bool SfoFile::load(const std::string &path) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  std::string b;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && b.size() < (1u << 20))
    b.append(buf, n);
  std::fclose(f);
  return parse(b);
}

std::string SfoFile::str(const char *key) const {
  auto it = strings_.find(key);
  return it == strings_.end() ? std::string() : it->second;
}

bool SfoFile::num(const char *key, uint32_t &out) const {
  auto it = ints_.find(key);
  if (it == ints_.end())
    return false;
  out = it->second;
  return true;
}

namespace Sfo {

std::string firmwareString(uint32_t v) {
  if (!v)
    return "";
  char b[16];
  std::snprintf(b, sizeof(b), "%x.%02x", (v >> 24) & 0xFF, (v >> 16) & 0xFF);
  return b;
}

const char *categoryName(const std::string &c) {
  if (c == "gd")
    return "Game";
  if (c == "gp")
    return "Patch";
  if (c.rfind("ac", 0) == 0)
    return "Add-on";
  if (c == "gde")
    return "Application";
  if (c.rfind("gd", 0) == 0)
    return "Application";
  if (c.empty())
    return "";
  return "Other";
}

} // namespace Sfo

// PARAM.SFO stored inside a PS4 package (entry id 0x1000, not encrypted).
// Installed games keep it only there: /user/app/<id>/app.pkg.
bool SfoFile::loadFromPkg(const std::string &pkgPath) {
  FILE *f = std::fopen(pkgPath.c_str(), "rb");
  if (!f)
    return false;
  auto be32 = [](const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
  };
  bool ok = false;
  unsigned char head[0x20];
  if (std::fread(head, 1, sizeof(head), f) == sizeof(head) && head[0] == 0x7F &&
      head[1] == 'C' && head[2] == 'N' && head[3] == 'T') {
    uint32_t count = be32(head + 0x10), tableOff = be32(head + 0x18);
    if (count > 0 && count <= 0x1000 &&
        std::fseek(f, (long)tableOff, SEEK_SET) == 0) {
      std::string table(count * 0x20u, '\0');
      if (std::fread(&table[0], 1, table.size(), f) == table.size()) {
        for (uint32_t i = 0; i < count && !ok; ++i) {
          const unsigned char *e =
              (const unsigned char *)table.data() + i * 0x20u;
          if (be32(e) != 0x1000)
            continue;
          uint32_t off = be32(e + 0x10), size = be32(e + 0x14);
          if (!size || size > 0x100000 ||
              std::fseek(f, (long)off, SEEK_SET) != 0)
            break;
          std::string b(size, '\0');
          if (std::fread(&b[0], 1, size, f) == size)
            ok = parse(b);
          break;
        }
      }
    }
  }
  std::fclose(f);
  return ok;
}

namespace Sfo {
bool loadInstalled(const std::string &id, bool patch, SfoFile &out) {
  std::vector<std::string> paths;
  if (patch) {
    paths = {"/user/patch/" + id + "/sce_sys/param.sfo",
             "/user/patch/" + id + "/patch.pkg",
             "/mnt/ext0/user/patch/" + id + "/patch.pkg"};
  } else {
    paths = {"/user/app/" + id + "/sce_sys/param.sfo",
             "/user/app/" + id + "/app.pkg",
             "/user/appmeta/" + id + "/param.sfo",
             "/mnt/ext0/user/app/" + id + "/app.pkg",
             "/user/appmeta/external/" + id + "/param.sfo",
             "/system_ex/app/" + id + "/sce_sys/param.sfo"};
  }
  for (const auto &p : paths) {
    SfoFile f;
    bool ok = p.size() > 4 && p.compare(p.size() - 4, 4, ".pkg") == 0
                  ? f.loadFromPkg(p)
                  : f.load(p);
    if (ok && !f.empty()) {
      out = f;
      return true;
    }
  }
  return false;
}
} // namespace Sfo

namespace Sfo {
// Keeps only characters the UI font draws (Latin, Greek, Cyrillic,
// punctuation, currency). Trademark signs, '?' that app.db stores in their
// place, and scripts the font lacks (e.g. Japanese) are dropped without a
// replacement; doubled spaces are collapsed. May return "" - callers then
// show the title id / label instead.
std::string cleanTitle(const std::string &in) {
  std::string out;
  for (size_t i = 0; i < in.size();) {
    unsigned char c = (unsigned char)in[i];
    uint32_t cp;
    size_t n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c >> 5) == 6) {
      cp = c & 0x1Fu;
      n = 2;
    } else if ((c >> 4) == 14) {
      cp = c & 0x0Fu;
      n = 3;
    } else if ((c >> 3) == 30) {
      cp = c & 0x07u;
      n = 4;
    } else {
      ++i;
      continue;
    } // stray byte
    if (i + n > in.size())
      break;
    bool valid = true;
    for (size_t k = 1; k < n; ++k) {
      unsigned char cc = (unsigned char)in[i + k];
      if ((cc & 0xC0) != 0x80) {
        valid = false;
        break;
      }
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (!valid) {
      ++i;
      continue;
    }
    bool keep = (cp >= 0x20 && cp < 0x7F && cp != '?') ||
                (cp >= 0xA0 && cp <= 0x24F) || (cp >= 0x370 && cp <= 0x4FF) ||
                (cp >= 0x2010 && cp <= 0x205E) ||
                (cp >= 0x20A0 && cp <= 0x20BF);
    if (keep) {
      if (cp == ' ' && (out.empty() || out.back() == ' ')) {
        i += n;
        continue;
      }
      out.append(in, i, n);
    }
    i += n;
  }
  while (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}
} // namespace Sfo
