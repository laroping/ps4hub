#pragma once
#include <string>
#include <vector>

namespace NetProfiles {

struct Profile {
  std::string file;
  std::string name;    // "LAN 192.168.137.2"
  std::string summary; // "IP 192.168.137.2, gateway ..., DNS ..."
};

bool available(std::string &err);
bool saveCurrent(std::string &savedName, std::string &err);
std::vector<Profile> list();
// Writes a profile into the registry; it takes effect after Settings >
// Network > Test Internet Connection.
bool apply(const Profile &p, std::string &err);
bool remove(const Profile &p, std::string &err);
std::string currentSummary();

} // namespace NetProfiles
