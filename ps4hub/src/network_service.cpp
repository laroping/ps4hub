#include "network_service.h"
#include "sfo_parser.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bgft_api.h"
#include "config.h"
#include "libc_compat.h"
#include "ntfs_manager.h"
#include "pkg_installer.h"
#include "platform.h"
#include "port_owner.h"

namespace {

struct Lock {
  pthread_mutex_t *m;
  explicit Lock(pthread_mutex_t *mm) : m(mm) { pthread_mutex_lock(m); }
  ~Lock() { pthread_mutex_unlock(m); }
};

constexpr size_t kMaxHeader = 16 * 1024;
constexpr size_t kMaxBody = 256 * 1024;
constexpr int kMaxClients = 12;

void setTimeouts(int fd, int seconds) {
  timeval tv{};
  tv.tv_sec = seconds;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

bool sendAll(int fd, const char *data, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = send(fd, data + off, len - off, 0);
    if (n <= 0)
      return false;
    off += (size_t)n;
  }
  return true;
}
bool sendAll(int fd, const std::string &s) {
  return sendAll(fd, s.data(), s.size());
}

bool isLanAddress(uint32_t hostOrder) {
  uint8_t a = hostOrder >> 24, b = (hostOrder >> 16) & 0xFF;
  return a == 10 || a == 127 || (a == 172 && b >= 16 && b <= 31) ||
         (a == 192 && b == 168);
}

std::string lower(std::string s) {
  for (auto &c : s)
    c = (char)std::tolower((unsigned char)c);
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

size_t skipWs(const std::string &j, size_t p) {
  while (p < j.size() && std::isspace((unsigned char)j[p]))
    ++p;
  return p;
}

size_t findValue(const std::string &j, const char *key) {
  std::string needle = std::string("\"") + key + "\"";
  size_t p = 0;
  while ((p = j.find(needle, p)) != std::string::npos) {
    size_t q = skipWs(j, p + needle.size());
    if (q < j.size() && j[q] == ':')
      return skipWs(j, q + 1);
    p += needle.size();
  }
  return std::string::npos;
}

bool readString(const std::string &j, size_t &p, std::string &out) {
  if (p >= j.size() || j[p] != '"')
    return false;
  out.clear();
  for (++p; p < j.size(); ++p) {
    char c = j[p];
    if (c == '"') {
      ++p;
      return true;
    }
    if (c == '\\' && p + 1 < j.size()) {
      char n = j[++p];
      out += n == 'n' ? '\n' : n == 't' ? '\t' : n;
    } else {
      out += c;
    }
  }
  return false;
}

bool getString(const std::string &j, const char *key, std::string &out) {
  size_t p = findValue(j, key);
  return p != std::string::npos && readString(j, p, out);
}

bool getNumber(const std::string &j, const char *key, long long &out) {
  size_t p = findValue(j, key);
  if (p == std::string::npos)
    return false;
  if (p < j.size() && j[p] == '"')
    ++p; // tolerate "42"
  char *end = nullptr;
  out = std::strtoll(j.c_str() + p, &end, 0);
  return end != j.c_str() + p;
}

std::vector<std::string> getStringArray(const std::string &j, const char *key) {
  std::vector<std::string> v;
  size_t p = findValue(j, key);
  if (p == std::string::npos || j[p] != '[')
    return v;
  p = skipWs(j, p + 1);
  std::string s;
  while (p < j.size() && j[p] != ']') {
    if (!readString(j, p, s))
      break;
    v.push_back(s);
    p = skipWs(j, p);
    if (p < j.size() && j[p] == ',')
      p = skipWs(j, p + 1);
  }
  return v;
}

const char *kCors = "Access-Control-Allow-Origin: *\r\n"
                    "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                    "Access-Control-Allow-Headers: Content-Type\r\n";

std::string reason(int code) {
  switch (code) {
  case 200:
    return "OK";
  case 204:
    return "No Content";
  case 400:
    return "Bad Request";
  case 403:
    return "Forbidden";
  case 404:
    return "Not Found";
  case 503:
    return "Service Unavailable";
  default:
    return "Internal Server Error";
  }
}

std::string httpResponse(int code, const std::string &body,
                         const char *type = "application/json") {
  return "HTTP/1.1 " + std::to_string(code) + " " + reason(code) +
         "\r\nContent-Type: " + type +
         "\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n" + kCors + "\r\n" + body;
}

int hexVal(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

std::string urlDecode(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out += (char)(hi * 16 + lo);
        i += 2;
        continue;
      }
    }
    out += (s[i] == '+') ? ' ' : s[i];
  }
  return out;
}

std::string okJson() { return "{ \"status\": \"success\" }\n"; }

