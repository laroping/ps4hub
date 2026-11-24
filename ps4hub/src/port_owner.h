#pragma once
#include <cstdint>
#include <string>

struct PortOwnerInfo {
  int pid = -1;
  std::string name;    // process name, e.g. "eboot.bin"
  std::string titleId; // e.g. "FLTZ00003"
};

namespace PortOwner {

// Scans process file tables for an IPv4 TCP socket bound to `port`.
bool find(uint16_t port, PortOwnerInfo &out);

// True for processes we must never kill: system services, NPXS system apps,
// anything we cannot identify, and ourselves.
bool isProtected(const PortOwnerInfo &p);

// SIGKILLs the process (only if !isProtected). Returns true if it was sent.
bool terminate(const PortOwnerInfo &p);

std::string describe(const PortOwnerInfo &p);

bool findProcess(const char *name, PortOwnerInfo &out);

} // namespace PortOwner
