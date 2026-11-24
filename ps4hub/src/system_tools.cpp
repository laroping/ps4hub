#include "system_tools.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <exception>

#include "catalog_db.h"
#include "config.h"
#include "libc_compat.h"
#include "network_service.h"
#include "platform.h"

namespace {

struct Lock {
  pthread_mutex_t *m;
  explicit Lock(pthread_mutex_t *mm) : m(mm) { pthread_mutex_lock(m); }
  ~Lock() { pthread_mutex_unlock(m); }
};

const char *kUpdatePaths[] = {"/update/PS4UPDATE.PUP",
                              "/update/PS4UPDATE.PUP.net.temp"};

enum class Kind { Missing, Dir, Other };
Kind kindOf(const char *path) {
  struct stat st;
  if (Compat::stat(path, &st) != 0)
    return Kind::Missing;
  return S_ISDIR(st.st_mode) ? Kind::Dir : Kind::Other;
}

bool dirEmpty(const char *path) {
  DIR *d = opendir(path);
  if (!d)
    return false;
  bool empty = true;
  while (struct dirent *e = readdir(d))
    if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) {
      empty = false;
      break;
    }
  closedir(d);
  return empty;
}

// Non-blocking connect to 127.0.0.1:port with a short timeout.
bool portListening(int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  bool ok = false;
  if (connect(fd, (sockaddr *)&a, sizeof(a)) == 0) {
    ok = true;
  } else if (errno == EINPROGRESS) {
    pollfd p{fd, POLLOUT, 0};
    if (poll(&p, 1, 150) == 1 && (p.revents & POLLOUT)) {
      int soErr = 0;
      socklen_t len = sizeof(soErr);
      ok =
          getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &len) == 0 && soErr == 0;
    }
  }
  close(fd);
  return ok;
}

bool hasAppFiles(const std::string &dir) {
  struct stat st;
  return Compat::stat((dir + "/app.pkg").c_str(), &st) == 0 ||
         Compat::stat((dir + "/sce_sys/param.sfo").c_str(), &st) == 0;
}

// Full path to the on-disk pkg of an installed app, empty when not found.
std::string pkgPathFor(const std::string &titleId) {
  for (const char *root : {"/user/app", "/mnt/ext0/user/app"}) {
    std::string candidate = std::string(root) + "/" + titleId + "/app.pkg";
    struct stat st;
    if (Compat::stat(candidate.c_str(), &st) == 0 && st.st_size > 0)
      return candidate;
  }
  return "";
}

} // namespace

namespace UpdateBlocker {

State state() {
  int dirs = 0;
  for (const char *p : kUpdatePaths)
    if (kindOf(p) == Kind::Dir)
      ++dirs;
  return dirs == 2 ? State::On : (dirs == 0 ? State::Off : State::Partial);
}

bool enable(std::string &err) {
  if (kindOf("/update") != Kind::Dir) {
    err = "/update is not accessible";
    return false;
  }
  for (const char *p : kUpdatePaths) {
    Kind k = kindOf(p);
    if (k == Kind::Dir)
      continue;
    if (k == Kind::Other && unlink(p) != 0) {
      err = std::string("cannot remove ") + p;
      return false;
    }
    if (mkdir(p, 0555) != 0 && errno != EEXIST) {
      err = std::string("cannot create ") + p;
      return false;
    }
  }
  return state() == State::On ||
         (err = "blocker directories could not be verified", false);
}

bool disable(std::string &err) {
  for (const char *p : kUpdatePaths) {
    if (kindOf(p) != Kind::Dir)
      continue;
    if (!dirEmpty(p)) {
      err = std::string(p) + " is not empty; left in place";
      return false;
    }
    if (rmdir(p) != 0) {
      err = std::string("cannot remove ") + p;
      return false;
    }
  }
  return true;
}

} // namespace UpdateBlocker