std::string failMessage(const std::string &msg, int &code) {
  code = 500;
  return "{ \"status\": \"fail\", \"error\": \"" + jsonEscape(msg) + "\" }\n";
}

std::string failCode(int32_t errorCode, int &code) {
  code = 200;
  char b[64];
  std::snprintf(b, sizeof(b),
                "{ \"status\": \"fail\", \"error_code\": 0x%08X }\n",
                (unsigned)errorCode);
  return b;
}

// Bound + listening socket on `port`, or -1 with `err` = errno.
int openListener(int port, int &err) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    err = errno;
    return -1;
  }
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons((uint16_t)port);
  if (bind(fd, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, 16) != 0) {
    err = errno;
    close(fd);
    return -1;
  }
  err = 0;
  return fd;
}

struct ClientJob {
  NetworkService *svc;
  int fd;
  std::string peer;
};

std::atomic<int> *g_active = nullptr;

void *clientEntry(void *arg) {
  ClientJob *job = static_cast<ClientJob *>(arg);
  try {
    job->svc->handleClient(job->fd, job->peer);
  } catch (const std::exception &e) {
    std::printf("[PS4Hub] HTTP worker exception: %s\n", e.what());
  } catch (...) {
    std::printf("[PS4Hub] HTTP worker exception (unknown)\n");
  }
  close(job->fd);
  if (g_active)
    --*g_active;
  delete job;
  return nullptr;
}

} // namespace

NetworkService &NetworkService::instance() {
  static NetworkService s;
  return s;
}

void NetworkService::addLog(const std::string &line) {
  Lock l(&mutex_);
  log_.push_back(line);
  while (log_.size() > 60)
    log_.pop_front();
}

void NetworkService::setError(const std::string &e) {
  {
    Lock l(&mutex_);
    lastError_ = e;
  }
  addLog("ERROR: " + e);
}

std::vector<std::string> NetworkService::log() {
  Lock l(&mutex_);
  return std::vector<std::string>(log_.begin(), log_.end());
}

std::vector<InstallTask> NetworkService::tasks() {
  Lock l(&mutex_);
  return tasks_;
}

std::string NetworkService::lastError() {
  Lock l(&mutex_);
  return lastError_;
}

void NetworkService::rememberTask(const InstallTask &t) {
  Lock l(&mutex_);
  for (auto &e : tasks_)
    if (e.taskId == t.taskId && t.taskId >= 0) {
      e = t;
      return;
    }
  tasks_.push_back(t);
  if (tasks_.size() > 24)
    tasks_.erase(tasks_.begin());
}

bool NetworkService::start(int port, int userId) {
  if (running_)
    return true;
  port_ = port;
  userId_ = userId;
  g_active = &activeClients_;

  if (!Platform::bgftReady())
    setError("Background downloads unavailable: " +
             Platform::status().bgftError);

  int err = 0;
  listenFd_ = openListener(port, err);

  // EADDRINUSE: another app (typically an installer left open in the
  // background) owns the port. Close it if it is an ordinary app, then
  // retry; system processes are never touched.
  if (listenFd_ < 0 && err == EADDRINUSE) {
    PortOwnerInfo owner;
    if (PortOwner::find((uint16_t)port, owner)) {
      if (PortOwner::terminate(owner)) {
        addLog("Closed " + PortOwner::describe(owner) + " to free port " +
               std::to_string(port));
        for (int i = 0; i < 30 && listenFd_ < 0;
             ++i) { // up to ~3 s for the kernel to release it
          usleep(100 * 1000);
          listenFd_ = openListener(port, err);
        }
      } else {
        addLog("Port " + std::to_string(port) + " is held by " +
               PortOwner::describe(owner) + " (protected)");
      }
    } else {
      addLog("Port " + std::to_string(port) +
             " is busy and its owner could not be identified");
    }
  }
  for (int alt = port + 1;
       listenFd_ < 0 && err == EADDRINUSE && alt <= port + 9; ++alt) {
    listenFd_ = openListener(alt, err);
    if (listenFd_ >= 0) {
      port_ = alt;
      addLog("Using port " + std::to_string(alt) + " instead of " +
             std::to_string(port));
    }
  }
  if (listenFd_ < 0) {
    setError("Cannot listen on port " + std::to_string(port) + " (errno " +
             std::to_string(err) + ")");
    return false;
  }

  running_ = true;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  int trc = pthread_create(&thread_, &attr, &NetworkService::acceptEntry, this);
  pthread_attr_destroy(&attr);
  if (trc != 0) {
    running_ = false;
    close(listenFd_);
    listenFd_ = -1;
    setError("pthread_create failed");
    return false;
  }
  threadStarted_ = true;
  addLog("Listening on port " + std::to_string(port_));
  return true;
}

