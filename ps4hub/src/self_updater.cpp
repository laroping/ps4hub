#include "self_updater.h"

#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "config.h"
#include "gh_plugins.h"
#include "libc_compat.h"
#include "update_cleanup.h"

namespace {

constexpr const char *kLatestReleaseUrl =
    "https://api.github.com/repos/laroping/ps4hub/releases/latest";
constexpr const char *kPkgSaveDir = "/data/pkg";

std::mutex g_mutex;
SelfUpdater::Snapshot g_snap;
std::atomic<bool> g_busy{false};
std::atomic<bool> g_autoStarted{false};

std::string lowerStr(std::string s) {
  for (char &c : s)
    c = (char)std::tolower((unsigned char)c);
  return s;
}

bool isHex64(const std::string &s) {
  if (s.size() != 64)
    return false;
  for (unsigned char c : s) {
    if (!std::isxdigit(c))
      return false;
  }
  return true;
}

std::string normalizeVer(const std::string &raw) {
  std::string s = raw;
  while (!s.empty() && std::isspace((unsigned char)s.front()))
    s.erase(s.begin());
  while (!s.empty() && std::isspace((unsigned char)s.back()))
    s.pop_back();
  if (!s.empty() && (s[0] == 'v' || s[0] == 'V'))
    s.erase(s.begin());
  return s;
}

std::vector<int> parseVerParts(const std::string &v) {
  std::vector<int> parts;
  std::string s = normalizeVer(v);
  size_t i = 0;
  while (i < s.size()) {
    if (!std::isdigit((unsigned char)s[i])) {
      if (s[i] == '-')
        break;
      ++i;
      continue;
    }
    int val = 0;
    while (i < s.size() && std::isdigit((unsigned char)s[i])) {
      val = val * 10 + (s[i] - '0');
      ++i;
    }
    parts.push_back(val);
    if (i < s.size() && s[i] == '.')
      ++i;
  }
  return parts;
}

std::string sanitizeTagForFilename(const std::string &v) {
  std::string out;
  for (unsigned char c : v) {
    if (std::isalnum(c) || c == '.' || c == '_' || c == '-')
      out.push_back((char)c);
  }
  return out.empty() ? std::string("latest") : out;
}

std::string formatMb(uint64_t bytes) {
  char buf[32];
  double mb = (double)bytes / (1024.0 * 1024.0);
  std::snprintf(buf, sizeof(buf), "%.1f MB", mb);
  return buf;
}

// Minimal streaming SHA-256 (FIPS 180-4) for downloaded package verification.
struct Sha256 {
  uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                   0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
  uint64_t totalBits = 0;
  uint8_t buf[64]{};
  size_t bufLen = 0;

  static inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32u - n));
  }

  void compress(const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
        0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
        0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
        0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
        0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
        0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
        0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
        0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      w[i] = ((uint32_t)block[i * 4] << 24) |
             ((uint32_t)block[i * 4 + 1] << 16) |
             ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 64; ++i) {
      uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t t1 = hh + S1 + ch + k[i] + w[i];
      uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + maj;
      hh = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
  }

  void update(const uint8_t *data, size_t len) {
    totalBits += (uint64_t)len * 8u;
    size_t off = 0;
    if (bufLen > 0) {
      size_t take = std::min(64 - bufLen, len);
      std::memcpy(buf + bufLen, data, take);
      bufLen += take;
      off += take;
      if (bufLen == 64) {
        compress(buf);
        bufLen = 0;
      }
    }
    while (off + 64 <= len) {
      compress(data + off);
      off += 64;
    }
    if (off < len) {
      bufLen = len - off;
      std::memcpy(buf, data + off, bufLen);
    }
  }

  std::string finalizeHex() {
    uint8_t tail[128]{};
    std::memcpy(tail, buf, bufLen);
    tail[bufLen] = 0x80;
    size_t padLen = (bufLen < 56) ? 64 : 128;
    for (int i = 0; i < 8; ++i)
      tail[padLen - 1 - i] = (uint8_t)((totalBits >> (i * 8)) & 0xFFu);
    for (size_t off = 0; off < padLen; off += 64)
      compress(tail + off);

    static const char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (int i = 0; i < 8; ++i) {
      for (int s = 28; s >= 0; s -= 4)
        out.push_back(kHex[(h[i] >> s) & 0xFu]);
    }
    return out;
  }
};

size_t skipWs(const std::string &j, size_t p) {
  while (p < j.size() && std::isspace((unsigned char)j[p]))
    ++p;
  return p;
}

