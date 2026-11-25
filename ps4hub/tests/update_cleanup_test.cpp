#include "update_cleanup.h"
#include "sfo_parser.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
namespace fs = std::filesystem;
int main(int argc, char **argv) {
  assert(argc == 3);
  const fs::path fixture = fs::absolute(argv[1]);
  const fs::path dir = fs::absolute(argv[2]);
  assert(!fs::exists(dir));
  fs::create_directories(dir);
  SfoFile source;
  assert(source.loadFromPkg(fixture.string()));
  assert(source.str("TITLE_ID") == "CPAN00001");
  assert(source.str("APP_VER") == "1.29");
  std::ifstream input(fixture, std::ios::binary);
  const std::string original((std::istreambuf_iterator<char>(input)), {});
  auto write = [&](const char *filename, const char *version, const char *title = "CPAN00001") {
    std::string bytes = original;
    for (size_t p = 0; (p = bytes.find("1.29", p)) != std::string::npos; p += 4)
      bytes.replace(p, 4, version);
    for (size_t p = 0; (p = bytes.find("CPAN00001", p)) != std::string::npos; p += 9)
      bytes.replace(p, 9, title);
    std::ofstream out(dir / filename, std::ios::binary);
    out.write(bytes.data(), bytes.size());
  };
  auto clean = [&](const char *version) {
    return UpdateCleanup::removeInstalledPackages(dir.string(), version, "CPAN00001");
  };
  write("PS4Hub-v1.27.pkg", "1.27");
  write("PS4Hub-v1.28.pkg", "1.28");
  write("PS4Hub-v1.29.pkg", "1.29");
  write("PS4Hub-v1.30.pkg", "1.30");
  write("PS4Hub-v2.00.pkg", "2.00");
  write("other.pkg", "1.27");
  write("PS4Hub-v1.27.pkg.part", "1.27");
  write("PS4Hub-v1.26.pkg", "1.29"); // Mismatched filename and SFO.
  write("PS4Hub-v1.25.pkg", "1.25", "OTHR00001");
  { std::ofstream out(dir / "PS4Hub-v1.24.pkg"); out << "invalid"; }
  fs::create_directories(dir / "nested");
  fs::copy_file(fixture, dir / "nested/PS4Hub-v1.29.pkg");
  assert(clean("../1.29") == 0);
  assert(clean("1..29") == 0);
  assert(clean("1.28") == 2);
  assert(!fs::exists(dir / "PS4Hub-v1.27.pkg"));
  assert(!fs::exists(dir / "PS4Hub-v1.28.pkg"));
  assert(fs::exists(dir / "PS4Hub-v1.29.pkg"));
  assert(clean("1.29") == 1);
  assert(!fs::exists(dir / "PS4Hub-v1.29.pkg"));
  assert(fs::exists(dir / "PS4Hub-v1.30.pkg"));
  assert(fs::exists(dir / "PS4Hub-v2.00.pkg"));
  assert(clean("1.29") == 0);
  assert(clean("1.30") == 1);
  assert(clean("2.0") == 1); // Numeric comparison, not lexicographic.
  for (const char *name : {"other.pkg", "PS4Hub-v1.27.pkg.part", "PS4Hub-v1.26.pkg",
                           "PS4Hub-v1.25.pkg", "PS4Hub-v1.24.pkg", "nested/PS4Hub-v1.29.pkg"})
    assert(fs::exists(dir / name));
  assert(UpdateCleanup::removeInstalledPackages((dir / "missing").string(), "1.29", "CPAN00001") == 0);
  std::cout << "PASS: old/current removed; newer, foreign, mismatched, corrupt, partial, "
               "unrelated and nested packages preserved; numeric version comparison verified.\n";
}
