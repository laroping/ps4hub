#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

struct NtfsEntry {
  std::string name;
  bool isDir = false;
  uint64_t size = 0;
};

struct NtfsDeviceInfo {
  std::string path;        // /dev/da0s1 or /mnt/usb0
  std::string label;       // human-friendly label for the picker
  bool shellMount = false; // true: already mounted by shell, no ntfs-3g
  uint64_t totalSize = 0;  // best-effort size (0 when unknown)
};

class NtfsManager {
public:
  static NtfsManager &instance();

  bool mounted() const;
  // True when the currently mounted volume is the PS4 shell's exFAT/FAT
  // auto-mount (read-only, incompatible with fake PKG install flow).
  bool isShellMount() const;
  bool mount();
  bool mountDevice(const NtfsDeviceInfo &dev);
  void unmount();

  std::vector<NtfsDeviceInfo> scan();

  std::string devicePath() const;
  std::string currentDir() const;

  bool listDir(const std::string &path, std::vector<NtfsEntry> &out);
  bool navigate(const std::string &dir);
  bool navigateUp();
  const std::vector<NtfsEntry> &entries() const { return entries_; }
  void refresh();

  int64_t readFile(const std::string &path, uint64_t offset, void *buf,
                   uint64_t len);
  int64_t fileSize(const std::string &path);

private:
  NtfsManager() = default;

  bool detectDevice();
  bool detectShellMount(); // PS4 shell auto-mount (/mnt/usb*, /mnt/ext*)

  mutable std::mutex mutex_;
  void *vol_ = nullptr;   // ntfs_volume* when ntfs-3g owns the mount
  std::string devPath_;   // display name ("/dev/da0s1" or "/mnt/usb0")
  std::string rootPath_;  // real FS root when using shell mount
  bool useShell_ = false; // true: read via plain POSIX from rootPath_
  std::string curDir_ = "/";
  std::vector<NtfsEntry> entries_;
};
