#include "gh_plugins.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <string>
#include <vector>

#include <orbis/Sysmodule.h>
#include <sqlite3.h>

#include "libc_compat.h"
#include "platform.h"

extern "C" {
// System HTTPS stack, declared with the real signatures (several OpenOrbis
// headers declare these without parameters).
int sceNetInit();
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool);
int sceSslInit(size_t poolSize);
int sceSslTerm(int sslCtx);
int sceHttpInit(int netPool, int sslCtx, size_t poolSize);
int sceHttpTerm(int httpCtx);
int sceHttpCreateTemplate(int httpCtx, const char *userAgent, int httpVer,
                          int autoProxy);
int sceHttpDeleteTemplate(int tmpl);
int sceHttpsSetSslCallback(int id,
                           int (*cb)(int, unsigned int, void *const *, int,
                                     void *),
                           void *arg);
int sceHttpSetResponseHeaderMaxSize(int id, uint32_t size);
int sceHttpCreateConnectionWithURL(int tmpl, const char *url, bool keepAlive);
int sceHttpDeleteConnection(int conn);
int sceHttpCreateRequestWithURL(int conn, int method, const char *url,
                                uint64_t contentLength);
int sceHttpDeleteRequest(int req);
int sceHttpSendRequest(int req, const void *data, size_t size);
int sceHttpGetStatusCode(int req, int *status);
int sceHttpGetAllResponseHeaders(int req, char **headers, size_t *size);
int sceHttpReadData(int req, void *data, uint32_t size);
int sceHttpGetLastErrno(int req, int *err);
// zlib inflate from stb_image (compiled in icon_cache.cpp).
int stbi_zlib_decode_noheader_buffer(char *obuffer, int olen,
                                     const char *ibuffer, int ilen);
}

namespace GhPlugins {
namespace {

struct Lock {
  pthread_mutex_t *m;
  explicit Lock(pthread_mutex_t *mm) : m(mm) { pthread_mutex_lock(m); }
  ~Lock() { pthread_mutex_unlock(m); }
};

const char *kReleaseApi = "https://api.github.com/repos/GoldHEN/"
                          "GoldHEN_Plugins_Repository/releases/latest";
const char *kPatchZip = "https://github.com/GoldHEN/GoldHEN_Patch_Repository/"
                        "raw/gh-pages/patch1.zip";
const char *kSharePrx = "/data/GoldHEN/plugins/no_share_watermark.prx";
const char *kPatchPrx = "/data/GoldHEN/plugins/game_patch.prx";
const char *kPatchXml = "/data/GoldHEN/patches/xml/";
const char *kPatchSet = "/data/GoldHEN/patches/settings/";
const char *kPatchVer = "/data/GoldHEN/patches/misc/patch_ver.txt";

bool exists(const std::string &path) {
  struct stat st;
  return Compat::stat(path.c_str(), &st) == 0;
}

bool readFile(const std::string &path, std::string &out,
              size_t limit = 8u << 20) {
  out.clear();
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    return false;
  char buf[8192];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && out.size() < limit)
    out.append(buf, n);
  std::fclose(f);
  return true;
}

void mkdirs(const std::string &path) {
  for (size_t i = 1; i <= path.size(); ++i)
    if (i == path.size() || path[i] == '/') {
      std::string d = path.substr(0, i);
      if (mkdir(d.c_str(), 0777) == 0)
        chmod(d.c_str(), 0777);
    }
}

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
    ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
    --b;
  return s.substr(a, b - a);
}

std::vector<std::string> splitLines(const std::string &s) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < s.size()) {
    size_t nl = s.find('\n', pos);
    if (nl == std::string::npos)
      nl = s.size();
    lines.push_back(s.substr(pos, nl - pos));
    pos = nl + 1;
  }
  return lines;
}

bool isSection(const std::string &t) {
  return t.size() >= 2 && t.front() == '[' && t.back() == ']';
}

int acceptVerifiedCert(int, unsigned int verifyErr, void *const *, int,
                       void *) {
  return verifyErr == 0 ? 1 : 0; // only certificates the system trusts
}

bool redirectLocation(int req, std::string &loc) {
  char *headers = nullptr;
  size_t size = 0;
  if (sceHttpGetAllResponseHeaders(req, &headers, &size) < 0 || !headers ||
      !size)
    return false;
  for (auto &line : splitLines(std::string(headers, size))) {
    std::string t = trim(line);
    if (t.size() <= 9)
      continue;
    std::string key = t.substr(0, 9);
    for (auto &c : key)
      c = (char)std::tolower((unsigned char)c);
    if (key == "location:") {
      loc = trim(t.substr(9));
      return loc.rfind("https://", 0) == 0; // HTTPS only
    }
  }
  return false;
}

uint32_t le16(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}
uint32_t le32(const unsigned char *p) { return le16(p) | (le16(p + 2) << 16); }

// GitHub release JSON: first asset URL ending in ".zip".
std::string firstZipAsset(const std::string &json) {
  const std::string key = "\"browser_download_url\"";
  for (size_t pos = json.find(key); pos != std::string::npos;
       pos = json.find(key, pos + 1)) {
    size_t q1 = json.find('"', json.find(':', pos + key.size()) + 1);
    size_t q2 = q1 == std::string::npos ? q1 : json.find('"', q1 + 1);
    if (q2 == std::string::npos)
      break;
    std::string url = json.substr(q1 + 1, q2 - q1 - 1);
    if (url.size() > 4 && url.compare(url.size() - 4, 4, ".zip") == 0 &&
        url.rfind("https://", 0) == 0)
      return url;
  }
  return "";
}

bool fetchPlugin(const std::string &fileName, std::string &prx,
                 std::string &err,
                 const std::function<void(const std::string &)> &progress) {
  progress("Looking up the latest GoldHEN plugins release...");
  std::string json;
  if (!httpsGet(kReleaseApi, json, 1u << 20, err))
    return false;
  std::string url = firstZipAsset(json);
  if (url.empty()) {
    err = "no plugin package in the latest release";
    return false;
  }
  progress("Downloading GoldHEN plugins...");
  std::string zip;
  if (!httpsGet(url, zip, 16u << 20, err))
    return false;
  std::map<std::string, std::string> files;
  const std::string want = "plugins/" + fileName;
  if (!unzip(zip, [&](const std::string &n) { return n == want; }, files, err))
    return false;
  auto it = files.find(want);
  if (it == files.end() || it->second.size() < 1024) {
    err = fileName + " not found in the release";
    return false;
  }
  prx = it->second;
  return true;
}

bool installPrx(const std::string &path, const std::string &data,
                std::string &err) {
  mkdirs(kPluginDir);
  return writeFileInPlace(path, data, 0777,
                          err); // 0777 as the original installer
}

// XML attribute value with the five standard entities decoded.
std::string xmlAttr(const std::string &tag, const char *name) {
  std::string key = std::string(name) + "=\"";
  size_t p = 0;
  for (;;) {
    p = tag.find(key, p);
    if (p == std::string::npos)
      return "";
    if (p == 0 || std::isspace((unsigned char)tag[p - 1]))
      break;
    ++p;
  }
  size_t a = p + key.size(), b = tag.find('"', a);
  if (b == std::string::npos)
    return "";
  std::string raw = tag.substr(a, b - a), out;
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '&') {
      static const struct {
        const char *e;
        char c;
      } ents[] = {{"&amp;", '&'},
                  {"&lt;", '<'},
                  {"&gt;", '>'},
                  {"&quot;", '"'},
                  {"&apos;", '\''}};
      bool done = false;
      for (auto &e : ents)
        if (raw.compare(i, std::strlen(e.e), e.e) == 0) {
          out += e.c;
          i += std::strlen(e.e) - 1;
          done = true;
          break;
        }
      if (done)
        continue;
    }
    out += raw[i];
  }
  return out;
}

