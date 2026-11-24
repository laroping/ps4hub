#include "title_inspector.h"
#include "platform.h"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <exception>

#include "catalog_db.h"
#include "libc_compat.h"
#include "sfo_parser.h"

namespace {

struct Lock {
  pthread_mutex_t *m;
  explicit Lock(pthread_mutex_t *mm) : m(mm) { pthread_mutex_lock(m); }
  ~Lock() { pthread_mutex_unlock(m); }
};

std::string sdkFromPubtool(const std::string &pubtool) {
  // PUBTOOLINFO: "c_date=20190101,sdk_ver=05050001,st_type=digital50,..."
  size_t p = pubtool.find("sdk_ver=");
  if (p == std::string::npos || p + 8 + 4 > pubtool.size())
    return "";
  std::string v = pubtool.substr(p + 8, 4); // "0505"
  if (v.size() < 4)
    return "";
  std::string major = v.substr(0, 2);
  if (major[0] == '0')
    major.erase(0, 1);
  return major + "." + v.substr(2, 2);
}

} // namespace

namespace Fs {

uint64_t treeSize(const std::string &path) {
  DIR *d = opendir(path.c_str());
  if (!d)
    return 0;
  uint64_t total = 0;
  while (struct dirent *e = readdir(d)) {
    if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
      continue;
    std::string child = path + "/" + e->d_name;
    if (e->d_type == DT_DIR) {
      total += treeSize(child);
      continue;
    }
    if (e->d_type == DT_LNK)
      continue;
    struct stat st;
    if (Compat::stat(child.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode))
        total += treeSize(child);
      else if (S_ISREG(st.st_mode))
        total += (uint64_t)st.st_size;
    }
  }
  closedir(d);
  return total;
}

} // namespace Fs

TitleInspector &TitleInspector::instance() {
  static TitleInspector t;
  return t;
}

void TitleInspector::start() {
  if (started_)
    return;
  running_ = true;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  started_ = pthread_create(&thread_, &attr, &TitleInspector::entry, this) == 0;
  pthread_attr_destroy(&attr);
  if (!started_)
    running_ = false;
}

void TitleInspector::stop() {
  if (!started_)
    return;
  {
    Lock l(&mutex_);
    running_ = false;
    pthread_cond_signal(&cond_);
  }
  pthread_join(thread_, nullptr);
  started_ = false;
}

TitleInfo TitleInspector::get(const std::string &titleId) {
  Lock l(&mutex_);
  auto it = cache_.find(titleId);
  if (it != cache_.end())
    return it->second;
  TitleInfo pending;
  pending.titleId = titleId;
  if (!CatalogDb::isPlainTitleId(titleId)) { // folders etc.: nothing to inspect
    pending.ready = true;
    cache_[titleId] = pending;
    return pending;
  }
  cache_[titleId] = pending;
  queue_.push_front(titleId); // the focused title goes first
  pthread_cond_signal(&cond_);
  return pending;
}

void TitleInspector::invalidate(const std::string &titleId) {
  Lock l(&mutex_);
  cache_.erase(titleId);
}

void *TitleInspector::entry(void *self) {
  try {
    static_cast<TitleInspector *>(self)->worker();
  } catch (const std::exception &e) {
    std::printf("[PS4Hub] TitleInspector exception: %s\n", e.what());
  } catch (...) {
    std::printf("[PS4Hub] TitleInspector exception (unknown)\n");
  }
  return nullptr;
}

void TitleInspector::worker() {
  for (;;) {
    std::string id;
    {
      Lock l(&mutex_);
      while (running_ && queue_.empty())
        pthread_cond_wait(&cond_, &mutex_);
      if (!running_)
        return;
      id = queue_.front();
      queue_.pop_front();
    }
    TitleInfo info = inspect(id);
    Lock l(&mutex_);
    cache_[id] = info;
  }
}

