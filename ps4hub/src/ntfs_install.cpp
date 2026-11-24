#include "ntfs_install.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "config.h"
#include "network_service.h"
#include "ntfs_manager.h"
#include "pkg_installer.h"
#include "platform.h"

NtfsInstall &NtfsInstall::instance() {
  static NtfsInstall n;
  return n;
}

bool NtfsInstall::busy() const { return running_.load(); }

NtfsInstall::Status NtfsInstall::status() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return st_;
}

bool NtfsInstall::start(const std::string &ntfsSourcePath,
                        const std::string &fileName, int userId) {
  if (running_.exchange(true))
    return false;

  {
    std::lock_guard<std::mutex> lk(mutex_);
    st_ = Status{};
    st_.phase = Phase::Copying;
    st_.fileName = fileName;
    st_.message = "Copying " + fileName + "...";
  }

  struct Args {
    NtfsInstall *self;
    std::string src;
    std::string name;
    int userId;
  };
  auto *a = new Args{this, ntfsSourcePath, fileName, userId};

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 512 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int rc = pthread_create(&thread_, &attr, &NtfsInstall::entry, a);
  pthread_attr_destroy(&attr);

  if (rc != 0) {
    delete a;
    running_ = false;
    std::lock_guard<std::mutex> lk(mutex_);
    st_.phase = Phase::Failed;
    st_.message = "Cannot start background thread";
    return false;
  }
  return true;
}

void *NtfsInstall::entry(void *vargs) {
  struct Args {
    NtfsInstall *self;
    std::string src;
    std::string name;
    int userId;
  };
  auto *a = static_cast<Args *>(vargs);
  try {
    a->self->run(a->src, a->name, a->userId);
  } catch (const std::exception &e) {
    std::lock_guard<std::mutex> lk(a->self->mutex_);
    a->self->st_.phase = Phase::Failed;
    a->self->st_.message = std::string("Exception: ") + e.what();
  } catch (...) {
    std::lock_guard<std::mutex> lk(a->self->mutex_);
    a->self->st_.phase = Phase::Failed;
    a->self->st_.message = "Unknown exception";
  }
  a->self->running_ = false;
  delete a;
  return nullptr;
}

void NtfsInstall::run(std::string src, std::string name, int userId) {
  auto &ntfs = NtfsManager::instance();

  // Prepare destination with a sanitised name (isServableName only allows
  // alnum/_/-/.; otherwise /static/... would 404 and BGFT would reject).
  mkdir(APP_WORK_DIR, 0777);
  std::string safeName;
  for (char c : name) {
    safeName +=
        (std::isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')
            ? c
            : '_';
  }
  if (safeName.size() > 60)
    safeName = safeName.substr(safeName.size() - 60);
  std::string dst = std::string(APP_WORK_DIR) + "/" + safeName;

  int64_t total = ntfs.fileSize(src);
  if (total <= 0) {
    std::lock_guard<std::mutex> lk(mutex_);
    st_.phase = Phase::Failed;
    st_.message = "Cannot read source size";
    return;
  }
  {
    std::lock_guard<std::mutex> lk(mutex_);
    st_.totalBytes = (uint64_t)total;
  }

  FILE *out = std::fopen(dst.c_str(), "wb");
  if (!out) {
    std::lock_guard<std::mutex> lk(mutex_);
    st_.phase = Phase::Failed;
    st_.message = "Cannot create destination file";
    return;
  }

  const uint64_t chunk = 1 << 20; // 1 MiB
  std::vector<char> buf(chunk);
  uint64_t off = 0;
  bool ok = true;
  while ((int64_t)off < total) {
    uint64_t want = (uint64_t)total - off;
    if (want > chunk)
      want = chunk;
    int64_t got = ntfs.readFile(src, off, buf.data(), want);
    if (got <= 0) {
      ok = false;
      break;
    }
    if (std::fwrite(buf.data(), 1, (size_t)got, out) != (size_t)got) {
      ok = false;
      break;
    }
    off += (uint64_t)got;
    std::lock_guard<std::mutex> lk(mutex_);
    st_.bytesCopied = off;
  }
  std::fclose(out);

  if (!ok) {
    unlink(dst.c_str());
    std::lock_guard<std::mutex> lk(mutex_);
    st_.phase = Phase::Failed;
    st_.message =
        "Copy failed at " + std::to_string(off) + " / " + std::to_string(total);
    return;
  }

  // Phase 2: install via PkgInstaller::installFromFile (same path App
  // Catalog uses - sceAppInstUtilAppInstallPkg under the hood).
  {
    std::lock_guard<std::mutex> lk(mutex_);
    st_.phase = Phase::Installing;
    st_.message = "Installing " + name + "...";
  }

  // Full BGFT path (identical to Remote Install / App Catalog). Writes a
  // manifest, registers a BGFT download task pointing at /static/<name>,
  // and starts it. This is what makes fake PKGs / patches / DLCs show up
  // in the shell correctly - sceAppInstUtilAppInstallPkg alone installs
  // the files but can leave them unplayable (CE-30012-6 on launch).
  std::string ip = Platform::localIp();
  if (ip.empty())
    ip = "127.0.0.1";
  int port = NetworkService::instance().port();
  std::string serveUrl =
      "http://" + ip + ":" + std::to_string(port) + "/static/" + safeName;

  InstallResult ir;
  bool installed =
      PkgInstaller::installDirect({serveUrl}, ip, port, userId, ir);

  std::lock_guard<std::mutex> lk(mutex_);
  if (installed) {
    st_.phase = Phase::Done;
    if (ir.alreadyInstalled)
      st_.message = name + " is already installed";
    else
      st_.message = "Install queued: " + name;
  } else {
    st_.phase = Phase::Failed;
    st_.message = "Install failed: " +
                  (ir.error.empty() ? std::string("unknown") : ir.error);
  }
}