namespace Services {

std::vector<ServiceInfo> scan() {
  struct Known {
    const char *name;
    int port;
  };
  static const Known kKnown[] = {
      {"FTP server", 2121},     {"Kernel log (klog)", 3232},
      {"Payload loader", 9090}, {"Payload loader (alt)", 9020},
      {"ELF loader", 9021},
  };
  std::vector<ServiceInfo> out;
  auto &svc = NetworkService::instance();
  ServiceInfo own;
  own.name = "Remote install (" APP_TITLE ")";
  own.port = svc.running() ? svc.port() : SERVER_PORT;
  own.listening = svc.running();
  own.ours = true;
  own.stoppable = true;
  out.push_back(own);

  for (const auto &k : kKnown) {
    ServiceInfo s;
    s.name = k.name;
    s.port = k.port;
    s.listening = portListening(k.port);
    if (s.listening && PortOwner::find((uint16_t)k.port, s.owner))
      s.stoppable = !PortOwner::isProtected(s.owner);
    out.push_back(s);
  }
  return out;
}

bool stop(const ServiceInfo &s, std::string &err) {
  if (s.ours) {
    NetworkService::instance().stop();
    return true;
  }
  if (!s.stoppable) {
    err =
        "This service belongs to the system or GoldHEN and is not stopped here";
    return false;
  }
  if (!PortOwner::terminate(s.owner)) {
    err = "Could not stop " + PortOwner::describe(s.owner);
    return false;
  }
  return true;
}

} // namespace Services

FpkgRebuilder &FpkgRebuilder::instance() {
  static FpkgRebuilder r;
  return r;
}

std::vector<std::string>
FpkgRebuilder::findMissing(const std::set<std::string> &known) {
  std::vector<std::string> missing;
  for (const char *root : {"/user/app", "/mnt/ext0/user/app"}) {
    DIR *d = opendir(root);
    if (!d)
      continue;
    while (struct dirent *e = readdir(d)) {
      std::string id = e->d_name;
      if (!CatalogDb::isPlainTitleId(id) || id.rfind("NPXS", 0) == 0 ||
          id == APP_TITLE_ID)
        continue;
      if (known.count(id))
        continue;
      if (!hasAppFiles(std::string(root) + "/" + id))
        continue;
      if (std::find(missing.begin(), missing.end(), id) == missing.end())
        missing.push_back(id);
    }
    closedir(d);
  }
  return missing;
}

void FpkgRebuilder::start(const std::set<std::string> &known) {
  if (state_ == State::Running)
    return;
  {
    Lock l(&mutex_);
    known_ = known;
    summary_.clear();
    restored_ = 0;
  }
  state_ = State::Running;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t t;
  if (pthread_create(&t, &attr, &FpkgRebuilder::entry, this) != 0)
    state_ = State::Idle;
  pthread_attr_destroy(&attr);
}

void *FpkgRebuilder::entry(void *self) {
  auto *r = static_cast<FpkgRebuilder *>(self);
  std::set<std::string> known;
  {
    Lock l(&r->mutex_);
    known = r->known_;
  }

  std::vector<std::string> missing;
  int ok = 0, failed = 0;
  std::string firstError;
  try {
    missing = findMissing(known);
    const Platform::AppInstApi *ai = Platform::appInst();
    bool haveInstall = ai && ai->installPkg;
    bool haveRecover = ai && ai->recoverApp;
    if (!missing.empty() && !haveInstall && !haveRecover) {
      failed = (int)missing.size();
      firstError = "app recovery is not available on this firmware";
    } else {
      for (const auto &id : missing) {
        int32_t rc = -1;

        // Preferred: reinstall the PKG that's already on disk.
        // sceAppInstUtilAppInstallPkg works for homebrew and rebuilds
        // the app.db row on its own. Falls back to recoverApp only if
        // we can't find the PKG file.
        if (haveInstall) {
          std::string pkg = pkgPathFor(id);
          if (!pkg.empty())
            rc = ai->installPkg(pkg.c_str(), nullptr);
        }
        if (rc != 0 && haveRecover)
          rc = ai->recoverApp(id.c_str());

        if (rc == 0) {
          ++ok;
          continue;
        }
        ++failed;
        if (firstError.empty()) {
          char b[80];
          std::snprintf(b, sizeof(b), "%s: 0x%08X", id.c_str(), (unsigned)rc);
          firstError = b;
        }
      }
    }
  } catch (const std::exception &e) {
    firstError = std::string("Exception: ") + e.what();
    failed = (int)missing.size();
  } catch (...) {
    firstError = "Unknown exception";
    failed = (int)missing.size();
  }

  std::string sum;
  if (missing.empty())
    sum = "No missing games found";
  else {
    sum = "Restored " + std::to_string(ok) + " of " +
          std::to_string(missing.size()) + " missing games";
    if (failed)
      sum += " (" + firstError + ")";
  }
  {
    Lock l(&r->mutex_);
    r->summary_ = sum;
    r->restored_ = ok;
  }
  ++r->serial_;
  r->state_ = State::Done;
  return nullptr;
}

std::string FpkgRebuilder::summary() {
  Lock l(&mutex_);
  return summary_;
}

int FpkgRebuilder::restoredCount() {
  Lock l(&mutex_);
  return restored_;
}