bool parseJsonString(const std::string &j, size_t &p, std::string &out) {
  if (p >= j.size() || j[p] != '"')
    return false;
  ++p;
  out.clear();
  while (p < j.size()) {
    char c = j[p++];
    if (c == '"')
      return true;
    if (c == '\\' && p < j.size()) {
      char e = j[p++];
      if (e == '"' || e == '\\' || e == '/')
        out.push_back(e);
      else if (e == 'n')
        out.push_back('\n');
      else if (e == 'r')
        out.push_back('\r');
      else if (e == 't')
        out.push_back('\t');
      else
        out.push_back(e);
      continue;
    }
    out.push_back(c);
  }
  return false;
}

bool parseJsonUint(const std::string &j, size_t &p, uint64_t &out) {
  if (p >= j.size() || !std::isdigit((unsigned char)j[p]))
    return false;
  uint64_t v = 0;
  while (p < j.size() && std::isdigit((unsigned char)j[p])) {
    v = v * 10 + (uint64_t)(j[p] - '0');
    ++p;
  }
  out = v;
  return true;
}

size_t skipJsonValue(const std::string &j, size_t p) {
  p = skipWs(j, p);
  if (p >= j.size())
    return p;
  if (j[p] == '"') {
    std::string dummy;
    parseJsonString(j, p, dummy);
    return p;
  }
  if (j[p] == '{' || j[p] == '[') {
    char open = j[p], close = open == '{' ? '}' : ']';
    int depth = 1;
    ++p;
    while (p < j.size() && depth > 0) {
      if (j[p] == '"') {
        std::string dummy;
        parseJsonString(j, p, dummy);
        continue;
      }
      if (j[p] == open)
        ++depth;
      else if (j[p] == close)
        --depth;
      ++p;
    }
    return p;
  }
  while (p < j.size() && j[p] != ',' && j[p] != '}' && j[p] != ']' &&
         !std::isspace((unsigned char)j[p]))
    ++p;
  return p;
}

// Parses asset object fields at top depth so nested uploader fields cannot
// collide with asset metadata.
void parseAssetObject(const std::string &j, size_t &p,
                      SelfUpdater::ReleaseInfo &out) {
  if (p >= j.size() || j[p] != '{')
    return;
  ++p;
  while (p < j.size()) {
    p = skipWs(j, p);
    if (p >= j.size())
      break;
    if (j[p] == '}') {
      ++p;
      break;
    }
    if (j[p] == ',') {
      ++p;
      continue;
    }
    if (j[p] != '"') {
      p = skipJsonValue(j, p);
      continue;
    }
    std::string key;
    if (!parseJsonString(j, p, key))
      break;
    p = skipWs(j, p);
    if (p >= j.size() || j[p] != ':')
      break;
    p = skipWs(j, p + 1);
    if (p >= j.size())
      break;

    if (j[p] == '"') {
      std::string val;
      if (!parseJsonString(j, p, val))
        break;
      if (key == "name")
        out.assetName = val;
      else if (key == "browser_download_url")
        out.downloadUrl = val;
      else if (key == "digest") {
        std::string low = lowerStr(val);
        if (low.rfind("sha256:", 0) == 0) {
          std::string hex = low.substr(7);
          if (isHex64(hex))
            out.sha256 = hex;
        }
      }
    } else if (key == "size" && std::isdigit((unsigned char)j[p])) {
      parseJsonUint(j, p, out.sizeBytes);
    } else {
      p = skipJsonValue(j, p);
    }
  }
}