// Same hash as game_patch.prx: djb2 over Title+Name+AppVer+<xml path>+AppElf,
// truncated to 255 bytes like its snprintf buffer; bytes are sign-extended
// (char -> u32) exactly as in the plugin.
uint64_t patchHash(const GamePatch &p, const std::string &xmlPath) {
  std::string s = p.title + p.name + p.appVer + xmlPath + p.appElf;
  if (s.size() > 255)
    s.resize(255);
  uint64_t h = 5381;
  for (char ch : s) {
    uint32_t c = (uint32_t)(int32_t)(signed char)ch;
    h = h * 33 ^ c;
  }
  return h;
}

std::string settingsPath(uint64_t hash) {
  char b[96];
  std::snprintf(b, sizeof(b), "%s0x%016llx.txt", kPatchSet,
                (unsigned long long)hash);
  return b;
}

} // namespace

bool httpsGet(const std::string &url, std::string &body, size_t maxBytes,
              std::string &err) {
  body.clear();
  int pool = -1, ssl = -1, http = -1, tmpl = -1, conn = -1, req = -1,
      status = 0;
  bool ok = false;
  std::string current = url;
  static char buf[16384];

  sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
  sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
  sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
  sceNetInit();

  pool = sceNetPoolCreate("ps4hub_https", 16 * 1024, 0);
  if (pool < 0) {
    err = "network init failed";
    goto done;
  }
  ssl = sceSslInit(512 * 1024);
  if (ssl < 0) {
    err = "SSL init failed";
    goto done;
  }
  http = sceHttpInit(pool, ssl, 512 * 1024);
  if (http < 0) {
    err = "HTTP init failed";
    goto done;
  }
  tmpl = sceHttpCreateTemplate(http, "PS4Hub", 2 /*HTTP/1.1*/, 1);
  if (tmpl < 0) {
    err = "HTTP template failed";
    goto done;
  }
  sceHttpsSetSslCallback(tmpl, acceptVerifiedCert, nullptr);
  sceHttpSetResponseHeaderMaxSize(
      tmpl, 64 * 1024); // GitHub redirects carry big headers

  for (int hop = 0;; ++hop) {
    conn = sceHttpCreateConnectionWithURL(tmpl, current.c_str(), true);
    if (conn < 0) {
      err = "connection failed";
      goto done;
    }
    req = sceHttpCreateRequestWithURL(conn, 0 /*GET*/, current.c_str(), 0);
    if (req < 0) {
      err = "request failed";
      goto done;
    }
    if (sceHttpSendRequest(req, nullptr, 0) < 0) {
      int e = 0;
      sceHttpGetLastErrno(req, &e);
      char b[64];
      std::snprintf(b, sizeof(b), "no connection to GitHub (0x%08X)",
                    (unsigned)e);
      err = b;
      goto done;
    }
    if (sceHttpGetStatusCode(req, &status) < 0) {
      err = "no HTTP status";
      goto done;
    }
    if (status == 200)
      break;
    bool redirect = status == 301 || status == 302 || status == 303 ||
                    status == 307 || status == 308;
    std::string next;
    if (!redirect || hop >= 6 || !redirectLocation(req, next)) {
      err = "GitHub answered HTTP " + std::to_string(status);
      goto done;
    }
    sceHttpDeleteRequest(req);
    req = -1;
    sceHttpDeleteConnection(conn);
    conn = -1;
    current = next;
  }
  for (;;) {
    int n = sceHttpReadData(req, buf, sizeof(buf));
    if (n == 0)
      break;
    if (n < 0) {
      err = "download interrupted";
      goto done;
    }
    body.append(buf, (size_t)n);
    if (body.size() > maxBytes) {
      err = "download too large";
      goto done;
    }
  }
  ok = !body.empty();
  if (!ok)
    err = "empty download";

done:
  if (req >= 0)
    sceHttpDeleteRequest(req);
  if (conn >= 0)
    sceHttpDeleteConnection(conn);
  if (tmpl >= 0)
    sceHttpDeleteTemplate(tmpl);
  if (http >= 0)
    sceHttpTerm(http);
  if (ssl >= 0)
    sceSslTerm(ssl);
  if (pool >= 0)
    sceNetPoolDestroy(pool);
  if (!ok)
    body.clear();
  return ok;
}

bool httpsGetToFile(const std::string &url, const std::string &path,
                    std::string &err,
                    const std::function<void(uint64_t bytes)> &onProgress) {
  int pool = -1, ssl = -1, http = -1, tmpl = -1, conn = -1, req = -1,
      status = 0;
  bool ok = false;
  std::string current = url;
  static char buf[32768];
  FILE *fp = nullptr;
  uint64_t written = 0;

  sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_NET);
  sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_HTTP);
  sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SSL);
  sceNetInit();

  pool = sceNetPoolCreate("ps4hub_dl", 16 * 1024, 0);
  if (pool < 0) {
    err = "network init failed";
    goto done;
  }
  ssl = sceSslInit(512 * 1024);
  if (ssl < 0) {
    err = "SSL init failed";
    goto done;
  }
  http = sceHttpInit(pool, ssl, 512 * 1024);
  if (http < 0) {
    err = "HTTP init failed";
    goto done;
  }
  tmpl = sceHttpCreateTemplate(http, "PS4Hub", 2, 1);
  if (tmpl < 0) {
    err = "HTTP template failed";
    goto done;
  }
  sceHttpsSetSslCallback(tmpl, acceptVerifiedCert, nullptr);
  sceHttpSetResponseHeaderMaxSize(tmpl, 64 * 1024);

  for (int hop = 0;; ++hop) {
    conn = sceHttpCreateConnectionWithURL(tmpl, current.c_str(), true);
    if (conn < 0) {
      err = "connection failed";
      goto done;
    }
    req = sceHttpCreateRequestWithURL(conn, 0, current.c_str(), 0);
    if (req < 0) {
      err = "request failed";
      goto done;
    }
    if (sceHttpSendRequest(req, nullptr, 0) < 0) {
      err = "no connection to server";
      goto done;
    }
    if (sceHttpGetStatusCode(req, &status) < 0) {
      err = "no HTTP status";
      goto done;
    }
    if (status == 200)
      break;
    bool redirect = status == 301 || status == 302 || status == 303 ||
                    status == 307 || status == 308;
    std::string next;
    if (!redirect || hop >= 6 || !redirectLocation(req, next)) {
      err = "server answered HTTP " + std::to_string(status);
      goto done;
    }
    sceHttpDeleteRequest(req);
    req = -1;
    sceHttpDeleteConnection(conn);
    conn = -1;
    current = next;
  }

  fp = std::fopen(path.c_str(), "wb");
  if (!fp) {
    err = "cannot create " + path;
    goto done;
  }
  for (;;) {
    int n = sceHttpReadData(req, buf, sizeof(buf));
    if (n == 0)
      break;
    if (n < 0) {
      err = "download interrupted";
      goto done;
    }
    if (std::fwrite(buf, 1, (size_t)n, fp) != (size_t)n) {
      err = "disk write failed (out of space?)";
      goto done;
    }
    written += (uint64_t)n;
    if (onProgress)
      onProgress(written);
  }
  ok = written > 0;
  if (!ok)
    err = "empty download";

