#pragma once
#include <cstdint>
#include <string>

struct HttpResult {
  int status = 0;
  bool partial = false;
  uint64_t totalSize = 0; // from Content-Range, or Content-Length on 200
  std::string body;
};

namespace Http {

// GET `length` bytes at `offset` (length 0 = whole body, capped at maxBody).
bool get(const std::string &url, uint64_t offset, uint64_t length,
         HttpResult &out, std::string &err, size_t maxBody = 16u << 20);

// Size of the resource and whether the server honours Range requests.
bool probe(const std::string &url, uint64_t &size, bool &ranges,
           std::string &err);

} // namespace Http
