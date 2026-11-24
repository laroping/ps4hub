#include "pkg_installer.h"

#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstring>

#include "bgft_api.h"
#include "http_client.h"
#include "platform.h"
#include "sfo_parser.h"

namespace {

constexpr size_t kHeaderSize = 0x2000;
constexpr size_t kEntrySize = 0x20;
constexpr uint32_t kEntryParamSfo = 0x1000;
constexpr uint32_t kEntryIcon0 = 0x1200;

uint32_t be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | p[3];
}
uint64_t be64(const unsigned char *p) {
  return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

struct Header {
  uint32_t entryCount = 0, entryTableOffset = 0, contentType = 0,
           contentFlags = 0;
  uint64_t packageSize = 0;
  std::string contentId;
  unsigned char digest[32] = {};
};

bool parseHeader(const std::string &b, Header &h) {
  const unsigned char *p = (const unsigned char *)b.data();
  if (b.size() < kHeaderSize || std::memcmp(p,
                                            "\x7F"
                                            "CNT",
                                            4) != 0)
    return false;
  h.entryCount = be32(p + 0x10);
  h.entryTableOffset = be32(p + 0x18);
  h.contentId.assign((const char *)p + 0x40,
                     strnlen((const char *)p + 0x40, 0x24));
  h.contentType = be32(p + 0x74);
  h.contentFlags = be32(p + 0x78);
  h.packageSize = be64(p + 0x430);
  std::memcpy(h.digest, p + 0xFE0, 32);
  return h.entryCount > 0 && h.entryCount <= 0x1000 && h.contentId.size() == 36;
}

bool isPatch(uint32_t contentType) {
  return contentType == 0x1E; // PS4DP = delta patch
}

const char *packageType(uint32_t contentType) {
  switch (contentType) {
  case 0x1A:
    return "PS4GD";
  case 0x1B:
    return "PS4AC";
  case 0x1C:
    return "PS4AL";
  case 0x1E:
    return "PS4DP";
  default:
    return nullptr;
  }
}

std::string sfoString(const std::string &sfo, const char *key) {
  SfoFile f;
  return f.parse(sfo) ? f.str(key) : std::string();
}

std::string hexOf(const unsigned char *d, size_t n) {
  static const char *k = "0123456789ABCDEF";
  std::string s;
  for (size_t i = 0; i < n; ++i) {
    s += k[d[i] >> 4];
    s += k[d[i] & 15];
  }
  return s;
}

std::string jsonEscape(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if ((unsigned char)c < 0x20)
      o += ' ';
    else
      o += c;
  }
  return o;
}

bool writeFile(const std::string &path, const std::string &data) {
  FILE *f = std::fopen(path.c_str(), "wb");
  if (!f)
    return false;
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = (std::fclose(f) == 0) && ok;
  return ok;
}

std::string tempName() {
  static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  static unsigned counter = 0;
  pthread_mutex_lock(&m);
  unsigned n = counter++;
  pthread_mutex_unlock(&m);
  char b[32];
  std::snprintf(b, sizeof(b), "tmp_%05u_%04x", (unsigned)getpid() % 100000,
                n & 0xFFFF);
  return b;
}

bool fail(InstallResult &out, const std::string &msg) {
  out.error = msg;
  return false;
}

// Pulls every string value of "url" keys, in order (ref manifests).
std::vector<std::string> urlsInJson(const std::string &j) {
  std::vector<std::string> v;
  size_t p = 0;
  while ((p = j.find("\"url\"", p)) != std::string::npos) {
    p = j.find(':', p);
    if (p == std::string::npos)
      break;
    p = j.find('"', p);
    if (p == std::string::npos)
      break;
    std::string s;
    for (++p; p < j.size() && j[p] != '"'; ++p) {
      if (j[p] == '\\' && p + 1 < j.size())
        ++p;
      s += j[p];
    }
    if (!s.empty())
      v.push_back(s);
  }
  return v;
}

} // namespace