done:
  if (fp)
    std::fclose(fp);
  if (req >= 0)
    sceHttpDeleteRequest(req);
  if (conn >= 0)
    sceHttpDeleteConnection(conn);
  if (tmpl >= 0)
    sceHttpDeleteTemplate(tmpl);
  if (http >= 0)
    sceHttpTerm(http);
  if (ssl >= 0)
    sceSslTerm(ssl);
  if (pool >= 0)
    sceNetPoolDestroy(pool);
  if (!ok)
    std::remove(path.c_str());
  return ok;
}

bool unzip(const std::string &zip,
           const std::function<bool(const std::string &)> &want,
           std::map<std::string, std::string> &out, std::string &err) {
  const unsigned char *z = (const unsigned char *)zip.data();
  const size_t n = zip.size();
  if (n < 22) {
    err = "archive too small";
    return false;
  }
  size_t eocd = std::string::npos;
  for (size_t i = n - 22 + 1; i-- > 0 && n - i <= 22 + 65535;)
    if (le32(z + i) == 0x06054b50) {
      eocd = i;
      break;
    }
  if (eocd == std::string::npos) {
    err = "not a zip archive";
    return false;
  }
  uint32_t count = le16(z + eocd + 10), cdOff = le32(z + eocd + 16);
  size_t p = cdOff;
  for (uint32_t i = 0; i < count; ++i) {
    if (p + 46 > n || le32(z + p) != 0x02014b50) {
      err = "damaged archive";
      return false;
    }
    uint32_t method = le16(z + p + 10), csize = le32(z + p + 20),
             usize = le32(z + p + 24);
    uint32_t nameLen = le16(z + p + 28), extraLen = le16(z + p + 30),
             commentLen = le16(z + p + 32);
    uint32_t local = le32(z + p + 42);
    if (p + 46 + nameLen > n) {
      err = "damaged archive";
      return false;
    }
    std::string name((const char *)z + p + 46, nameLen);
    p += 46 + nameLen + extraLen + commentLen;
    if (name.empty() || name.back() == '/' || !want(name))
      continue;
    if (name.find("..") != std::string::npos || name[0] == '/')
      continue; // never leave the target dir
    if (local + 30 > n || le32(z + local) != 0x04034b50) {
      err = "damaged archive";
      return false;
    }
    size_t data = local + 30 + le16(z + local + 26) + le16(z + local + 28);
    if (data + csize > n || usize > (64u << 20)) {
      err = "damaged archive";
      return false;
    }
    std::string bytes;
    if (method == 0) {
      bytes.assign((const char *)z + data, csize);
    } else if (method == 8) {
      bytes.resize(usize);
      int got = usize ? stbi_zlib_decode_noheader_buffer(&bytes[0], (int)usize,
                                                         (const char *)z + data,
                                                         (int)csize)
                      : 0;
      if (got != (int)usize) {
        err = "cannot unpack " + name;
        return false;
      }
    } else {
      err = "unsupported compression in " + name;
      return false;
    }
    out[name] = std::move(bytes);
  }
  return true;
}

bool writeFileAtomic(const std::string &path, const std::string &data,
                     std::string &err, int mode) {
  std::string temp = path + ".ps4hub.tmp";
  int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0) {
    err = "cannot write " + path + " (errno " + std::to_string(errno) + ")";
    return false;
  }
  fchmod(fd, (mode_t)mode); // umask would otherwise strip group / other bits
  bool ok = true;
  for (size_t w = 0; ok && w < data.size();) {
    ssize_t r = write(fd, data.data() + w, data.size() - w);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      ok = false;
    else
      w += (size_t)r;
  }
  ok = (fsync(fd) == 0) && ok;
  ok = (close(fd) == 0) && ok;
  if (!ok) {
    unlink(temp.c_str());
    err = "write failed: " + path;
    return false;
  }
  if (std::rename(temp.c_str(), path.c_str()) != 0) {
    unlink(temp.c_str());
    err = "cannot replace " + path + " (errno " + std::to_string(errno) + ")";
    return false;
  }
  return true;
}

bool writeFileInPlace(const std::string &path, const std::string &data,
                      int mode, std::string &err) {
  int fd = open(path.c_str(), O_WRONLY | O_CREAT, mode);
  if (fd < 0) {
    err = "cannot open " + path + " (errno " + std::to_string(errno) + ")";
    return false;
  }
  if (fchmod(fd, (mode_t)mode) != 0) {
    struct stat st;
    // Like the original: an EPERM is fine when the mode is already right.
    if (errno != EPERM || fstat(fd, &st) != 0 ||
        (int)(st.st_mode & 0777) != mode) {
      err = "cannot set permissions on " + path + " (errno " +
            std::to_string(errno) + ")";
      close(fd);
      return false;
    }
  }
  if (ftruncate(fd, 0) != 0) {
    err = "cannot truncate " + path;
    close(fd);
    return false;
  }
  bool ok = true;
  for (size_t w = 0; ok && w < data.size();) {
    ssize_t r = write(fd, data.data() + w, data.size() - w);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      ok = false;
    else
      w += (size_t)r;
  }
  ok = (fsync(fd) == 0) && ok;
  ok = (close(fd) == 0) && ok;
  if (!ok)
    err = "writing " + path + " failed";
  return ok;
}

bool iniHas(const std::string &section, const std::string &line) {
  std::string text;
  if (!readFile(kIniPath, text))
    return false;
  bool in = false;
  for (auto &l : splitLines(text)) {
    std::string t = trim(l);
    if (isSection(t)) {
      in = t == section;
      continue;
    }
    if (in && t == line)
      return true;
  }
  return false;
}

