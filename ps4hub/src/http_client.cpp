#include "http_client.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Url {
  std::string host, path = "/";
  uint16_t port = 80;
};

bool parseUrl(const std::string &url, Url &u, std::string &err) {
  if (url.compare(0, 7, "http://") != 0) {
    err = "only http:// URLs are supported";
    return false;
  }
  std::string rest = url.substr(7);
  size_t slash = rest.find('/');
  std::string hostPort = rest.substr(0, slash);
  if (slash != std::string::npos)
    u.path = rest.substr(slash);
  size_t colon = hostPort.rfind(':');
  u.host = colon == std::string::npos ? hostPort : hostPort.substr(0, colon);
  if (colon != std::string::npos) {
    long p = std::strtol(hostPort.c_str() + colon + 1, nullptr, 10);
    if (p <= 0 || p > 65535) {
      err = "invalid port in URL";
      return false;
    }
    u.port = (uint16_t)p;
  }
  if (u.host.empty()) {
    err = "missing host in URL";
    return false;
  }
  return true;
}

int connectTo(const Url &u, std::string &err) {
  // Numeric IPv4 only: name resolution in this libc goes through libSceNet,
  // which the app does not load.
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(u.port);
  if (inet_pton(AF_INET, u.host.c_str(), &dst.sin_addr) != 1) {
    err = "use a numeric IPv4 address in the URL (got " + u.host + ")";
    return -1;
  }
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    err = "socket() failed";
    return -1;
  }
  timeval tv{};
  tv.tv_sec = 15;
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#ifdef SO_NOSIGPIPE
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
  if (connect(fd, (sockaddr *)&dst, sizeof(dst)) != 0) {
    close(fd);
    err = "cannot connect to " + u.host + ":" + std::to_string(u.port);
    return -1;
  }
  return fd;
}

bool sendAll(int fd, const std::string &s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = send(fd, s.data() + off, s.size() - off, 0);
    if (n <= 0)
      return false;
    off += (size_t)n;
  }
  return true;
}

std::string lower(std::string s) {
  for (auto &c : s)
    c = (char)std::tolower((unsigned char)c);
  return s;
}

// Sends the request and parses the response headers. `rest` receives any body
// bytes that arrived with the headers.
bool request(const std::string &url, const std::string &range, int &fd,
             int &status, std::string &headers, std::string &rest,
             std::string &err) {
  Url u;
  if (!parseUrl(url, u, err))
    return false;
  fd = connectTo(u, err);
  if (fd < 0)
    return false;
  std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" +
                    std::to_string(u.port) +
                    "\r\nUser-Agent: PS4Hub/1.0\r\nAccept-Encoding: "
                    "identity\r\nConnection: close\r\n";
  if (!range.empty())
    req += "Range: bytes=" + range + "\r\n";
  req += "\r\n";
  if (!sendAll(fd, req)) {
    close(fd);
    err = "send failed";
    return false;
  }

  std::string buf;
  char tmp[4096];
  size_t he;
  while ((he = buf.find("\r\n\r\n")) == std::string::npos) {
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
    if (n <= 0 || buf.size() > 64 * 1024) {
      close(fd);
      err = "no HTTP response";
      return false;
    }
    buf.append(tmp, (size_t)n);
  }
  headers = lower(buf.substr(0, he));
  rest = buf.substr(he + 4);
  status = 0;
  std::sscanf(headers.c_str(), "http/%*d.%*d %d", &status);
  return true;
}

uint64_t headerNumber(const std::string &h, const char *name) {
  size_t p = h.find(name);
  return p == std::string::npos
             ? 0
             : std::strtoull(h.c_str() + p + std::strlen(name), nullptr, 10);
}

uint64_t contentRangeTotal(const std::string &h) {
  size_t p = h.find("\r\ncontent-range:");
  if (p == std::string::npos)
    return 0;
  size_t slash = h.find('/', p);
  size_t eol = h.find("\r\n", p + 2);
  if (slash == std::string::npos || (eol != std::string::npos && slash > eol))
    return 0;
  return std::strtoull(h.c_str() + slash + 1, nullptr, 10);
}

} // namespace

namespace Http {

bool get(const std::string &url, uint64_t offset, uint64_t length,
         HttpResult &out, std::string &err, size_t maxBody) {
  out = HttpResult{};
  int fd = -1;
  std::string headers, body;
  std::string range;
  if (length)
    range = std::to_string(offset) + "-" + std::to_string(offset + length - 1);
  if (!request(url, range, fd, out.status, headers, body, err))
    return false;

  if (out.status != 200 && out.status != 206) {
    close(fd);
    err = "HTTP " + std::to_string(out.status);
    return false;
  }
  if (headers.find("transfer-encoding: chunked") != std::string::npos) {
    close(fd);
    err = "chunked responses are not supported";
    return false;
  }
  out.partial = out.status == 206;
  uint64_t contentLength = headerNumber(headers, "\r\ncontent-length:");
  out.totalSize = out.partial ? contentRangeTotal(headers) : contentLength;

  uint64_t want = contentLength ? contentLength : (length ? length : maxBody);
  if (length && !out.partial) {
    // Server ignored Range and is sending the whole file.
    if (offset != 0) {
      close(fd);
      err = "the web server does not support Range requests";
      return false;
    }
    if (want > length)
      want = length;
  }
  if (want > maxBody)
    want = maxBody;
  char tmp[16 * 1024];
  while (body.size() < want) {
    ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
    if (n <= 0)
      break;
    body.append(tmp, (size_t)n);
  }
  close(fd);
  if (body.size() > want)
    body.resize(want);
  out.body.swap(body);
  return true;
}

bool probe(const std::string &url, uint64_t &size, bool &ranges,
           std::string &err) {
  int fd = -1, status = 0;
  std::string headers, rest;
  if (!request(url, "0-0", fd, status, headers, rest, err))
    return false;
  close(fd); // body not needed
  if (status == 206) {
    ranges = true;
    size = contentRangeTotal(headers);
  } else if (status == 200) {
    ranges = false;
    size = headerNumber(headers, "\r\ncontent-length:");
  } else {
    err = "HTTP " + std::to_string(status);
    return false;
  }
  if (!size) {
    err = "server did not report the file size";
    return false;
  }
  return true;
}

} // namespace Http