bool parseReleaseJson(const std::string &j, SelfUpdater::ReleaseInfo &out,
                      std::string &err) {
  out = SelfUpdater::ReleaseInfo{};
  size_t p = skipWs(j, 0);
  if (p >= j.size() || j[p] != '{') {
    err = "unexpected GitHub response";
    return false;
  }
  ++p;
  std::string tagName;
  SelfUpdater::ReleaseInfo exactAsset;
  SelfUpdater::ReleaseInfo pkgFallbackAsset;
  bool haveExact = false;
  bool haveFallback = false;

  while (p < j.size()) {
    p = skipWs(j, p);
    if (p >= j.size())
      break;
    if (j[p] == '}') {
      ++p;
      break;
    }
    if (j[p] == ',') {
      ++p;
      continue;
    }
    if (j[p] != '"') {
      p = skipJsonValue(j, p);
      continue;
    }
    std::string key;
    if (!parseJsonString(j, p, key))
      break;
    p = skipWs(j, p);
    if (p >= j.size() || j[p] != ':')
      break;
    p = skipWs(j, p + 1);
    if (p >= j.size())
      break;

    if (key == "tag_name" && j[p] == '"') {
      parseJsonString(j, p, tagName);
    } else if (key == "assets" && j[p] == '[') {
      ++p;
      while (p < j.size()) {
        p = skipWs(j, p);
        if (p >= j.size())
          break;
        if (j[p] == ']') {
          ++p;
          break;
        }
        if (j[p] == ',') {
          ++p;
          continue;
        }
        if (j[p] == '{') {
          SelfUpdater::ReleaseInfo item;
          parseAssetObject(j, p, item);
          std::string lowName = lowerStr(item.assetName);
          if (lowName == "ps4hub.pkg" && !item.downloadUrl.empty()) {
            exactAsset = item;
            haveExact = true;
          } else if (lowName.size() > 4 &&
                     lowName.rfind(".pkg") == lowName.size() - 4 &&
                     !item.downloadUrl.empty() && !haveFallback) {
            pkgFallbackAsset = item;
            haveFallback = true;
          }
        } else {
          p = skipJsonValue(j, p);
        }
      }
    } else {
      p = skipJsonValue(j, p);
    }
  }

  if (tagName.empty()) {
    err = "no tag_name in release";
    return false;
  }
  if (!haveExact && !haveFallback) {
    err = "no ps4hub.pkg asset in release " + tagName;
    return false;
  }
  out = haveExact ? exactAsset : pkgFallbackAsset;
  out.tag = tagName;
  out.version = normalizeVer(tagName);
  return true;
}

bool mkdirs(const std::string &path) {
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur.push_back(path[i]);
    if (path[i] == '/' && cur.size() > 1)
      mkdir(cur.c_str(), 0777);
  }
  if (!path.empty())
    mkdir(path.c_str(), 0777);
  struct stat st{};
  return Compat::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

} // namespace

SelfUpdater &SelfUpdater::instance() {
  static SelfUpdater u;
  return u;
}

int SelfUpdater::compareVersions(const std::string &a, const std::string &b) {
  std::vector<int> pa = parseVerParts(a);
  std::vector<int> pb = parseVerParts(b);
  size_t n = std::max(pa.size(), pb.size());
  pa.resize(n, 0);
  pb.resize(n, 0);
  for (size_t i = 0; i < n; ++i) {
    if (pa[i] != pb[i])
      return pa[i] < pb[i] ? -1 : 1;
  }
  return 0;
}

bool SelfUpdater::sha256File(const std::string &path, std::string &hexOut,
                             std::string &err) {
  FILE *f = std::fopen(path.c_str(), "rb");
  if (!f) {
    err = "cannot open " + path + " for hashing";
    return false;
  }
  Sha256 ctx;
  std::vector<uint8_t> buf(64 * 1024);
  while (true) {
    size_t n = std::fread(buf.data(), 1, buf.size(), f);
    if (n > 0)
      ctx.update(buf.data(), n);
    if (n < buf.size()) {
      if (std::ferror(f)) {
        std::fclose(f);
        err = "read error while hashing " + path;
        return false;
      }
      break;
    }
  }
  std::fclose(f);
  hexOut = ctx.finalizeHex();
  return true;
}

SelfUpdater::Snapshot SelfUpdater::snapshot() const {
  std::lock_guard<std::mutex> lk(g_mutex);
  return g_snap;
}

void SelfUpdater::setProgress(State st, const std::string &msg,
                              uint64_t dlBytes, uint64_t totalBytes) {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_snap.state = st;
  g_snap.busy = (st == State::Checking || st == State::Downloading ||
                 st == State::Verifying);
  g_snap.message = msg;
  if (dlBytes > 0)
    g_snap.bytesDownloaded = dlBytes;
  if (totalBytes > 0)
    g_snap.totalBytes = totalBytes;
}

void SelfUpdater::finishEvent(State st, const std::string &msg, bool error,
                              const std::string &savedPath) {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_snap.state = st;
  g_snap.busy = false;
  // The list row is single-line; keep detailed instructions in the popup event.
  g_snap.message = st == State::ReadyToInstall ? "Download verified. PKG ready." : msg;
  if (!savedPath.empty())
    g_snap.savedPkgPath = savedPath;
  g_snap.eventError = error;
  g_snap.eventMessage = msg;
  ++g_snap.eventSerial;
}