bool iniAdd(const std::string &section, const std::string &line,
            std::string &err) {
  if (iniHas(section, line))
    return true;
  mkdirs(kGoldhenDir);
  std::string text;
  readFile(kIniPath, text);
  // Exactly like the original installer: append "[section]\nline\n" at the
  // end of plugins.ini (0666), adding a newline first if the file lacks one.
  std::string addition;
  if (!text.empty() && text.back() != '\n')
    addition += "\n";
  addition += section + "\n" + line + "\n";
  int fd = open(kIniPath, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (fd < 0) {
    err = "plugins.ini open failed (errno " + std::to_string(errno) + ")";
    return false;
  }
  bool ok = true;
  for (size_t w = 0; ok && w < addition.size();) {
    ssize_t r = write(fd, addition.data() + w, addition.size() - w);
    if (r < 0 && errno == EINTR)
      continue;
    if (r <= 0)
      ok = false;
    else
      w += (size_t)r;
  }
  ok = (fsync(fd) == 0) && ok;
  ok = (close(fd) == 0) && ok;
  if (!ok) {
    err = "plugins.ini append failed";
    return false;
  }
  if (!iniHas(section, line)) {
    err = "plugins.ini entry could not be verified";
    return false;
  }
  return true;
}

bool iniRemove(const std::string &section, const std::string &line,
               std::string &err) {
  std::string text;
  if (!readFile(kIniPath, text))
    return true;
  std::vector<std::string> lines = splitLines(text), out;
  bool changed = false;
  for (size_t i = 0; i < lines.size(); ++i) {
    if (trim(lines[i]) != section) {
      out.push_back(lines[i]);
      continue;
    }
    // Our section: drop our line; drop the header too if nothing is left.
    size_t j = i + 1;
    std::vector<std::string> body;
    bool others = false;
    for (; j < lines.size() && !isSection(trim(lines[j])); ++j) {
      std::string b = trim(lines[j]);
      if (b == line) {
        changed = true;
        continue;
      }
      if (!b.empty() && b[0] != ';' && b[0] != '#')
        others = true;
      body.push_back(lines[j]);
    }
    if (others || section == "[default]") { // keep [default] even when empty
      out.push_back(lines[i]);
      out.insert(out.end(), body.begin(), body.end());
    } else {
      changed = true;
    }
    i = j - 1;
  }
  if (!changed)
    return true;
  std::string ignore;
  writeFileAtomic(std::string(kIniPath) + ".ps4hub.bak", text, ignore);
  std::string result;
  for (auto &l : out)
    result += l + "\n";
  struct stat st;
  int mode = Compat::stat(kIniPath, &st) == 0 ? (int)(st.st_mode & 0777) : 0666;
  return writeFileAtomic(kIniPath, result, err, mode ? mode : 0666);
}

Jobs &Jobs::instance() {
  static Jobs j;
  return j;
}

bool Jobs::start(
    const std::string &name,
    std::function<bool(const std::function<void(const std::string &)> &,
                       std::string &)>
        fn) {
  if (state_ == State::Running)
    return false;
  if (threadStarted_) {
    pthread_join(thread_, nullptr);
    threadStarted_ = false;
  }
  fn_ = std::move(fn);
  {
    Lock l(&mutex_);
    name_ = name;
    message_ = "Starting...";
  }
  state_ = State::Running;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 512 * 1024);
  threadStarted_ = pthread_create(&thread_, &attr, &Jobs::entry, this) == 0;
  pthread_attr_destroy(&attr);
  if (!threadStarted_) {
    {
      Lock l(&mutex_);
      message_ = "Could not start";
    }
    state_ = State::Failed;
    ++serial_;
    return false;
  }
  return true;
}

std::string Jobs::message() {
  Lock l(&mutex_);
  return message_;
}
std::string Jobs::name() {
  Lock l(&mutex_);
  return name_;
}

void *Jobs::entry(void *self) {
  Jobs *j = static_cast<Jobs *>(self);
  std::string result;
  bool ok = false;
  try {
    ok = j->fn_(
        [j](const std::string &m) {
          Lock l(&j->mutex_);
          j->message_ = m;
        },
        result);
  } catch (const std::exception &e) {
    result = std::string("Exception: ") + e.what();
    ok = false;
  } catch (...) {
    result = "Unknown exception";
    ok = false;
  }
  {
    Lock l(&j->mutex_);
    j->message_ = result;
  }
  j->state_ = ok ? State::Done : State::Failed;
  ++j->serial_;
  return nullptr;
}

bool shareEnabled() {
  return exists(kSharePrx) && iniHas("[default]", kSharePrx);
}

bool startShareInstall() {
  return Jobs::instance().start(
      "No Share Watermark & Blocks",
      [](const auto &progress, std::string &result) {
        std::string prx, err;
        if (!fetchPlugin("no_share_watermark.prx", prx, err, progress)) {
          result = "Download failed: " + err;
          return false;
        }
        progress("Installing the plugin...");
        if (!installPrx(kSharePrx, prx, err)) {
          result = err;
          return false;
        }
        if (!iniAdd("[default]", kSharePrx, err)) {
          result = "plugins.ini: " + err;
          return false;
        }
        result = "No Share Watermark & Blocks enabled for all games";
        return true;
      });
}

bool disableShare(std::string &err) {
  if (!iniRemove("[default]", kSharePrx, err))
    return false;
  unlink(kSharePrx);
  return true;
}

bool patchSupportInstalled() {
  return exists(kPatchPrx) && iniHas("[default]", kPatchPrx);
}

std::string patchDatabaseVersion() {
  std::string t;
  if (!readFile(kPatchVer, t, 4096))
    return "";
  t = trim(splitLines(t).empty() ? std::string() : splitLines(t)[0]);
  size_t c = t.find(':');
  if (c != std::string::npos)
    t = trim(t.substr(c + 1));
  return t.substr(0, 10); // date part "YYYY-MM-DD"
}

bool startPatchSupportInstall() {
  return Jobs::instance().start("Game patches", [](const auto &progress,
                                                   std::string &result) {
    std::string prx, err;
    if (!fetchPlugin("game_patch.prx", prx, err, progress)) {
      result = "Download failed: " + err;
      return false;
    }
    progress("Downloading the patch database...");
    std::string zip;
    if (!httpsGet(kPatchZip, zip, 32u << 20, err)) {
      result = "Patch database: " + err;
      return false;
    }
    std::map<std::string, std::string> files;
    if (!unzip(
            zip,
            [](const std::string &n) {
              return n.rfind("patches/xml/", 0) == 0 ||
                     n.rfind("patches/misc/", 0) == 0;
            },
            files, err)) {
      result = "Patch database: " + err;
      return false;
    }
    if (files.empty()) {
      result = "Patch database is empty";
      return false;
    }
    progress("Installing " + std::to_string(files.size()) + " patch files...");
    mkdirs("/data/GoldHEN/patches/xml");
    mkdirs("/data/GoldHEN/patches/misc");
    mkdirs("/data/GoldHEN/patches/settings");
    for (auto &kv : files)
      if (!writeFileInPlace(std::string(kGoldhenDir) + "/" + kv.first,
                            kv.second, 0666, err)) {
        result = err;
        return false;
      }
    if (!installPrx(kPatchPrx, prx, err)) {
      result = err;
      return false;
    }
    if (!iniAdd("[default]", kPatchPrx, err)) {
      result = "plugins.ini: " + err;
      return false;
    }
    result = "Game patches ready (" + std::to_string(files.size()) +
             " files). Turn patches on per game.";
    return true;
  });
}

bool hasPatches(const std::string &titleId) {
  return exists(kPatchXml + titleId + ".xml");
}

std::vector<GamePatch> patchesFor(const std::string &titleId) {
  std::vector<GamePatch> out;
  const std::string xmlPath = kPatchXml + titleId + ".xml";
  std::string xml;
  if (!readFile(xmlPath, xml, 4u << 20))
    return out;
  for (size_t p = xml.find("<Metadata"); p != std::string::npos;
       p = xml.find("<Metadata", p + 1)) {
    size_t e = xml.find('>', p);
    if (e == std::string::npos)
      break;
    std::string tag = xml.substr(p, e - p);
    GamePatch g;
    g.title = xmlAttr(tag, "Title");
    g.name = xmlAttr(tag, "Name");
    g.note = xmlAttr(tag, "Note");
    g.author = xmlAttr(tag, "Author");
    g.appVer = xmlAttr(tag, "AppVer");
    g.appElf = xmlAttr(tag, "AppElf");
    if (g.name.empty())
      continue;
    g.hash = patchHash(g, xmlPath);
    std::string s;
    g.enabled =
        readFile(settingsPath(g.hash), s, 16) && !s.empty() && s[0] == '1';
    out.push_back(g);
  }
  return out;
}

