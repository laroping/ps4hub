#include "net_profiles.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <orbis/libkernel.h>

#include "config.h"
#include "gh_plugins.h"

namespace NetProfiles {
namespace {

// Registry entries of the wired / general connection (type 0 = int,
// 1 = string with its maximum size). Lease data is left out on purpose.
struct Key {
  uint32_t id;
  int type;
  uint32_t size;
  const char *path;
};
const Key kKeys[] = {
    {0x14140100, 1, 0x10, "/NET/IP/ip_address"},
    {0x14140200, 1, 0x10, "/NET/IP/netmask"},
    {0x14140300, 1, 0x10, "/NET/IP/default_route"},
    {0x14140400, 0, 4, "/NET/IP/dns_flag"},
    {0x14140500, 1, 0x10, "/NET/IP/primary_dns"},
    {0x14140600, 1, 0x10, "/NET/IP/secondary_dns"},
    {0x14140700, 0, 4, "/NET/IP/ip_config"},
    {0x14140800, 1, 0x100, "/NET/IP/dhcp_hostname"},
    {0x14140C00, 1, 0x80, "/NET/IP/auth_name"},
    {0x14140D00, 1, 0x80, "/NET/IP/auth_key"},
    {0x14190100, 0, 4, "/NET/COMMON/device"},
    {0x14190600, 0, 4, "/NET/COMMON/net_flag"},
    {0x14190700, 0, 4, "/NET/COMMON/conf_type"},
    {0x14190800, 0, 4, "/NET/COMMON/mtu"},
    {0x14190900, 0, 4, "/NET/COMMON/ether_mode"},
    {0x14280100, 0, 4, "/NET/APP/http_proxy_flag"},
    {0x14280200, 1, 0x100, "/NET/APP/http_proxy_svr"},
    {0x14280300, 0, 4, "/NET/APP/http_proxy_port"},
};

const char *kDir = APP_WORK_DIR "/network";

int (*p_getInt)(uint32_t, int32_t *) = nullptr;
int (*p_setInt)(uint32_t, int32_t) = nullptr;
int (*p_getStr)(uint32_t, char *, uint32_t) = nullptr;
int (*p_setStr)(uint32_t, const char *, uint32_t) = nullptr;
std::string g_loadError;
bool g_tried = false;

bool load() {
  if (g_tried)
    return p_getInt && p_setInt && p_getStr && p_setStr;
  g_tried = true;
  int32_t h = sceKernelLoadStartModule("/system/common/lib/libSceRegMgr.sprx",
                                       0, nullptr, 0, nullptr, nullptr);
  if (h < 0) {
    char b[64];
    std::snprintf(b, sizeof(b), "registry module not available (0x%08X)",
                  (uint32_t)h);
    g_loadError = b;
    return false;
  }
  void *f = nullptr;
  if (sceKernelDlsym(h, "sceRegMgrGetInt", &f) == 0)
    p_getInt = (int (*)(uint32_t, int32_t *))f;
  if (sceKernelDlsym(h, "sceRegMgrSetInt", &f) == 0)
    p_setInt = (int (*)(uint32_t, int32_t))f;
  if (sceKernelDlsym(h, "sceRegMgrGetStr", &f) == 0)
    p_getStr = (int (*)(uint32_t, char *, uint32_t))f;
  if (sceKernelDlsym(h, "sceRegMgrSetStr", &f) == 0)
    p_setStr = (int (*)(uint32_t, const char *, uint32_t))f;
  if (!(p_getInt && p_setInt && p_getStr && p_setStr)) {
    g_loadError = "registry functions not found";
    return false;
  }
  return true;
}

std::string hex(const std::string &s) {
  static const char *d = "0123456789abcdef";
  std::string o;
  for (unsigned char c : s) {
    o += d[c >> 4];
    o += d[c & 15];
  }
  return o;
}
std::string unhex(const std::string &h) {
  std::string o;
  for (size_t i = 0; i + 1 < h.size(); i += 2)
    o += (char)std::strtol(h.substr(i, 2).c_str(), nullptr, 16);
  return o;
}

struct Value {
  int type = 0;
  int32_t i = 0;
  std::string s;
  bool ok = false;
};
using Snapshot = std::vector<std::pair<const Key *, Value>>;

bool readAll(Snapshot &out, std::string &err) {
  if (!load()) {
    err = g_loadError;
    return false;
  }
  out.clear();
  for (const Key &k : kKeys) {
    Value v;
    v.type = k.type;
    if (k.type == 0) {
      v.ok = p_getInt(k.id, &v.i) == 0;
    } else {
      std::vector<char> buf(k.size + 1, 0);
      v.ok = p_getStr(k.id, buf.data(), k.size) == 0;
      v.s = std::string(buf.data(), strnlen(buf.data(), k.size));
    }
    out.push_back({&k, v});
  }
  for (auto &e : out)
    if (e.second.ok)
      return true;
  err = "the network settings could not be read";
  return false;
}

const Value *find(const Snapshot &s, uint32_t id) {
  for (auto &e : s)
    if (e.first->id == id && e.second.ok)
      return &e.second;
  return nullptr;
}

std::string summaryOf(const Snapshot &s) {
  auto str = [&](uint32_t id) {
    const Value *v = find(s, id);
    return v ? v->s : std::string();
  };
  std::string ip = str(0x14140100), gw = str(0x14140300), dns = str(0x14140500);
  std::string out;
  out += ip.empty() || ip == "0.0.0.0" ? "IP automatic" : "IP " + ip;
  if (!gw.empty() && gw != "0.0.0.0")
    out += ", gateway " + gw;
  out += dns.empty() || dns == "0.0.0.0" ? ", DNS automatic" : ", DNS " + dns;
  if (!str(0x14140C00).empty())
    out += ", PPPoE";
  return out;
}

bool writeSnapshot(const Snapshot &s, const std::string &name,
                   const std::string &path, std::string &err) {
  mkdir(APP_WORK_DIR, 0777);
  mkdir(kDir, 0777);
  std::string text = "name\t" + name + "\n";
  for (auto &e : s) {
    if (!e.second.ok)
      continue;
    char id[16];
    std::snprintf(id, sizeof(id), "%08x", e.first->id);
    text += std::string(id) + "\t" +
            (e.first->type == 0 ? "i\t" + std::to_string(e.second.i)
                                : "s\t" + hex(e.second.s)) +
            "\n";
  }
  return GhPlugins::writeFileAtomic(path, text, err);
}

bool readSnapshot(const std::string &path, Snapshot &s, std::string &name) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  char line[1024];
  while (std::fgets(line, sizeof(line), f)) {
    std::string l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
      l.pop_back();
    size_t t1 = l.find('\t');
    if (t1 == std::string::npos)
      continue;
    std::string a = l.substr(0, t1), rest = l.substr(t1 + 1);
    if (a == "name") {
      name = rest;
      continue;
    }
    size_t t2 = rest.find('\t');
    if (t2 == std::string::npos)
      continue;
    uint32_t id = (uint32_t)std::strtoul(a.c_str(), nullptr, 16);
    const Key *key = nullptr;
    for (const Key &k : kKeys)
      if (k.id == id)
        key = &k;
    if (!key)
      continue; // only the known network keys are ever written
    Value v;
    v.type = key->type;
    v.ok = true;
    if (rest[0] == 'i' && key->type == 0)
      v.i = (int32_t)std::strtol(rest.c_str() + t2 + 1, nullptr, 10);
    else if (rest[0] == 's' && key->type == 1)
      v.s = unhex(rest.substr(t2 + 1));
    else
      continue;
    if (v.type == 1 && v.s.size() >= key->size)
      continue; // never write past the entry size
    s.push_back({key, v});
  }
  std::fclose(f);
  return !s.empty();
}

std::string stamp() {
  char b[32];
  std::time_t now = std::time(nullptr);
  std::tm lt{};
  localtime_r(&now, &lt);
  std::strftime(b, sizeof(b), "%Y%m%d-%H%M%S", &lt);
  return b;
}

} // namespace