namespace PkgInstaller {

bool isServableName(const std::string &n) {
  if (n.empty() || n.size() > 64 || n.find("..") != std::string::npos)
    return false;
  for (char c : n)
    if (!(std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.'))
      return false;
  return true;
}

bool installDirect(const std::vector<std::string> &urls,
                   const std::string &serverIp, int serverPort, int userId,
                   InstallResult &out) {
  out = InstallResult{};
  const Bgft::Api *bgft = Platform::bgft();
  if (!bgft)
    return fail(out, "background downloads are unavailable (" +
                         Platform::status().bgftError + ")");
  if (urls.empty())
    return fail(out, "no package URL given");
  if (serverIp.empty())
    return fail(out, "the console has no network address");
  for (auto &u : urls)
    if (u.compare(0, 7, "http://") != 0 || u.size() >= 0x800)
      return fail(out, "invalid package URL: " + u);

  std::string err;
  HttpResult r;

  if (!Http::get(urls[0], 0, kHeaderSize, r, err))
    return fail(out, "pkg header: " + err);
  if (!r.partial)
    return fail(out, "the web server must support Range requests");
  Header h;
  if (!parseHeader(r.body, h))
    return fail(out, "not a PS4 package");
  const char *type = packageType(h.contentType);
  if (!type)
    return fail(out, "unsupported package content type");

  size_t tableSize = (size_t)h.entryCount * kEntrySize;
  if (!Http::get(urls[0], h.entryTableOffset, tableSize, r, err) ||
      r.body.size() != tableSize)
    return fail(out, "pkg entry table: " +
                         (err.empty() ? std::string("short read") : err));
  std::string table = r.body, sfo, icon;
  for (uint32_t i = 0; i < h.entryCount; ++i) {
    const unsigned char *e =
        (const unsigned char *)table.data() + i * kEntrySize;
    uint32_t id = be32(e), off = be32(e + 0x10), size = be32(e + 0x14);
    if (id == kEntryParamSfo && sfo.empty() && size && size < 0x100000) {
      if (!Http::get(urls[0], off, size, r, err) || r.body.size() != size)
        return fail(out, "param.sfo: " + err);
      sfo = r.body;
    } else if (id == kEntryIcon0 && icon.empty() && size && size < 0x400000) {
      if (Http::get(urls[0], off, size, r, err) && r.body.size() == size)
        icon = r.body;
    }
  }
  if (sfo.empty())
    return fail(out, "package has no PARAM.SFO");
  std::string title = sfoString(sfo, "TITLE");
  if (title.empty())
    title = h.contentId;

  std::vector<uint64_t> sizes;
  uint64_t total = 0;
  for (auto &u : urls) {
    uint64_t size = 0;
    bool ranges = false;
    if (!Http::probe(u, size, ranges, err))
      return fail(out, "cannot read size of " + u + ": " + err);
    sizes.push_back(size);
    total += size;
  }
  if (h.packageSize && total != h.packageSize)
    return fail(out, "piece sizes do not add up to the package size (missing "
                     "or extra pieces?)");

  mkdir(PS4HUB_WORK_DIR, 0777);
  std::string name = tempName();
  std::string manifestPath =
      std::string(PS4HUB_WORK_DIR) + "/" + name + ".json";
  std::string iconPath = std::string(PS4HUB_WORK_DIR) + "/" + name + ".png";
  std::string zeroMini(2 * 20, '0');
  std::string m = "{\"originalFileSize\":" +
                  std::to_string(h.packageSize ? h.packageSize : total) +
                  ",\"packageDigest\":\"" + hexOf(h.digest, 32) +
                  "\",\"numberOfSplitFiles\":" + std::to_string(urls.size()) +
                  ",\"pieces\":[";
  uint64_t offset = 0;
  for (size_t i = 0; i < urls.size(); ++i) {
    m += "{\"url\":\"" + jsonEscape(urls[i]) +
         "\",\"fileOffset\":" + std::to_string(offset) +
         ",\"fileSize\":" + std::to_string(sizes[i]) + ",\"hashValue\":\"" +
         zeroMini + "\"}";
    if (i + 1 < urls.size())
      m += ",";
    offset += sizes[i];
  }
  m += "]}";
  if (!writeFile(manifestPath, m))
    return fail(out, "cannot write " + manifestPath);
  bool hasIcon = !icon.empty() && writeFile(iconPath, icon);

  std::string contentUrl = "http://" + serverIp + ":" +
                           std::to_string(serverPort) + "/static/" + name +
                           ".json";
  Bgft::DownloadParam p{};
  p.userId = userId;
  p.entitlementType = 5;
  p.id = h.contentId.c_str();
  p.contentUrl = contentUrl.c_str();
  p.contentExUrl = "";
  p.contentName = title.c_str();
  p.iconPath = hasIcon ? iconPath.c_str() : "";
  p.skuId = "";
  p.option = Bgft::kOptDisableCdnQueryParam;
  p.playgoScenarioId = "0";
  p.releaseDate = "";
  p.packageType = type;
  p.packageSubType = "";
  p.packageSize = h.packageSize ? h.packageSize : total;

  out.title = title;
  out.iconPath = hasIcon ? iconPath : std::string();
  out.contentId = h.contentId;
  out.packageType = type;
  out.packageSize = p.packageSize;
  out.isPatch = isPatch(h.contentType);

  int32_t taskId = Bgft::kInvalidTask;
  // For patches we try the debug register entry first (that's what RPI
  // NextGen uses). If it fails (e.g. the FW's BGFT rejects a patch whose
  // base game was installed via a different path) fall back to the normal
  // registerTask; some firmwares accept patches through it too.
  int32_t rc;
  if (out.isPatch && bgft->debugRegisterPkg) {
    rc = bgft->debugRegisterPkg(&p, &taskId);
    if (rc != 0 && rc != Bgft::kErrAlreadyInstalled)
      rc = bgft->registerTask(&p, &taskId);
  } else {
    rc = bgft->registerTask(&p, &taskId);
    if (rc != 0 && rc != Bgft::kErrAlreadyInstalled && bgft->debugRegisterPkg)
      rc = bgft->debugRegisterPkg(&p, &taskId);
  }
  if (rc == Bgft::kErrAlreadyInstalled) {
    out.alreadyInstalled = true;
    out.taskId = -1;
    unlink(manifestPath.c_str());
    if (hasIcon)
      unlink(iconPath.c_str());
    return true;
  }
  if (rc != 0) {
    out.bgftError = rc;
    unlink(manifestPath.c_str());
    if (hasIcon)
      unlink(iconPath.c_str());
    char b[64];
    std::snprintf(b, sizeof(b), "download registration rejected (0x%08X)",
                  (unsigned)rc);
    return fail(out, b);
  }
  out.taskId = taskId;
  bgft->startTask(taskId); // clients may also call /api/start_task; a second
                           // start is harmless
  return true;
}

bool installRef(const std::string &manifestUrl, const std::string &serverIp,
                int serverPort, int userId, InstallResult &out) {
  HttpResult r;
  std::string err;
  if (!Http::get(manifestUrl, 0, 0, r, err, 1u << 20))
    return fail(out, "reference manifest: " + err);
  std::vector<std::string> urls = urlsInJson(r.body);
  if (urls.empty())
    return fail(out, "reference manifest has no pieces");
  return installDirect(urls, serverIp, serverPort, userId, out);
}

bool installFromFile(const std::string &localPath, const std::string &serveUrl,
                     const std::string &serverIp, int serverPort, int userId,
                     InstallResult &out) {
  out = InstallResult{};

  FILE *f = std::fopen(localPath.c_str(), "rb");
  if (!f)
    return fail(out, "cannot open " + localPath);

  std::string hdrBuf(kHeaderSize, '\0');
  size_t got = std::fread(&hdrBuf[0], 1, kHeaderSize, f);
  if (got < kHeaderSize) {
    std::fclose(f);
    return fail(out, "file too small for a PS4 package");
  }
  Header h;
  if (!parseHeader(hdrBuf, h)) {
    std::fclose(f);
    return fail(out, "not a PS4 package");
  }
  const char *type = packageType(h.contentType);
  if (!type) {
    std::fclose(f);
    return fail(out, "unsupported package content type");
  }

  size_t tableSize = (size_t)h.entryCount * kEntrySize;
  std::string table(tableSize, '\0');
  std::fseek(f, (long)h.entryTableOffset, SEEK_SET);
  if (std::fread(&table[0], 1, tableSize, f) != tableSize) {
    std::fclose(f);
    return fail(out, "pkg entry table truncated");
  }
  std::string sfo, icon;
  for (uint32_t i = 0; i < h.entryCount; ++i) {
    const unsigned char *e =
        (const unsigned char *)table.data() + i * kEntrySize;
    uint32_t id = be32(e), off = be32(e + 0x10), size = be32(e + 0x14);
    if (id == kEntryParamSfo && sfo.empty() && size && size < 0x100000) {
      sfo.resize(size);
      std::fseek(f, (long)off, SEEK_SET);
      if (std::fread(&sfo[0], 1, size, f) != size)
        sfo.clear();
    } else if (id == kEntryIcon0 && icon.empty() && size && size < 0x400000) {
      icon.resize(size);
      std::fseek(f, (long)off, SEEK_SET);
      if (std::fread(&icon[0], 1, size, f) != size)
        icon.clear();
    }
  }

  std::fseek(f, 0, SEEK_END);
  uint64_t fileSize = (uint64_t)std::ftell(f);
  std::fclose(f);

  if (sfo.empty())
    return fail(out, "package has no PARAM.SFO");
  std::string title = sfoString(sfo, "TITLE");
  if (title.empty())
    title = h.contentId;

  out.title = title;
  out.contentId = h.contentId;
  out.packageType = type;
  out.packageSize = fileSize;
  out.isPatch = isPatch(h.contentType);

  // Full BGFT path (identical to Remote Install / NTFS install). Writes a
  // manifest, registers a BGFT download task pointing at /static/<name>,
  // and starts it. This is what allows GoldHEN to generate the fake RIF license
  // for FPKGs / homebrew / NoPSN apps so they launch properly without
  // CE-30012-6.
  const Bgft::Api *bgft = Platform::bgft();
  if (!bgft)
    return fail(out, "package installer unavailable");

  mkdir(PS4HUB_WORK_DIR, 0777);
  std::string name = tempName();
  std::string manifestPath =
      std::string(PS4HUB_WORK_DIR) + "/" + name + ".json";
  std::string iconPath = std::string(PS4HUB_WORK_DIR) + "/" + name + ".png";
  std::string zeroMini(2 * 20, '0');
  std::string m = "{\"originalFileSize\":" + std::to_string(fileSize) +
                  ",\"packageDigest\":\"" + hexOf(h.digest, 32) +
                  "\",\"numberOfSplitFiles\":1,\"pieces\":[" + "{\"url\":\"" +
                  jsonEscape(serveUrl) + "\",\"fileOffset\":0" +
                  ",\"fileSize\":" + std::to_string(fileSize) +
                  ",\"hashValue\":\"" + zeroMini + "\"}]}";
  if (!writeFile(manifestPath, m))
    return fail(out, "cannot write " + manifestPath);
  bool hasIcon = !icon.empty() && writeFile(iconPath, icon);

  std::string contentUrl = "http://" + serverIp + ":" +
                           std::to_string(serverPort) + "/static/" + name +
                           ".json";
  Bgft::DownloadParam p{};
  p.userId = userId;
  p.entitlementType = 5;
  p.id = h.contentId.c_str();
  p.contentUrl = contentUrl.c_str();
  p.contentExUrl = "";
  p.contentName = title.c_str();
  p.iconPath = hasIcon ? iconPath.c_str() : "";
  p.skuId = "";
  p.option = Bgft::kOptDisableCdnQueryParam;
  p.playgoScenarioId = "0";
  p.releaseDate = "";
  p.packageType = type;
  p.packageSubType = "";
  p.packageSize = fileSize;

  int32_t taskId = Bgft::kInvalidTask;
  int32_t rc;
  if (out.isPatch && bgft->debugRegisterPkg) {
    rc = bgft->debugRegisterPkg(&p, &taskId);
    if (rc != 0 && rc != Bgft::kErrAlreadyInstalled)
      rc = bgft->registerTask(&p, &taskId);
  } else {
    rc = bgft->registerTask(&p, &taskId);
    if (rc != 0 && rc != Bgft::kErrAlreadyInstalled && bgft->debugRegisterPkg)
      rc = bgft->debugRegisterPkg(&p, &taskId);
  }

  if (rc == Bgft::kErrAlreadyInstalled) {
    out.alreadyInstalled = true;
    out.taskId = -1;
    unlink(manifestPath.c_str());
    if (hasIcon)
      unlink(iconPath.c_str());
    return true;
  }
  if (rc != 0) {
    out.bgftError = rc;
    unlink(manifestPath.c_str());
    if (hasIcon)
      unlink(iconPath.c_str());
    char b[64];
    std::snprintf(b, sizeof(b), "download registration rejected (0x%08X)",
                  (unsigned)rc);
    return fail(out, b);
  }
  out.taskId = taskId;
  bgft->startTask(taskId);
  return true;
}

} // namespace PkgInstaller