void NetworkService::stop() {
  if (!threadStarted_)
    return;
  running_ = false; // accept loop polls with a short timeout
  pthread_join(thread_, nullptr);
  threadStarted_ = false;
  if (listenFd_ >= 0) {
    close(listenFd_);
    listenFd_ = -1;
  }

  // Wait for in-flight detached worker threads to finish before letting
  // the destructor run. Workers touch log_/tasks_/lastError_ under mutex_,
  // so tearing the service down while one is still running would be a
  // use-after-free. Bounded wait so shutdown can never hang.
  for (int i = 0; i < 50 && activeClients_.load() > 0; ++i) {
    usleep(100 * 1000); // 100 ms * 50 = up to 5 s
  }
  addLog("Service stopped");
}

void *NetworkService::acceptEntry(void *self) {
  try {
    static_cast<NetworkService *>(self)->serve();
  } catch (const std::exception &e) {
    std::printf("[PS4Hub] accept thread exception: %s\n", e.what());
  } catch (...) {
    std::printf("[PS4Hub] accept thread exception (unknown)\n");
  }
  return nullptr;
}

void NetworkService::serve() {
  while (running_) {
    pollfd pfd{listenFd_, POLLIN, 0};
    if (poll(&pfd, 1, 250) <= 0 || !(pfd.revents & POLLIN))
      continue;

    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    int client = accept(listenFd_, (sockaddr *)&peer, &len);
    if (client < 0)
      continue;

    char peerStr[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &peer.sin_addr, peerStr, sizeof(peerStr));
    setTimeouts(client, 30);

    if (!isLanAddress(ntohl(peer.sin_addr.s_addr))) {
      int code = 403;
      sendAll(
          client,
          httpResponse(
              code,
              "{ \"status\": \"fail\", \"error\": \"LAN clients only\" }\n"));
      close(client);
      continue;
    }
    if (activeClients_.load() >= kMaxClients) {
      sendAll(
          client,
          httpResponse(503, "{ \"status\": \"fail\", \"error\": \"busy\" }\n"));
      close(client);
      continue;
    }

    ++activeClients_;
    ClientJob *job = new ClientJob{this, client, peerStr};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 512 * 1024);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t t;
    if (pthread_create(&t, &attr, clientEntry, job) != 0) {
      handleClient(client, peerStr); // fall back to serving inline
      close(client);
      --activeClients_;
      delete job;
    }
    pthread_attr_destroy(&attr);
  }
}

void NetworkService::handleClient(int fd, const std::string &peer) {
  std::string req;
  char buf[8192];
  size_t headerEnd = std::string::npos;
  while (headerEnd == std::string::npos) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0)
      return;
    req.append(buf, (size_t)n);
    headerEnd = req.find("\r\n\r\n");
    if (headerEnd == std::string::npos && req.size() > kMaxHeader) {
      sendAll(
          fd,
          httpResponse(
              400,
              "{ \"status\": \"fail\", \"error\": \"header too large\" }\n"));
      return;
    }
  }

  std::string head = req.substr(0, headerEnd);
  std::string headLower = lower(head);
  size_t contentLength = 0;
  size_t cl = headLower.find("\r\ncontent-length:");
  if (cl != std::string::npos)
    contentLength = std::strtoul(head.c_str() + cl + 17, nullptr, 10);
  if (contentLength > kMaxBody) {
    sendAll(
        fd,
        httpResponse(
            400, "{ \"status\": \"fail\", \"error\": \"body too large\" }\n"));
    return;
  }
  std::string body = req.substr(headerEnd + 4);
  while (body.size() < contentLength) {
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0)
      break;
    body.append(buf, (size_t)n);
  }
  if (body.size() > contentLength)
    body.resize(contentLength);

  char method[16] = {0}, rawPath[1024] = {0};
  std::sscanf(head.c_str(), "%15s %1023s", method, rawPath);
  std::string m = method, fullPath = rawPath, path = fullPath;
  std::string queryStr;
  size_t q = path.find('?');
  if (q != std::string::npos) {
    queryStr = path.substr(q + 1);
    path.resize(q);
  }

  if (m == "OPTIONS") { // CORS preflight from browser-based senders
    sendAll(fd, "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\nConnection: "
                "close\r\nAccess-Control-Max-Age: 86400\r\n" +
                    std::string(kCors) + "\r\n");
    return;
  }
  if ((m == "GET" || m == "HEAD") && path == "/stream/ntfs") {
    if (!serveNtfsStream(fd, queryStr, headLower, m == "HEAD"))
      sendAll(fd, httpResponse(404, "{ \"status\": \"fail\", \"error\": \"ntfs "
                                    "file not found\" }\n"));
    return;
  }
  if ((m == "GET" || m == "HEAD") && path.compare(0, 8, "/static/") == 0) {
    if (!serveStatic(fd, path, headLower, m == "HEAD"))
      sendAll(fd,
              httpResponse(
                  404, "{ \"status\": \"fail\", \"error\": \"not found\" }\n"));
    return;
  }
  // Redirect bare /load and /autoloader to their trailing-slash form so the
  // browser resolves relative URLs (style.css, slopkit/..., etc.) under the
  // right base path.
  if (m == "GET" && (path == "/load" || path == "/autoloader")) {
    std::string hdr = "HTTP/1.1 302 Found\r\nLocation: " + path +
                      "/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    sendAll(fd, hdr);
    return;
  }
  // Short alias /load/ and canonical /autoloader/ both serve the autoloader.
  if (m == "GET" && (path.compare(0, 12, "/autoloader/") == 0 ||
                     path.compare(0, 6, "/load/") == 0)) {
    if (!serveAutoloader(fd, path))
      sendAll(fd,
              httpResponse(
                  404, "{ \"status\": \"fail\", \"error\": \"not found\" }\n"));
    return;
  }
  // Manifests reference ../img.jpg and ../style.css — relative to a sub-
  // directory under /load/, those resolve to /img.jpg and /style.css at
  // the server root.  Serve them straight from the autoloader directory.
  if (m == "GET" && (path == "/img.jpg" || path == "/style.css")) {
    std::string redirected = "/load" + path;
    if (!serveAutoloader(fd, redirected))
      sendAll(fd,
              httpResponse(
                  404, "{ \"status\": \"fail\", \"error\": \"not found\" }\n"));
    return;
  }
  if (path != "/" && path != "/api/v2/system" && path != "/api/ping")
    addLog(peer + "  " + m + " " + path);

  int code = 200;
  std::string json = route(m, path, body, code);
  sendAll(fd, httpResponse(code, json));
}