TitleInfo TitleInspector::inspect(const std::string &id) {
  TitleInfo info;
  info.titleId = id;

  SfoFile base, patch;
  bool hasBase = Sfo::loadInstalled(id, false, base);
  bool hasPatch = Sfo::loadInstalled(id, true, patch);
  info.hasSfo = hasBase || hasPatch;
  const SfoFile &main = hasBase ? base : patch;
  if (info.hasSfo) {
    info.title = main.str("TITLE");
    info.category = main.str("CATEGORY");
    // Installed version: the update's APP_VER when there is one.
    info.appVer = hasPatch && !patch.str("APP_VER").empty()
                      ? patch.str("APP_VER")
                      : main.str("APP_VER");
    info.version = hasBase ? base.str("APP_VER") : std::string();
    if (info.version == info.appVer)
      info.version.clear();
    uint32_t sys = 0;
    if (main.num("SYSTEM_VER", sys))
      info.requiredFw = Sfo::firmwareString(sys);
    info.sdkVer = sdkFromPubtool(main.str("PUBTOOLINFO"));
  }

  info.baseBytes = Fs::treeSize("/user/app/" + id);
  info.patchBytes = Fs::treeSize("/user/patch/" + id);
  info.addonBytes = Fs::treeSize("/user/addcont/" + id);
  info.ready = true;
  return info;
}

namespace Addons {

std::vector<AddonEntry> list(const std::string &id) {
  std::vector<AddonEntry> out;
  if (!CatalogDb::isPlainTitleId(id))
    return out;
  for (const std::string &root :
       {std::string("/user"), std::string("/mnt/ext0/user")}) {
    std::string pdir = root + "/patch/" + id;
    uint64_t pbytes = Fs::treeSize(pdir);
    if (pbytes) {
      AddonEntry e;
      e.isPatch = true;
      e.name = "Update";
      SfoFile sfo;
      if (Sfo::loadInstalled(id, true, sfo))
        e.version = sfo.str("APP_VER");
      e.bytes = pbytes;
      out.push_back(e);
    }
    std::string adir = root + "/addcont/" + id;
    if (DIR *d = opendir(adir.c_str())) {
      while (struct dirent *de = readdir(d)) {
        std::string label = de->d_name;
        if (label == "." || label == "..")
          continue;
        std::string dir = adir + "/" + label;
        AddonEntry e;
        e.label = label;
        e.bytes = Fs::treeSize(dir);
        SfoFile sfo;
        if (sfo.loadFromPkg(dir + "/ac.pkg") ||
            sfo.load(dir + "/sce_sys/param.sfo")) {
          e.name = Sfo::cleanTitle(
              sfo.str("TITLE")); // "" for e.g. Japanese-only names
          e.version = sfo.str("APP_VER");
        }
        if (e.name.empty())
          e.name = "Add-on " + label;
        out.push_back(e);
      }
      closedir(d);
    }
  }
  return out;
}

// Entitlement labels are exactly 16 characters of A-Z, a-z, 0-9.
bool validLabel(const std::string &l) {
  if (l.size() != 16)
    return false;
  for (char c : l)
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9')))
      return false;
  return true;
}

// Removal goes only through the system installer (AppInstUtil), which also
// updates its own databases (app.db, addcont.db). PS4Hub never deletes the
// files or edits those databases itself - doing that is what leaves
// "corrupted data" entries behind.
bool uninstall(const std::string &id, const AddonEntry &e, std::string &err) {
  if (!CatalogDb::isPlainTitleId(id) || id.rfind("NPXS", 0) == 0) {
    err = "invalid title id " + id;
    return false;
  }
  if (!e.isPatch && !validLabel(e.label)) {
    err = "invalid add-on label " + e.label;
    return false;
  }
  bool present = false;
  for (const AddonEntry &x : list(id))
    if (x.isPatch == e.isPatch && (e.isPatch || x.label == e.label))
      present = true;
  if (!present) {
    err = "not installed any more";
    return false;
  }

  const Platform::AppInstApi *ai = Platform::appInst();
  if (!ai) {
    err = "App management is not available";
    return false;
  }
  int32_t rc;
  if (e.isPatch) {
    if (!ai->uninstallPatch) {
      err = "Not supported on this firmware";
      return false;
    }
    rc = ai->uninstallPatch(id.c_str());
  } else {
    if (!ai->uninstallAddcont) {
      err = "Not supported on this firmware";
      return false;
    }
    rc = ai->uninstallAddcont(id.c_str(), e.label.c_str());
  }
  if (rc != 0) {
    char b[24];
    std::snprintf(b, sizeof(b), "error 0x%08X", (uint32_t)rc);
    err = b;
    return false;
  }
  TitleInspector::instance().invalidate(id);
  for (const AddonEntry &x : list(id))
    if (x.isPatch == e.isPatch && (e.isPatch || x.label == e.label)) {
      err = "the system reported success but the files are still there";
      return false;
    }
  return true;
}

} // namespace Addons