struct ThreadArgs {
  SelfUpdater *self;
  int action;
};

void SelfUpdater::startAutoCheck() {
  if (g_autoStarted.exchange(true))
    return;
  launch(Action::QuietCheck);
}

bool SelfUpdater::startCheck() { return launch(Action::ManualCheck); }

bool SelfUpdater::startUpdate() { return launch(Action::Download); }

bool SelfUpdater::launch(Action action) {
  if (g_busy.exchange(true))
    return false;
  auto *args = new ThreadArgs{this, (int)action};
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 512 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t t{};
  int rc = pthread_create(&t, &attr, &SelfUpdater::threadEntry, args);
  pthread_attr_destroy(&attr);
  if (rc != 0) {
    delete args;
    g_busy = false;
    return false;
  }
  return true;
}

void *SelfUpdater::threadEntry(void *arg) {
  auto *a = static_cast<ThreadArgs *>(arg);
  try {
    Action act = (Action)a->action;
    if (act == Action::QuietCheck) {
      UpdateCleanup::removeInstalledPackages(kPkgSaveDir, APP_VERSION, APP_TITLE_ID);
      a->self->runCheck(true);
    }
    else if (act == Action::ManualCheck)
      a->self->runCheck(false);
    else if (act == Action::Download)
      a->self->runUpdate();
  } catch (const std::exception &e) {
    a->self->finishEvent(State::Failed,
                         std::string("Updater error: ") + e.what(), true);
  } catch (...) {
    a->self->finishEvent(State::Failed, "Updater encountered an unknown error",
                         true);
  }
  g_busy = false;
  delete a;
  return nullptr;
}

void SelfUpdater::runCheck(bool quiet) {
  if (!quiet)
    setProgress(State::Checking, "Checking GitHub Releases...");

  std::string body, err;
  if (!GhPlugins::httpsGet(kLatestReleaseUrl, body, 256 * 1024, err)) {
    if (quiet) {
      std::lock_guard<std::mutex> lk(g_mutex);
      g_snap.busy = false;
      return;
    }
    finishEvent(State::Failed, "Update check failed: " + err, true);
    return;
  }

  ReleaseInfo rel;
  if (!parseReleaseJson(body, rel, err)) {
    if (quiet) {
      std::lock_guard<std::mutex> lk(g_mutex);
      g_snap.busy = false;
      return;
    }
    finishEvent(State::Failed, "Update check failed: " + err, true);
    return;
  }

  bool newer = compareVersions(rel.version, APP_VERSION) > 0;
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_snap.release = rel;
    g_snap.updateAvailable = newer;
    g_snap.totalBytes = rel.sizeBytes;
  }

  if (newer) {
    std::string msg = "PS4Hub v" + rel.version + " available (" +
                      formatMb(rel.sizeBytes) + ")";
    finishEvent(State::Available, msg, false);
  } else if (quiet) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_snap.state = State::UpToDate;
    g_snap.busy = false;
    g_snap.message = "PS4Hub v" APP_VERSION " is up to date";
  } else {
    finishEvent(State::UpToDate, "PS4Hub v" APP_VERSION " is up to date",
                false);
  }
}