bool NetworkService::serveStatic(int fd, const std::string &path,
                                 const std::string &headLower, bool headOnly) {
  std::string name = path.substr(8);
  if (!PkgInstaller::isServableName(name))
    return false;
  std::string file = std::string(PS4HUB_WORK_DIR) + "/" + name;
  int in = open(file.c_str(), O_RDONLY);
  if (in < 0)
    return false;
  struct stat st;
  if (Compat::fstat(in, &st) != 0) {
    close(in);
    return false;
  }
  uint64_t total = (uint64_t)st.st_size;

  const char *type = "application/json";
  if (name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0)
    type = "image/png";
  else if (name.size() > 4 && name.compare(name.size() - 4, 4, ".pkg") == 0)
    type = "application/octet-stream";

  uint64_t rangeStart = 0, rangeEnd = total - 1;
  bool hasRange = false;
  size_t rp = headLower.find("\r\nrange: bytes=");
  if (rp != std::string::npos) {
    rp += 15; // strlen("\r\nrange: bytes=")
    size_t dash = headLower.find('-', rp);
    if (dash != std::string::npos) {
      if (dash > rp)
        rangeStart = std::strtoull(headLower.c_str() + rp, nullptr, 10);
      size_t eol = headLower.find('\r', dash);
      std::string endStr = headLower.substr(dash + 1, eol - dash - 1);
      if (!endStr.empty())
        rangeEnd = std::strtoull(endStr.c_str(), nullptr, 10);
      else
        rangeEnd = total - 1;
      if (rangeStart <= rangeEnd && rangeEnd < total)
        hasRange = true;
    }
  }

  uint64_t length = hasRange ? (rangeEnd - rangeStart + 1) : total;
  std::string hdr;
  if (hasRange) {
    hdr = "HTTP/1.1 206 Partial Content\r\nContent-Type: " + std::string(type) +
          "\r\nContent-Range: bytes " + std::to_string(rangeStart) + "-" +
          std::to_string(rangeEnd) + "/" + std::to_string(total) +
          "\r\nContent-Length: " + std::to_string(length) +
          "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n" + kCors + "\r\n";
  } else {
    hdr = "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(type) +
          "\r\nContent-Length: " + std::to_string(length) +
          "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n" + kCors + "\r\n";
  }

  bool ok = sendAll(fd, hdr);
  if (!headOnly) {
    if (hasRange && rangeStart > 0)
      lseek(in, (off_t)rangeStart, SEEK_SET);
    char buf[16 * 1024];
    uint64_t sent = 0;
    while (ok && sent < length) {
      size_t want = sizeof(buf);
      if (length - sent < want)
        want = (size_t)(length - sent);
      ssize_t n = read(in, buf, want);
      if (n <= 0)
        break;
      ok = sendAll(fd, buf, (size_t)n);
      sent += (uint64_t)n;
    }
  }
  close(in);
  return true;
}