bool setPatchEnabled(const GamePatch &p, bool on, std::string &err) {
  mkdirs("/data/GoldHEN/patches/settings");
  return writeFileInPlace(settingsPath(p.hash), on ? "1\n" : "0\n", 0777,
                          err); // game_patch chmods them 0777 too
}

} // namespace GhPlugins

namespace GhPlugins {
// Files written by PS4Hub before 1.25 were created with the default mode
// (0644 after umask); GoldHEN needs the plugins at 0777. Fix them in place.
void repairPermissions() {
  static const char *kPrx[] = {"/data/GoldHEN/plugins/yt_adblock.prx",
                               "/data/GoldHEN/plugins/no_share_watermark.prx",
                               "/data/GoldHEN/plugins/game_patch.prx"};
  for (const char *p : kPrx)
    chmod(p, 0777);
  chmod("/data/GoldHEN/plugins", 0777);
  chmod(kIniPath, 0666);
  chmod("/data/youtube/config.json", 0666);
}
} // namespace GhPlugins

namespace GhPlugins {
namespace {
// Deletes regular files directly inside `dir` (no recursion), then the dir.
void clearDir(const std::string &dir) {
  if (DIR *d = opendir(dir.c_str())) {
    while (struct dirent *e = readdir(d)) {
      std::string n = e->d_name;
      if (n == "." || n == ".." || n.find('/') != std::string::npos)
        continue;
      unlink((dir + "/" + n).c_str());
    }
    closedir(d);
  }
  rmdir(dir.c_str());
}
} // namespace

bool removePatchSupport(std::string &err) {
  if (!iniRemove("[default]", "/data/GoldHEN/plugins/game_patch.prx", err))
    return false;
  unlink("/data/GoldHEN/plugins/game_patch.prx");
  clearDir("/data/GoldHEN/patches/xml");
  clearDir("/data/GoldHEN/patches/misc");
  clearDir("/data/GoldHEN/patches/settings");
  rmdir("/data/GoldHEN/patches"); // only if nothing else is left
  return true;
}

// opendir() does not work on /app0 (PFS read-only mount), so we use a
// compile-time manifest of every file inside assets/autoloader/.

namespace {
const char *kAutoloaderSrc = "/app0/assets/autoloader/";
const char *kAutoloaderDst = "/user/data/ps4hub/autoloader";
const char *kAutoloaderMarker = "/user/data/ps4hub/autoloader/.installed";

static const char *kAutoloaderFiles[] = {"505.manifest",
                                         "505/exploit.js",
                                         "700.manifest",
                                         "700/alert.js",
                                         "700/config.js",
                                         "700/kpatch/700.elf",
                                         "700/kpatch/750.elf",
                                         "700/kpatch/800.elf",
                                         "700/kpatch/850.elf",
                                         "700/lapse.js",
                                         "700/lapse/700.js",
                                         "700/lapse/750.js",
                                         "700/lapse/751.js",
                                         "700/lapse/800.js",
                                         "700/lapse/850.js",
                                         "700/lapse/852.js",
                                         "700/module/chain.js",
                                         "700/module/int64.js",
                                         "700/module/mem.js",
                                         "700/module/memtools.js",
                                         "700/module/offset.js",
                                         "700/module/rw.js",
                                         "700/module/utils.js",
                                         "700/module/view.js",
                                         "700/psfree.js",
                                         "700/rop/700.js",
                                         "700/rop/750.js",
                                         "700/rop/800.js",
                                         "700/rop/850.js",
                                         "700/send.js",
                                         "900.manifest",
                                         "900/alert.js",
                                         "900/config.js",
                                         "900/kpatch/900.elf",
                                         "900/kpatch/903.elf",
                                         "900/kpatch/950.elf",
                                         "900/lapse.js",
                                         "900/lapse/900.js",
                                         "900/lapse/903.js",
                                         "900/lapse/950.js",
                                         "900/module/chain.js",
                                         "900/module/constants.js",
                                         "900/module/int64.js",
                                         "900/module/mem.js",
                                         "900/module/memtools.js",
                                         "900/module/offset.js",
                                         "900/module/rw.js",
                                         "900/module/utils.js",
                                         "900/module/view.js",
                                         "900/psfree.js",
                                         "900/rop/900.js",
                                         "900/rop/950.js",
                                         "900/send.js",
                                         "cache505.html",
                                         "cache700.html",
                                         "cache900.html",
                                         "cachecss.html",
                                         "cacheslopkit.html",
                                         "css.manifest",
                                         "css/lapse.js",
                                         "css/loader.js",
                                         "css/main.js",
                                         "css/misc.js",
                                         "css/ps4/constants.js",
                                         "css/ps4/kernel.js",
                                         "css/ps4/patches/1000.bin",
                                         "css/ps4/patches/1050.bin",
                                         "css/ps4/patches/1100.bin",
                                         "css/ps4/patches/1102.bin",
                                         "css/ps4/userland.js",
                                         "css/worker.js",
                                         "css/workers.js",
                                         "goldhen_2.4b18.14.bin",
                                         "img.jpg",
                                         "index.html",
                                         "slopkit.manifest",
                                         "slopkit/afterglow.js",
                                         "slopkit/chain_lapse.js",
                                         "slopkit/core.js",
                                         "slopkit/int64.js",
                                         "slopkit/jb.js",
                                         "slopkit/mem.js",
                                         "slopkit/patches/1150.bin",
                                         "slopkit/patches/1200.bin",
                                         "slopkit/patches/1202.bin",
                                         "slopkit/patches/1250.bin",
                                         "slopkit/patches/1252.bin",
                                         "slopkit/patches/1300.bin",
                                         "slopkit/patches/1302.bin",
                                         "slopkit/patches/1304.bin",
                                         "slopkit/patches/1350.bin",
                                         "slopkit/patches/1352.bin",
                                         "slopkit/patches/1400.bin",
                                         "slopkit/patches/1402.bin",
                                         "slopkit/ps4_offsets.js",
                                         "slopkit/rpc_worker.js",
                                         "style.css",
                                         nullptr};

// Copy a single file by full path using FILE* (fopen/fread/fwrite).
// Preserves errno; distinguishes src-open vs dst-open failures.
// Returns 0 on success, 1 on fopen(src) failure, 2 on fopen(dst) failure, 3 on
// read/write failure.
static int copyFileDetailed(const char *src, const char *dst, int &savedErrno) {
  FILE *in = std::fopen(src, "rb");
  if (!in) {
    savedErrno = errno;
    return 1;
  }
  FILE *out = std::fopen(dst, "wb");
  if (!out) {
    savedErrno = errno;
    std::fclose(in);
    return 2;
  }
  char buf[32768];
  size_t nr;
  bool ok = true;
  int rwErr = 0;
  while ((nr = std::fread(buf, 1, sizeof(buf), in)) > 0) {
    if (std::fwrite(buf, 1, nr, out) != nr) {
      rwErr = errno;
      ok = false;
      break;
    }
  }
  if (ok && std::ferror(in)) {
    rwErr = errno;
    ok = false;
  }
  std::fflush(out);
  std::fclose(out);
  std::fclose(in);
  if (!ok) {
    savedErrno = rwErr;
    return 3;
  }
  savedErrno = 0;
  return 0;
}

static bool copyFile(const char *src, const char *dst) {
  int e = 0;
  return copyFileDetailed(src, dst, e) == 0;
}

static void ensureParentDirs(const std::string &base, const std::string &rel) {
  std::string cur = base;
  size_t pos = 0;
  while ((pos = rel.find('/', pos)) != std::string::npos) {
    cur += "/" + rel.substr(0, pos);
    mkdir(cur.c_str(), 0777);
    cur = base;
    ++pos;
  }
  cur = base;
  for (size_t i = 0; i < rel.size(); ++i) {
    if (rel[i] == '/') {
      mkdir(cur.c_str(), 0777);
    }
    cur += rel[i];
  }
}

static void removeTree(const std::string &dir) {
  DIR *d = opendir(dir.c_str());
  if (!d) {
    unlink(dir.c_str());
    return;
  }
  while (struct dirent *e = readdir(d)) {
    std::string n = e->d_name;
    if (n == "." || n == "..")
      continue;
    std::string p = dir + "/" + n;
    struct stat st;
    if (Compat::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
      removeTree(p);
    else
      unlink(p.c_str());
  }
  closedir(d);
  rmdir(dir.c_str());
}
} // namespace

bool autoloaderInstalled() { return exists(kAutoloaderMarker); }

bool startAutoloaderInstall() {
  return Jobs::instance().start(
      "Autoloader", [](const auto &progress, std::string &result) {
        progress("Installing autoloader files...");

        mkdirs(kAutoloaderDst);

        int copied = 0, failed = 0;
        std::string firstErr;
        int firstErrStage = 0;
        for (int i = 0; kAutoloaderFiles[i]; ++i) {
          const char *rel = kAutoloaderFiles[i];
          std::string src = std::string(kAutoloaderSrc) + rel;
          std::string dst = std::string(kAutoloaderDst) + "/" + rel;

          std::string parent = dst.substr(0, dst.rfind('/'));
          mkdirs(parent);

          int errnoCopy = 0;
          int rc = copyFileDetailed(src.c_str(), dst.c_str(), errnoCopy);
          if (rc == 0) {
            ++copied;
            if (copied % 10 == 0)
              progress("Copying files... (" + std::to_string(copied) + "/" +
                       std::to_string(i + 1) + ")");
          } else {
            ++failed;
            if (firstErr.empty()) {
              const char *stage = rc == 1   ? "open-src"
                                  : rc == 2 ? "open-dst"
                                            : "rw";
              firstErr = std::string(stage) + " " + rel +
                         " errno=" + std::to_string(errnoCopy);
              firstErrStage = rc;
            }
          }
        }

        if (copied == 0) {
          result = "Install failed: " + firstErr;
          return false;
        }

        int fd = open(kAutoloaderMarker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
          write(fd, "1\n", 2);
          close(fd);
        }

        result =
            "Autoloader installed (" + std::to_string(copied) + " files copied";
        if (failed > 0)
          result += ", " + std::to_string(failed) + " failed: " + firstErr;
        result += ")";
        return true;
      });
}

static void listDir(const char *path, std::string &out) {
  DIR *d = opendir(path);
  if (!d) {
    out += std::string(path) + "=errno" + std::to_string(errno) + "; ";
    return;
  }
  out += std::string(path) + "=[";
  int n = 0;
  while (struct dirent *e = readdir(d)) {
    if (std::string(e->d_name) == "." || std::string(e->d_name) == "..")
      continue;
    if (n++)
      out += ",";
    if (n > 20) {
      out += "...";
      break;
    }
    out += e->d_name;
  }
  out += "]; ";
  closedir(d);
}

// Try multiple source prefixes and pick the one that works.  PS4 may mount
// the PFS at /app0, /App0, or /mnt/sandbox/<pfs>/app0 depending on firmware.
static std::string detectAppRoot(std::string &diag) {
  std::string plRoot = Platform::appRoot();
  if (!plRoot.empty()) {
    diag += "Platform::appRoot=" + plRoot + "; ";
    return plRoot;
  }
  static const char *candidates[] = {".",
                                     "app0",
                                     "/app0",
                                     "/App0",
                                     "/APP0",
                                     "/mnt/sandbox/pfsmnt/CPAN00001-app0",
                                     "/mnt/sandbox/CPAN00001_000/app0",
                                     "/mnt/sandbox/CPAN00001/app0",
                                     "/mnt/pfs/CPAN00001/app0",
                                     nullptr};
  char cwd[512];
  cwd[0] = 0;
  if (getcwd(cwd, sizeof(cwd)))
    diag += std::string("CWD=") + cwd + "; ";
  else
    diag += "CWD=errno" + std::to_string(errno) + "; ";

  listDir("/", diag);
  listDir("/mnt", diag);
  listDir("/mnt/sandbox", diag);

  for (int i = 0; candidates[i]; ++i) {
    std::string probe = std::string(candidates[i]) + "/eboot.bin";
    FILE *f = std::fopen(probe.c_str(), "rb");
    if (f) {
      std::fclose(f);
      diag += std::string("FOUND=") + candidates[i] + "; ";
      return candidates[i];
    }
  }
  diag += "NO-ROOT-FOUND; ";
  return ".";
}

// Synchronous version: reads /app0 from the calling (main) thread.  This is
// required on PS4 because /app0 (the inner PFS mount) is not visible from
// worker threads under some jailbreaks.
bool installAutoloaderSync(std::string &result) {
  std::string diag;
  std::string appRoot = detectAppRoot(diag);

  if (appRoot.empty()) {
    result = "Install failed: /app0 not found";
    return false;
  }

  std::string srcBase = appRoot + "/assets/autoloader/";
  mkdirs(kAutoloaderDst);
  int copied = 0, failed = 0;
  std::string firstErr;
  int firstErrStage = 0;
  for (int i = 0; kAutoloaderFiles[i]; ++i) {
    const char *rel = kAutoloaderFiles[i];
    std::string src = srcBase + rel;
    std::string dst = std::string(kAutoloaderDst) + "/" + rel;
    std::string parent = dst.substr(0, dst.rfind('/'));
    mkdirs(parent);
    int errnoCopy = 0;
    int rc = copyFileDetailed(src.c_str(), dst.c_str(), errnoCopy);
    if (rc != 0) {
      if (Platform::copyAppFile(std::string("assets/autoloader/") + rel, dst))
        rc = 0;
    }
    if (rc == 0)
      ++copied;
    else {
      ++failed;
      if (firstErr.empty()) {
        const char *stage = rc == 1 ? "open-src" : rc == 2 ? "open-dst" : "rw";
        firstErr = std::string(stage) + " " + rel +
                   " errno=" + std::to_string(errnoCopy);
        firstErrStage = rc;
      }
    }
  }
  if (copied == 0) {
    result = "Install failed (stage=" + std::to_string(firstErrStage) +
             "): " + firstErr + " | root=" + appRoot;
    return false;
  }
  int fd = open(kAutoloaderMarker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) {
    write(fd, "1\n", 2);
    close(fd);
  }
  result = "Autoloader installed (" + std::to_string(copied) + " files from " +
           appRoot;
  if (failed > 0)
    result += ", " + std::to_string(failed) + " failed: " + firstErr;
  result += ")";
  return failed == 0;
}

bool removeAutoloader(std::string &err) {
  (void)err;
  removeTree(kAutoloaderDst);
  return true;
}

std::string cleanTemporaryFiles() {
  int removed = 0;
  long long freed = 0;

  auto lower = [](std::string s) {
    for (auto &c : s)
      c = std::tolower(static_cast<unsigned char>(c));
    return s;
  };
  auto endsWith = [](const std::string &s, const char *suf) {
    size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
  };
  auto startsWith = [](const std::string &s, const char *pre) {
    size_t n = std::strlen(pre);
    return s.size() >= n && s.compare(0, n, pre) == 0;
  };

  auto tryUnlinkFile = [&](const std::string &path) {
    struct stat st;
    if (Compat::stat(path.c_str(), &st) != 0)
      return;
    if (!S_ISREG(st.st_mode))
      return;
    if (unlink(path.c_str()) == 0) {
      ++removed;
      freed += (long long)st.st_size;
    }
  };

  // Does a filename look like a disposable temporary?  We stay conservative:
  // only match patterns that are UNAMBIGUOUSLY scratch output.
  auto isTemporary = [&](const std::string &name) {
    std::string nm = lower(name);
    // Protected names we never touch.
    static const char *kNeverTouch[] = {
        "config",   "settings", "save", "cookie",   "cache",   "profile",
        "license",  "trophy",   "user", "bookmark", "history", "manifest",
        "database", "sql",      "wal",  "shm",      nullptr};
    for (int i = 0; kNeverTouch[i]; ++i)
      if (nm.find(kNeverTouch[i]) != std::string::npos)
        return false;

    if (startsWith(nm, "tmp_") || startsWith(nm, "temp_") ||
        startsWith(nm, "~") || startsWith(nm, ".tmp"))
      return true;
    if (endsWith(nm, ".tmp") || endsWith(nm, ".bak") || endsWith(nm, ".old") ||
        endsWith(nm, ".log") || endsWith(nm, ".crash") ||
        endsWith(nm, ".dmp") || endsWith(nm, ".swp") || endsWith(nm, ".swo"))
      return true;
    // Crash / scratch suffixes used by common homebrew.
    if (nm.find("crash") != std::string::npos &&
        (endsWith(nm, ".txt") || endsWith(nm, ".bin")))
      return true;
    if (nm.find("scratch") != std::string::npos)
      return true;
    return false;
  };

  auto walk = [&](const char *dir) {
    DIR *d = opendir(dir);
    if (!d)
      return;
    while (struct dirent *e = readdir(d)) {
      std::string nm = e->d_name;
      if (nm == "." || nm == "..")
        continue;
      if (isTemporary(nm))
        tryUnlinkFile(std::string(dir) + "/" + nm);
    }
    closedir(d);
  };

  // Recursive walker.  Respects protected path patterns.
  auto isProtectedPath = [&](const std::string &p) {
    std::string lp = lower(p);
    static const char *kProtected[] = {
        "/system_data/priv/mms", // app.db directory
        "/system_data/priv/mms/app.db",
        "/webbrowser",
        "/webcore",
        "/web_browser",
        "/savedata",
        "/sshared",
        "/trophy",
        "/license",
        "/cookie",
        "/cookies",
        "/cache",
        "/httpcache",
        "/addcont",
        "/patch",
        "/sce_sys",
        "/sce_module",
        nullptr};
    for (int i = 0; kProtected[i]; ++i)
      if (lp.find(kProtected[i]) != std::string::npos)
        return true;
    return false;
  };

  std::function<void(const std::string &, int)> walkRec =
      [&](const std::string &dir, int depth) {
        if (depth > 6)
          return;
        if (isProtectedPath(dir))
          return;
        DIR *d = opendir(dir.c_str());
        if (!d)
          return;
        while (struct dirent *e = readdir(d)) {
          std::string nm = e->d_name;
          if (nm == "." || nm == "..")
            continue;
          std::string full = dir + "/" + nm;
          if (isProtectedPath(full))
            continue;
          struct stat st;
          if (Compat::stat(full.c_str(), &st) != 0)
            continue;
          if (S_ISDIR(st.st_mode)) {
            walkRec(full, depth + 1);
          } else if (S_ISREG(st.st_mode)) {
            if (isTemporary(nm))
              tryUnlinkFile(full);
          }
        }
        closedir(d);
      };

  tryUnlinkFile("/user/data/ps4hub/autoloader_debug.txt");
  tryUnlinkFile("/user/data/ps4hub/bookmark_scan.txt");

  // 2. System-wide scratch roots.  Only descend into writable user areas
  //    and skip anything that touches a protected path.
  static const char *kRoots[] = {"/user/data",    // shared homebrew scratch
                                 "/data",         // GoldHEN, hb loader, etc.
                                 "/user/app_tmp", // app temporaries
                                 "/app_tmp",      "/user/system_tmp",
                                 "/system_tmp",   nullptr};
  for (int i = 0; kRoots[i]; ++i)
    walkRec(kRoots[i], 0);

  return "Cleaned " + std::to_string(removed) + " file(s), freed " +
         std::to_string(freed / 1024) + " KB";
}

std::string scanBookmarkLocations() {
  std::string out;
  auto note = [&](const std::string &s) { out += s + "\n"; };

  auto probePath = [&](const char *path) {
    struct stat st;
    if (Compat::stat(path, &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        note(std::string(path) +
             " = DIR (mode=" + std::to_string(st.st_mode & 0777) + ")");
        DIR *d = opendir(path);
        if (d) {
          int n = 0;
          while (struct dirent *e = readdir(d)) {
            std::string nm = e->d_name;
            if (nm == "." || nm == "..")
              continue;
            if (n++ >= 30) {
              note("  ...");
              break;
            }
            note("  - " + nm);
          }
          closedir(d);
        } else {
          note("  (opendir errno=" + std::to_string(errno) + ")");
        }
      } else {
        note(std::string(path) +
             " = FILE (size=" + std::to_string((long long)st.st_size) + ")");
      }
    } else {
      note(std::string(path) + " = errno" + std::to_string(errno));
    }
  };

  note("=== PS4 bookmark location scan ===");

  // Per-user web browser data
  probePath("/user/home");
  {
    DIR *d = opendir("/user/home");
    if (d) {
      while (struct dirent *e = readdir(d)) {
        std::string nm = e->d_name;
        if (nm == "." || nm == "..")
          continue;
        std::string base = std::string("/user/home/") + nm;
        probePath(base.c_str());
        // Common sub-paths used by PS4 webapps.
        for (const char *sub :
             {"web_browser", "webbrowser", "browser", "webcore", "WebCore",
              "SceWebBrowser", "nvs", "sce_sys", "savedata", "NPXS20102",
              "NPXS22010"}) {
          std::string p = base + "/" + sub;
          probePath(p.c_str());
        }
      }
      closedir(d);
    }
  }

  probePath("/user/app");
  probePath("/user/app/NPXS20102");
  probePath("/user/savedata");
  probePath("/user/priv");
  probePath("/user/system");
  probePath("/user/common");
  probePath("/user/sshared");

  probePath("/system_data");
  probePath("/system_data/priv");
  probePath("/system_data/priv/mms");
  probePath("/system_data/priv/webbrowser");
  probePath("/system_data/priv/WebBrowser");
  probePath("/system_data/priv/webcore");
  probePath("/system_data/priv/WebCore");
  probePath("/system_data/priv/web_browser");
  probePath("/system_data/pub");
  probePath("/system_data/priv/appmeta/NPXS20102");

  // Mounts and sandbox directories
  probePath("/mnt/sandbox");
  probePath("/mnt/sandbox/pfsmnt");

  const char *dbCandidates[] = {"/system_data/priv/mms/app.db",
                                "/system_data/priv/mms/bookmark.db",
                                "/system_data/priv/mms/favorite.db",
                                "/system_data/priv/webbrowser/bookmark.db",
                                "/system_data/priv/webcore/bookmark.db",
                                "/user/data/webbrowser/bookmark.db",
                                "/system_data/priv/webcore/webbrowser.db",
                                nullptr};
  note("");
  note("=== Known DB candidates ===");
  for (int i = 0; dbCandidates[i]; ++i)
    probePath(dbCandidates[i]);

  // List app.db tables
  note("");
  note("=== app.db tables ===");
  sqlite3 *db = nullptr;
  if (sqlite3_open_v2("/system_data/priv/mms/app.db", &db, SQLITE_OPEN_READONLY,
                      nullptr) == SQLITE_OK) {
    sqlite3_stmt *st = nullptr;
    if (sqlite3_prepare_v2(
            db,
            "SELECT name FROM sqlite_master WHERE type='table' ORDER BY name;",
            -1, &st, nullptr) == SQLITE_OK) {
      while (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *n = sqlite3_column_text(st, 0);
        if (n) {
          std::string nm = reinterpret_cast<const char *>(n);
          std::string low = nm;
          for (auto &c : low)
            c = std::tolower(c);
          note(" - " + nm +
               ((low.find("book") != std::string::npos ||
                 low.find("fav") != std::string::npos ||
                 low.find("hist") != std::string::npos ||
                 low.find("url") != std::string::npos ||
                 low.find("web") != std::string::npos ||
                 low.find("brow") != std::string::npos)
                    ? "  <-- interesting"
                    : ""));
        }
      }
      sqlite3_finalize(st);
    }
    sqlite3_close(db);
  } else {
    note(" (cannot open app.db)");
  }

  // 6. Deep inspection: locate webbrowser_bookmark.db and dump its schema +
  // rows.
  note("");
  note("=== Dynamic bookmark DB inspection ===");
  DIR *dh = opendir("/user/home");
  if (dh) {
    while (struct dirent *e = readdir(dh)) {
      std::string nm = e->d_name;
      if (nm == "." || nm == "..")
        continue;
      std::string bookDb = std::string("/user/home/") + nm +
                           "/webbrowser/webbrowser_bookmark.db";
      struct stat st;
      if (Compat::stat(bookDb.c_str(), &st) != 0)
        continue;
      note("");
      note("DB: " + bookDb + " (size=" + std::to_string((long long)st.st_size) +
           ")");

      sqlite3 *bdb = nullptr;
      if (sqlite3_open_v2(bookDb.c_str(), &bdb, SQLITE_OPEN_READONLY,
                          nullptr) != SQLITE_OK) {
        note("  cannot open: " +
             std::string(bdb ? sqlite3_errmsg(bdb) : "null"));
        if (bdb)
          sqlite3_close(bdb);
        continue;
      }

      sqlite3_stmt *tstmt = nullptr;
      std::vector<std::string> tables;
      if (sqlite3_prepare_v2(bdb,
                             "SELECT name FROM sqlite_master WHERE "
                             "type='table' ORDER BY name;",
                             -1, &tstmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(tstmt) == SQLITE_ROW) {
          const unsigned char *n = sqlite3_column_text(tstmt, 0);
          if (n)
            tables.emplace_back(reinterpret_cast<const char *>(n));
        }
        sqlite3_finalize(tstmt);
      }

      for (const auto &tb : tables) {
        note("  TABLE: " + tb);
        std::string pragma = "PRAGMA table_info(\"" + tb + "\");";
        sqlite3_stmt *ps = nullptr;
        if (sqlite3_prepare_v2(bdb, pragma.c_str(), -1, &ps, nullptr) ==
            SQLITE_OK) {
          while (sqlite3_step(ps) == SQLITE_ROW) {
            const unsigned char *colName = sqlite3_column_text(ps, 1);
            const unsigned char *colType = sqlite3_column_text(ps, 2);
            note(std::string("    col: ") +
                 (colName ? reinterpret_cast<const char *>(colName) : "?") +
                 " " +
                 (colType ? reinterpret_cast<const char *>(colType) : ""));
          }
          sqlite3_finalize(ps);
        }
        std::string cq = "SELECT COUNT(*) FROM \"" + tb + "\";";
        sqlite3_stmt *cs = nullptr;
        if (sqlite3_prepare_v2(bdb, cq.c_str(), -1, &cs, nullptr) ==
                SQLITE_OK &&
            sqlite3_step(cs) == SQLITE_ROW) {
          note("    rows: " + std::to_string(sqlite3_column_int(cs, 0)));
        }
        if (cs)
          sqlite3_finalize(cs);

        std::string sq = "SELECT * FROM \"" + tb + "\" LIMIT 5;";
        sqlite3_stmt *ss = nullptr;
        if (sqlite3_prepare_v2(bdb, sq.c_str(), -1, &ss, nullptr) ==
            SQLITE_OK) {
          int rowNum = 0;
          while (sqlite3_step(ss) == SQLITE_ROW) {
            std::string row = "    row[" + std::to_string(rowNum++) + "]: ";
            int nc = sqlite3_column_count(ss);
            for (int i = 0; i < nc; ++i) {
              if (i)
                row += " | ";
              const char *cn = sqlite3_column_name(ss, i);
              row += (cn ? cn : "?");
              row += "=";
              int t = sqlite3_column_type(ss, i);
              if (t == SQLITE_TEXT) {
                const unsigned char *v = sqlite3_column_text(ss, i);
                std::string vs = v ? reinterpret_cast<const char *>(v) : "";
                if (vs.size() > 80)
                  vs = vs.substr(0, 80) + "...";
                row += vs;
              } else if (t == SQLITE_INTEGER) {
                row += std::to_string(sqlite3_column_int64(ss, i));
              } else if (t == SQLITE_FLOAT) {
                row += std::to_string(sqlite3_column_double(ss, i));
              } else if (t == SQLITE_NULL) {
                row += "NULL";
              } else {
                row += "<blob " + std::to_string(sqlite3_column_bytes(ss, i)) +
                       "B>";
              }
            }
            note(row);
          }
          sqlite3_finalize(ss);
        }
      }
      sqlite3_close(bdb);
    }
    closedir(dh);
  }

  mkdirs(std::string("/user/data/ps4hub"));
  FILE *f = std::fopen("/user/data/ps4hub/bookmark_scan.txt", "w");
  if (f) {
    std::fwrite(out.c_str(), 1, out.size(), f);
    std::fclose(f);
  }
  return "Scan saved to /user/data/ps4hub/bookmark_scan.txt (" +
         std::to_string(out.size()) + " bytes)";
}

} // namespace GhPlugins