void SelfUpdater::runUpdate() {
  ReleaseInfo rel;
  {
    std::lock_guard<std::mutex> lk(g_mutex);
    rel = g_snap.release;
  }

  if (rel.downloadUrl.empty() ||
      compareVersions(rel.version, APP_VERSION) <= 0) {
    setProgress(State::Checking, "Fetching latest release info...");
    std::string body, err;
    if (!GhPlugins::httpsGet(kLatestReleaseUrl, body, 256 * 1024, err) ||
        !parseReleaseJson(body, rel, err)) {
      finishEvent(State::Failed, "Release check failed: " + err, true);
      return;
    }
    bool newer = compareVersions(rel.version, APP_VERSION) > 0;
    {
      std::lock_guard<std::mutex> lk(g_mutex);
      g_snap.release = rel;
      g_snap.updateAvailable = newer;
    }
    if (!newer) {
      finishEvent(State::UpToDate, "PS4Hub v" APP_VERSION " is already latest",
                  false);
      return;
    }
  }

  if (rel.sha256.empty()) {
    finishEvent(State::Failed,
                "Release " + rel.tag + " has no SHA-256 digest on GitHub",
                true);
    return;
  }

  if (!mkdirs(kPkgSaveDir)) {
    finishEvent(State::Failed,
                std::string("Cannot create directory ") + kPkgSaveDir, true);
    return;
  }

  std::string safeVer = sanitizeTagForFilename(rel.version);
  std::string finalPath =
      std::string(kPkgSaveDir) + "/PS4Hub-v" + safeVer + ".pkg";
  std::string partPath = finalPath + ".part";

  // Reuse an already downloaded and verified package on disk when possible.
  struct stat existingSt{};
  bool alreadyVerifiedOnDisk = false;
  if (Compat::stat(finalPath.c_str(), &existingSt) == 0 &&
      existingSt.st_size > 0 &&
      (rel.sizeBytes == 0 || (uint64_t)existingSt.st_size == rel.sizeBytes)) {
    setProgress(State::Verifying, "Verifying existing package SHA-256...", 0,
                (uint64_t)existingSt.st_size);
    std::string diskHash, hashErr;
    if (sha256File(finalPath, diskHash, hashErr) && diskHash == rel.sha256)
      alreadyVerifiedOnDisk = true;
  }

  if (!alreadyVerifiedOnDisk) {
    std::remove(partPath.c_str());
    {
      std::lock_guard<std::mutex> lk(g_mutex);
      g_snap.bytesDownloaded = 0;
      g_snap.totalBytes = rel.sizeBytes;
    }
    setProgress(State::Downloading,
                "Downloading PS4Hub v" + rel.version + "...", 0, rel.sizeBytes);

    uint64_t lastUiBytes = 0;
    std::string dlErr;
    bool ok = GhPlugins::httpsGetToFile(
        rel.downloadUrl, partPath, dlErr, [&](uint64_t got) {
          if (got - lastUiBytes >= 64 * 1024 ||
              (rel.sizeBytes > 0 && got >= rel.sizeBytes)) {
            lastUiBytes = got;
            std::string m = "Downloading v" + rel.version + ": " +
                            formatMb(got) +
                            (rel.sizeBytes ? " / " + formatMb(rel.sizeBytes)
                                           : std::string());
            setProgress(State::Downloading, m, got, rel.sizeBytes);
          }
        });
    if (!ok) {
      std::remove(partPath.c_str());
      finishEvent(State::Failed, "Download failed: " + dlErr, true);
      return;
    }

    struct stat dlSt{};
    if (Compat::stat(partPath.c_str(), &dlSt) != 0 || dlSt.st_size <= 0 ||
        (rel.sizeBytes > 0 && (uint64_t)dlSt.st_size != rel.sizeBytes)) {
      long long gotSize = (long long)dlSt.st_size;
      std::remove(partPath.c_str());
      finishEvent(State::Failed,
                  "Downloaded package size mismatch (" +
                      std::to_string(gotSize) + " vs " +
                      std::to_string((unsigned long long)rel.sizeBytes) + ")",
                  true);
      return;
    }

    setProgress(State::Verifying, "Verifying SHA-256 checksum...",
                (uint64_t)dlSt.st_size, (uint64_t)dlSt.st_size);
    std::string actualHash, hashErr;
    if (!sha256File(partPath, actualHash, hashErr)) {
      std::remove(partPath.c_str());
      finishEvent(State::Failed, "SHA-256 verify error: " + hashErr, true);
      return;
    }
    if (actualHash != rel.sha256) {
      std::remove(partPath.c_str());
      finishEvent(State::Failed,
                  "SHA-256 mismatch (expected " + rel.sha256.substr(0, 12) +
                      "..., got " + actualHash.substr(0, 12) + "...)",
                  true);
      return;
    }

    std::remove(finalPath.c_str());
    if (std::rename(partPath.c_str(), finalPath.c_str()) != 0) {
      std::remove(partPath.c_str());
      finishEvent(State::Failed, "Cannot save verified package to " + finalPath,
                  true);
      return;
    }
  }

  finishEvent(State::ReadyToInstall, manualInstallInstructions(finalPath),
              false, finalPath);
}

std::string SelfUpdater::manualInstallInstructions(const std::string &path) {
  return "Download verified. Manual installation is required.\n\n"
         "1. Close PS4Hub and open GoldHEN > Debug Settings > Package Installer.\n"
         "2. Select and install " + path + ".\n"
         "3. If the PKG is missing, check GoldHEN Package Source and select "
         "the option that includes all drives.\n\n"
         "Launch the updated PS4Hub afterwards; it will remove its downloaded "
         "PKG and older downloaded PS4Hub updates automatically.";
}