bool NetworkService::serveNtfsStream(int fd, const std::string &query,
                                     const std::string &headLower,
                                     bool headOnly) {
  std::string ntfsPath;
  size_t pp = query.find("path=");
  if (pp == std::string::npos)
    return false;
  ntfsPath = urlDecode(query.substr(pp + 5));
  // Strip any extra query params that follow path= (e.g. "&foo=bar"); BGFT
  // likes to append garbage and we only care about the path itself.
  size_t amp = ntfsPath.find('&');
  if (amp != std::string::npos)
    ntfsPath = ntfsPath.substr(0, amp);
  if (ntfsPath.empty())
    return false;

  auto &ntfs = NtfsManager::instance();
  if (!ntfs.mounted())
    return false;

  int64_t total64 = ntfs.fileSize(ntfsPath);
  if (total64 <= 0)
    return false;
  uint64_t total = (uint64_t)total64;

  uint64_t rangeStart = 0, rangeEnd = total - 1;
  bool hasRange = false;
  size_t rp = headLower.find("\r\nrange: bytes=");
  if (rp != std::string::npos) {
    rp += 15;
    size_t dash = headLower.find('-', rp);
    if (dash != std::string::npos) {
      if (dash > rp)
        rangeStart = std::strtoull(headLower.c_str() + rp, nullptr, 10);
      size_t eol = headLower.find('\r', dash);
      std::string endStr = headLower.substr(dash + 1, eol - dash - 1);
      if (!endStr.empty())
        rangeEnd = std::strtoull(endStr.c_str(), nullptr, 10);
      else
        rangeEnd = total - 1;
      if (rangeStart <= rangeEnd && rangeEnd < total)
        hasRange = true;
    }
  }

  uint64_t length = hasRange ? (rangeEnd - rangeStart + 1) : total;
  std::string hdr;
  if (hasRange) {
    hdr =
        "HTTP/1.1 206 Partial Content\r\nContent-Type: application/octet-stream"
        "\r\nContent-Range: bytes " +
        std::to_string(rangeStart) + "-" + std::to_string(rangeEnd) + "/" +
        std::to_string(total) +
        "\r\nContent-Length: " + std::to_string(length) +
        "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n" + kCors + "\r\n";
  } else {
    hdr = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream"
          "\r\nContent-Length: " +
          std::to_string(length) +
          "\r\nAccept-Ranges: bytes\r\nConnection: close\r\n" + kCors + "\r\n";
  }

  bool ok = sendAll(fd, hdr);
  if (!headOnly) {
    char buf[64 * 1024];
    uint64_t sent = 0;
    uint64_t offset = hasRange ? rangeStart : 0;
    while (ok && sent < length) {
      uint64_t want = sizeof(buf);
      if (length - sent < want)
        want = length - sent;
      int64_t got = ntfs.readFile(ntfsPath, offset, buf, want);
      if (got <= 0)
        break;
      ok = sendAll(fd, buf, (size_t)got);
      sent += (uint64_t)got;
      offset += (uint64_t)got;
    }
  }
  return true;
}

bool NetworkService::serveAutoloader(int fd, const std::string &path) {
  // Accept both /autoloader/... and /load/... as the short alias.  The
  // bare /load and /autoloader forms are redirected upstream.
  std::string sub;
  if (path.compare(0, 12, "/autoloader/") == 0)
    sub = path.substr(12);
  else if (path.compare(0, 6, "/load/") == 0)
    sub = path.substr(6);
  else
    return false;

  if (sub.empty() || sub.back() == '/')
    sub += "index.html";

  // Normalize backslashes (some manifests from Windows reference files with \).
  for (auto &c : sub)
    if (c == '\\')
      c = '/';

  // Strip leading parent-dir segments the manifests use (../img.jpg,
  // ../style.css).
  while (sub.compare(0, 3, "../") == 0)
    sub = sub.substr(3);

  if (sub.find("..") != std::string::npos || sub[0] == '/')
    return false;

  std::string file = "/user/data/ps4hub/autoloader/" + sub;
  int in = open(file.c_str(), O_RDONLY);
  if (in < 0)
    return false;
  struct stat st;
  if (Compat::fstat(in, &st) != 0) {
    close(in);
    return false;
  }

  auto ext = [&](const char *e) {
    size_t l = sub.size(), le = std::strlen(e);
    return l >= le && sub.compare(l - le, le, e) == 0;
  };
  const char *mime = "application/octet-stream";
  if (ext(".html") || ext(".htm"))
    mime = "text/html; charset=utf-8";
  else if (ext(".js"))
    mime = "text/javascript; charset=utf-8";
  else if (ext(".css"))
    mime = "text/css; charset=utf-8";
  else if (ext(".json"))
    mime = "application/json";
  else if (ext(".png"))
    mime = "image/png";
  else if (ext(".jpg") || ext(".jpeg"))
    mime = "image/jpeg";
  else if (ext(".manifest"))
    mime = "text/cache-manifest";
  else if (ext(".elf"))
    mime = "application/octet-stream";

  // Manifests need backslash → slash transformation so AppCache can resolve the
  // URLs.
  if (ext(".manifest")) {
    std::string body;
    char buf[4096];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
      body.append(buf, n);
    close(in);
    for (auto &c : body)
      if (c == '\\')
        c = '/';
    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: "
                      "text/cache-manifest\r\nContent-Length: " +
                      std::to_string(body.size()) +
                      "\r\nCache-Control: no-cache\r\nConnection: close\r\n" +
                      kCors + "\r\n";
    if (!sendAll(fd, hdr))
      return true;
    sendAll(fd, body);
    return true;
  }

  // Must-revalidate for everything else (AppCache will cache them itself).
  std::string hdr =
      "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(mime) +
      "\r\nContent-Length: " + std::to_string((long long)st.st_size) +
      "\r\nCache-Control: no-cache\r\nConnection: close\r\n" + kCors + "\r\n";
  bool ok = sendAll(fd, hdr);
  char buf[16 * 1024];
  ssize_t n;
  while (ok && (n = read(in, buf, sizeof(buf))) > 0)
    ok = sendAll(fd, buf, (size_t)n);
  close(in);
  return true;
}