bool available(std::string &err) {
  if (load())
    return true;
  err = g_loadError;
  return false;
}

std::string currentSummary() {
  Snapshot s;
  std::string err;
  if (!readAll(s, err))
    return "Unavailable: " + err;
  return summaryOf(s);
}

bool saveCurrent(std::string &savedName, std::string &err) {
  Snapshot s;
  if (!readAll(s, err))
    return false;
  const Value *ip = find(s, 0x14140100);
  bool pppoe = find(s, 0x14140C00) && !find(s, 0x14140C00)->s.empty();
  savedName =
      pppoe ? "PPPoE"
            : (ip && !ip->s.empty() && ip->s != "0.0.0.0" ? "LAN " + ip->s
                                                          : "Automatic (DHCP)");
  return writeSnapshot(s, savedName,
                       std::string(kDir) + "/net-" + stamp() + ".txt", err);
}

std::vector<Profile> list() {
  std::vector<Profile> out;
  unlink((std::string(kDir) + "/zz-before-apply.txt")
             .c_str()); // undo copies are no longer made
  DIR *d = opendir(kDir);
  if (!d)
    return out;
  while (struct dirent *e = readdir(d)) {
    std::string n = e->d_name;
    if (n.size() < 5 || n.compare(n.size() - 4, 4, ".txt") != 0)
      continue;
    Profile p;
    p.file = std::string(kDir) + "/" + n;
    Snapshot s;
    if (!readSnapshot(p.file, s, p.name))
      continue;
    if (p.name.empty())
      p.name = n;
    p.summary = summaryOf(s);
    out.push_back(p);
  }
  closedir(d);
  std::sort(out.begin(), out.end(), [](const Profile &a, const Profile &b) {
    return a.file > b.file;
  }); // newest first
  return out;
}

bool apply(const Profile &p, std::string &err) {
  Snapshot target;
  std::string name;
  if (!readSnapshot(p.file, target, name)) {
    err = "the profile could not be read";
    return false;
  }
  if (!load()) {
    err = g_loadError;
    return false;
  }
  for (auto &e : target) {
    int rc;
    if (e.first->type == 0) {
      rc = p_setInt(e.first->id, e.second.i);
    } else {
      std::vector<char> buf(e.first->size, 0);
      std::memcpy(buf.data(), e.second.s.data(),
                  std::min(e.second.s.size(), (size_t)e.first->size - 1));
      rc = p_setStr(e.first->id, buf.data(), e.first->size);
    }
    if (rc != 0) {
      char b[96];
      std::snprintf(b, sizeof(b), "writing %s failed (0x%08X)", e.first->path,
                    (uint32_t)rc);
      err = b;
      return false;
    }
  }
  return true;
}

bool remove(const Profile &p, std::string &err) {
  if (p.file.rfind(std::string(kDir) + "/", 0) != 0) {
    err = "invalid profile";
    return false;
  }
  if (unlink(p.file.c_str()) != 0) {
    err = "cannot delete the profile";
    return false;
  }
  return true;
}

} // namespace NetProfiles