std::string NetworkService::route(const std::string &m, const std::string &path,
                                  const std::string &body, int &code) {
  if (m == "GET" && (path == "/" || path == "/api/v2/system"))
    return apiSystem();
  if (m == "GET" && path == "/api/ping")
    return okJson();
  if (m != "POST") {
    code = 404;
    return "{ \"status\": \"fail\", \"error\": \"not found\" }\n";
  }

  if (path == "/api/install")
    return apiInstall(body, false, code);
  if (path == "/api/v2/install")
    return apiInstall(body, true, code);
  if (path == "/api/get_task_progress")
    return apiProgress(body, code);
  if (path == "/api/start_task" || path == "/api/stop_task" ||
      path == "/api/pause_task" || path == "/api/resume_task" ||
      path == "/api/unregister_task")
    return apiTaskControl(path, body, code);
  if (path == "/api/find_task")
    return apiFindTask(body, code);
  if (path == "/api/is_exists")
    return apiIsExists(body, code);
  if (path.compare(0, 14, "/api/uninstall") == 0)
    return apiUninstall(path, body, code);
  if (path == "/api/task") { // PS4Hub 1.0 compatibility
    long long id;
    return getNumber(body, "task_id", id) &&
                   body.find("\"url\"") == std::string::npos
               ? apiProgress(body, code)
               : apiInstall(body, true, code);
  }
  code = 404;
  return "{ \"status\": \"fail\", \"error\": \"unknown endpoint\" }\n";
}

std::string NetworkService::apiInstall(const std::string &body, bool v2,
                                       int &code) {
  std::string type, url;
  std::vector<std::string> urls;
  bool ref = false;
  if (getString(body, "type", type)) {
    if (type == "direct")
      urls = getStringArray(body, "packages");
    else if (type == "ref_pkg_url") {
      ref = true;
      getString(body, "url", url);
    } else
      return failMessage("Invalid type of parameter 'type'.", code);
  } else if (getString(body, "ref_pkg_url", url)) {
    ref = true;
  } else {
    urls = getStringArray(body, "urls");
    if (urls.empty())
      urls = getStringArray(body, "packages");
    if (urls.empty() && getString(body, "url", url))
      urls.push_back(url);
  }
  if (ref ? url.empty() : urls.empty())
    return failMessage("No package URL specified.", code);

  std::string ip = Platform::localIp();
  InstallResult r;
  bool ok = ref ? PkgInstaller::installRef(url, ip, port_, userId_, r)
                : PkgInstaller::installDirect(urls, ip, port_, userId_, r);
  if (!ok) {
    setError(r.error);
    return r.bgftError ? failCode(r.bgftError, code)
                       : failMessage(r.error, code);
  }

  if (r.alreadyInstalled) {
    addLog(r.title + " is already installed");
  } else {
    InstallTask t;
    t.taskId = r.taskId;
    t.title = r.title;
    t.contentId = r.contentId;
    t.length = r.packageSize;
    t.iconPath = r.iconPath;
    rememberTask(t);
    addLog("Downloading " + r.title + " (task " + std::to_string(r.taskId) +
           ")");
  }
  code = 200;
  if (v2)
    return "{ \"status\": \"success\", \"data\": { \"task_id\": " +
           std::to_string(r.taskId) + ", \"title\": \"" + jsonEscape(r.title) +
           "\", \"content_id\": \"" + jsonEscape(r.contentId) +
           "\", \"package_size\": " + std::to_string(r.packageSize) + " } }\n";
  return "{ \"status\": \"success\", \"task_id\": " + std::to_string(r.taskId) +
         ", \"title\": \"" + jsonEscape(r.title) + "\" }\n";
}

std::string NetworkService::apiProgress(const std::string &body, int &code) {
  long long id;
  if (!getNumber(body, "task_id", id))
    return failMessage("No 'task_id' parameter specified.", code);
  const Bgft::Api *bgft = Platform::bgft();
  if (!bgft)
    return failMessage("BGFT unavailable", code);
  Bgft::TaskProgress p{};
  int32_t rc = bgft->getProgress((int32_t)id, &p);
  if (rc != 0)
    return failCode(rc, code);
  char b[512];
  std::snprintf(
      b, sizeof(b),
      "{ \"status\": \"success\", \"bits\": 0x%X, \"error\": %d, \"length\": "
      "0x%llX, \"transferred\": 0x%llX, "
      "\"length_total\": 0x%llX, \"transferred_total\": 0x%llX, \"num_index\": "
      "%u, \"num_total\": %u, "
      "\"rest_sec\": %u, \"rest_sec_total\": %u, \"preparing_percent\": %d, "
      "\"local_copy_percent\": %d }\n",
      p.bits, p.errorResult, (unsigned long long)p.length,
      (unsigned long long)p.transferred, (unsigned long long)p.lengthTotal,
      (unsigned long long)p.transferredTotal, p.numIndex, p.numTotal, p.restSec,
      p.restSecTotal, p.preparingPercent, p.localCopyPercent);
  code = 200;
  return b;
}

std::string NetworkService::apiTaskControl(const std::string &path,
                                           const std::string &body, int &code) {
  long long id;
  if (!getNumber(body, "task_id", id))
    return failMessage("No 'task_id' parameter specified.", code);
  const Bgft::Api *bgft = Platform::bgft();
  if (!bgft)
    return failMessage("BGFT unavailable", code);
  int32_t (*fn)(int32_t) = nullptr;
  if (path == "/api/start_task")
    fn = bgft->startTask;
  else if (path == "/api/stop_task")
    fn = bgft->stopTask;
  else if (path == "/api/pause_task")
    fn = bgft->pauseTask;
  else if (path == "/api/resume_task")
    fn = bgft->resumeTask;
  else if (path == "/api/unregister_task")
    fn = bgft->unregisterTask;
  if (!fn)
    return failMessage("Not supported on this firmware.", code);
  int32_t rc = fn((int32_t)id);
  if (rc != 0)
    return failCode(rc, code);
  {
    Lock l(&mutex_);
    for (auto &t : tasks_) {
      if (t.taskId != id)
        continue;
      if (path == "/api/pause_task")
        t.paused = true;
      if (path == "/api/resume_task" || path == "/api/start_task")
        t.paused = false;
      if (path == "/api/stop_task")
        t.stopped = true;
    }
  }
  code = 200;
  return okJson();
}

std::string NetworkService::apiFindTask(const std::string &body, int &code) {
  std::string cid;
  long long sub = 6;
  if (!getString(body, "content_id", cid))
    return failMessage("No 'content_id' parameter specified.", code);
  getNumber(body, "sub_type", sub);
  const Bgft::Api *bgft = Platform::bgft();
  if (!bgft || !bgft->findTaskByContentId)
    return failMessage("Not supported on this firmware.", code);
  int32_t id = -1;
  int32_t rc = bgft->findTaskByContentId(cid.c_str(), (int32_t)sub, &id);
  if (rc != 0)
    return failCode(rc, code);
  code = 200;
  return "{ \"status\": \"success\", \"task_id\": " + std::to_string(id) +
         " }\n";
}

std::string NetworkService::apiIsExists(const std::string &body, int &code) {
  std::string tid;
  if (!getString(body, "title_id", tid))
    return failMessage("No 'title_id' parameter specified.", code);
  const Platform::AppInstApi *ai = Platform::appInst();
  if (!ai || !ai->exists)
    return failMessage("Not supported on this firmware.", code);
  int32_t exists = 0;
  int32_t rc = ai->exists(tid.c_str(), &exists);
  if (rc != 0)
    return failCode(rc, code);
  code = 200;
  if (!exists)
    return "{ \"status\": \"success\", \"exists\": \"false\" }\n";
  uint64_t size = 0;
  if (ai->getSize)
    ai->getSize(tid.c_str(), &size);
  char b[128];
  std::snprintf(
      b, sizeof(b),
      "{ \"status\": \"success\", \"exists\": \"true\", \"size\": 0x%llX }\n",
      (unsigned long long)size);
  return b;
}

std::string NetworkService::apiUninstall(const std::string &path,
                                         const std::string &body, int &code) {
  const Platform::AppInstApi *ai = Platform::appInst();
  if (!ai)
    return failMessage("App management unavailable.", code);
  std::string id;
  int32_t rc = -1;
  if (path == "/api/uninstall_game" || path == "/api/uninstall_patch") {
    if (!getString(body, "title_id", id))
      return failMessage("No 'title_id' parameter specified.", code);
    auto fn =
        path == "/api/uninstall_game" ? ai->uninstall : ai->uninstallPatch;
    if (!fn)
      return failMessage("Not supported on this firmware.", code);
    rc = fn(id.c_str());
  } else if (path == "/api/uninstall_ac") {
    if (!getString(body, "content_id", id) || id.size() != 36)
      return failMessage("No valid 'content_id' specified.", code);
    if (!ai->uninstallAddcont)
      return failMessage("Not supported on this firmware.", code);
    rc = ai->uninstallAddcont(id.substr(7, 9).c_str(), id.substr(20).c_str());
  } else if (path == "/api/uninstall_theme") {
    if (!getString(body, "content_id", id))
      return failMessage("No 'content_id' parameter specified.", code);
    if (!ai->uninstallTheme)
      return failMessage("Not supported on this firmware.", code);
    rc = ai->uninstallTheme(id.c_str());
  } else {
    code = 404;
    return "{ \"status\": \"fail\", \"error\": \"unknown endpoint\" }\n";
  }
  if (rc != 0)
    return failCode(rc, code);
  addLog("Uninstalled " + id);
  code = 200;
  return okJson();
}

std::string NetworkService::apiSystem() {
  Platform::Storage s = Platform::queryStorage(DISK_MOUNT_POINT);
  int active = 0;
  {
    Lock l(&mutex_);
    for (auto &t : tasks_)
      if (!t.complete && !t.stopped && !t.error)
        ++active;
  }
  return "{ \"status\": \"success\", \"data\": { \"name\": \"" APP_TITLE
         "\", \"version\": \"" APP_VERSION "\", \"title_id\": \"" APP_TITLE_ID
         "\", \"ip\": \"" +
         Platform::localIp() + "\", \"port\": " + std::to_string(port_) +
         ", \"network\": true, \"storage\": { \"free\": " +
         std::to_string(s.freeBytes) +
         ", \"total\": " + std::to_string(s.totalBytes) +
         " }, \"active_jobs\": " + std::to_string(active) + " } }\n";
}

void NetworkService::refreshProgress() {
  const Bgft::Api *bgft = Platform::bgft();
  if (!bgft)
    return;
  std::vector<int> ids;
  {
    Lock l(&mutex_);
    for (auto &t : tasks_)
      if (t.taskId >= 0 && !t.complete)
        ids.push_back(t.taskId);
  }
  std::vector<std::string> bubbles;
  for (int id : ids) {
    Bgft::TaskProgress p{};
    if (bgft->getProgress(id, &p) != 0)
      continue;
    Lock l(&mutex_);
    for (auto &t : tasks_) {
      if (t.taskId != id)
        continue;
      if (p.lengthTotal)
        t.length = p.lengthTotal;
      t.transferred = p.transferredTotal;
      t.restSec = p.restSecTotal;
      t.error = p.errorResult;
      if (t.length && t.transferred >= t.length)
        t.complete = true;
      // One system bubble per transfer: finished, or failed.
      if (!t.announced && (t.complete || t.error)) {
        t.announced = true;
        std::string name =
            t.title.empty() ? t.contentId : t.title; // system font draws all
        bubbles.push_back(t.error ? name + " - download failed"
                                  : name + " - downloaded");
      }
    }
  }
  for (auto &b : bubbles)
    Platform::notify(b); // outside the lock
}

bool NetworkService::togglePause(int taskId) {
  const Bgft::Api *bgft = Platform::bgft();
  if (!bgft)
    return false;
  bool paused = false;
  {
    Lock l(&mutex_);
    for (auto &t : tasks_)
      if (t.taskId == taskId)
        paused = t.paused;
  }
  auto fn = paused ? (bgft->resumeTask ? bgft->resumeTask : bgft->startTask)
                   : bgft->pauseTask;
  if (!fn || fn(taskId) != 0)
    return false;
  Lock l(&mutex_);
  for (auto &t : tasks_)
    if (t.taskId == taskId)
      t.paused = !paused;
  return true;
}

bool NetworkService::stopTask(int taskId) {
  const Bgft::Api *bgft = Platform::bgft();
  if (!bgft || !bgft->stopTask || bgft->stopTask(taskId) != 0)
    return false;
  Lock l(&mutex_);
  for (auto &t : tasks_)
    if (t.taskId == taskId)
      t.stopped = true;
  return true;
}
