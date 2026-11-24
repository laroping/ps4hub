#include "webkit_browser.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <vector>

#include <sqlite3.h>

#include "config.h"
#include "gh_plugins.h" // Jobs, writeFileInPlace, mkdirs
#include "libc_compat.h"
#include "platform.h"

static const char *kDbPath = "/system_data/priv/mms/app.db";
static const char *kBackupDir = APP_WORK_DIR "/backup_browser";
static const char *kStateFile =
    APP_WORK_DIR "/backup_browser/browser_state.json";
static const char *kIconBackup =
    APP_WORK_DIR "/backup_browser/icon0_backup.png";

// Only the PS4 Internet Browser (NPXS20102).  User's Guide was tried earlier
// but it is not a real browser and cannot be made to open arbitrary URLs.
static const char *kBrowserCandidates[] = {"NPXS20102", nullptr};

namespace {

struct WbHandle {
  sqlite3 *db = nullptr;
  ~WbHandle() {
    if (db) {
      sqlite3_exec(db, "PRAGMA wal_checkpoint(FULL);", nullptr, nullptr,
                   nullptr);
      sqlite3_close(db);
    }
  }
};

struct WbStmt {
  sqlite3_stmt *s = nullptr;
  ~WbStmt() {
    if (s)
      sqlite3_finalize(s);
  }
};

extern "C" {
static int statFixed_wb(const char *path, struct stat *st) {
  return Compat::stat(path, st);
}
static int fstatFixed_wb(int fd, struct stat *st) {
  return Compat::fstat(fd, st);
}

struct KernelFlockWb {
  int64_t l_start;
  int64_t l_len;
  int32_t l_pid;
  int16_t l_type;
  int16_t l_whence;
  int32_t l_sysid;
};
static int16_t toKL(short t) {
  if (t == F_RDLCK)
    return 1;
  if (t == F_WRLCK)
    return 3;
  return 2;
}
static int fcntlWb(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  if (cmd != F_GETLK && cmd != F_SETLK && cmd != F_SETLKW)
    return fcntl(fd, cmd, arg);
  struct flock *u = static_cast<struct flock *>(arg);
  KernelFlockWb k{};
  k.l_start = u->l_start;
  k.l_len = u->l_len;
  k.l_pid = u->l_pid;
  k.l_type = toKL(u->l_type);
  k.l_whence = u->l_whence;
  int rc = fcntl(fd, cmd, &k);
  if (rc == 0 && cmd == F_GETLK) {
    u->l_start = k.l_start;
    u->l_len = k.l_len;
    u->l_pid = k.l_pid;
    u->l_type = (k.l_type == 1 ? F_RDLCK : k.l_type == 3 ? F_WRLCK : F_UNLCK);
    u->l_whence = k.l_whence;
  }
  return rc;
}
}

void installWorkarounds() {
  static bool done = false;
  if (done)
    return;
  done = true;
  sqlite3_initialize();
  sqlite3_vfs *vfs = sqlite3_vfs_find(nullptr);
  if (vfs && vfs->iVersion >= 3 && vfs->xSetSystemCall) {
    vfs->xSetSystemCall(vfs, "stat", (sqlite3_syscall_ptr)statFixed_wb);
    vfs->xSetSystemCall(vfs, "fstat", (sqlite3_syscall_ptr)fstatFixed_wb);
    vfs->xSetSystemCall(vfs, "lstat", (sqlite3_syscall_ptr)statFixed_wb);
    vfs->xSetSystemCall(vfs, "fcntl", (sqlite3_syscall_ptr)fcntlWb);
  }
}

bool openDb(const char *path, WbHandle &h, std::string &err) {
  installWorkarounds();
  int rc = sqlite3_open_v2(path, &h.db, SQLITE_OPEN_READWRITE, nullptr);
  if (rc != SQLITE_OK) {
    const char *msg = h.db ? sqlite3_errmsg(h.db) : sqlite3_errstr(rc);
    char buf[32];
    std::snprintf(buf, sizeof(buf), " (code %d)", rc);
    err = std::string("sqlite open: ") + msg + buf;
    return false;
  }
  sqlite3_busy_timeout(h.db, 3000);
  return true;
}

const char *colText(WbStmt &st, int col) {
  const unsigned char *t = sqlite3_column_text(st.s, col);
  return t ? reinterpret_cast<const char *>(t) : "";
}

bool fileExists(const char *path) {
  struct stat st;
  return Compat::stat(path, &st) == 0;
}

bool copyBinary(const char *src, const char *dst) {
  int in = open(src, O_RDONLY);
  if (in < 0)
    return false;
  int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) {
    close(in);
    return false;
  }
  char buf[32768];
  ssize_t n;
  bool ok = true;
  while (ok && (n = read(in, buf, sizeof(buf))) > 0) {
    for (ssize_t w = 0; w < n;) {
      ssize_t nw = write(out, buf + w, (size_t)(n - w));
      if (nw <= 0) {
        ok = false;
        break;
      }
      w += nw;
    }
  }
  fsync(out);
  close(out);
  close(in);
  return ok;
}

std::string je(const std::string &s) {
  std::string o;
  o.reserve(s.size() + 4);
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

std::string jsonGet(const std::string &j, const std::string &key) {
  std::string needle = "\"" + key + "\"";
  size_t p = j.find(needle);
  if (p == std::string::npos)
    return "";
  p = j.find('"', p + needle.size());
  if (p == std::string::npos)
    return "";
  std::string val;
  for (++p; p < j.size(); ++p) {
    char c = j[p];
    if (c == '"')
      break;
    if (c == '\\' && p + 1 < j.size())
      val += j[++p];
    else
      val += c;
  }
  return val;
}

std::string readFile(const char *path) {
  FILE *f = std::fopen(path, "rb");
  if (!f)
    return "";
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
    s.append(buf, n);
  std::fclose(f);
  return s;
}

std::set<std::string> tableColumns(sqlite3 *db, const std::string &table) {
  std::set<std::string> cols;
  WbStmt st;
  std::string q = "PRAGMA table_info(\"" + table + "\");";
  if (sqlite3_prepare_v2(db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK)
    return cols;
  while (sqlite3_step(st.s) == SQLITE_ROW) {
    const char *name =
        reinterpret_cast<const char *>(sqlite3_column_text(st.s, 1));
    if (name)
      cols.insert(name);
  }
  return cols;
}

std::vector<std::string> allBrowseTables(sqlite3 *db) {
  std::vector<std::string> out;
  WbStmt st;
  const char *q =
      "SELECT name FROM sqlite_master WHERE type='table' AND "
      "(name='tbl_appbrowse' OR name LIKE 'tbl_appbrowse_%') ORDER BY name;";
  if (sqlite3_prepare_v2(db, q, -1, &st.s, nullptr) != SQLITE_OK)
    return out;
  while (sqlite3_step(st.s) == SQLITE_ROW) {
    std::string n = colText(st, 0);
    if (!n.empty())
      out.push_back(n);
  }
  return out;
}

// Search every browse table for the given titleId. Returns {table, titleId}.
// If titleId is empty, probe for the browser using candidates and heuristics.
struct BrowserLoc {
  std::string table;
  std::string titleId;
};

BrowserLoc findBrowser(sqlite3 *db, const std::string &knownTitleId = "") {
  auto tables = allBrowseTables(db);
  if (tables.empty())
    return {};

  if (!knownTitleId.empty()) {
    for (auto &tbl : tables) {
      WbStmt st;
      std::string q = "SELECT 1 FROM \"" + tbl + "\" WHERE titleId=?1 LIMIT 1;";
      if (sqlite3_prepare_v2(db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK)
        continue;
      sqlite3_bind_text(st.s, 1, knownTitleId.c_str(), -1, SQLITE_STATIC);
      if (sqlite3_step(st.s) == SQLITE_ROW)
        return {tbl, knownTitleId};
    }
    return {};
  }

  for (int i = 0; kBrowserCandidates[i]; ++i) {
    const char *cand = kBrowserCandidates[i];
    for (auto &tbl : tables) {
      WbStmt st;
      std::string q = "SELECT 1 FROM \"" + tbl + "\" WHERE titleId=?1 LIMIT 1;";
      if (sqlite3_prepare_v2(db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK)
        continue;
      sqlite3_bind_text(st.s, 1, cand, -1, SQLITE_STATIC);
      if (sqlite3_step(st.s) == SQLITE_ROW)
        return {tbl, cand};
    }
  }

  // Fallback: match NPXS% entry containing "rowser" or "Web" (avoid User's
  // Guide).
  for (auto &tbl : tables) {
    auto cols = tableColumns(db, tbl);
    if (!cols.count("titleName"))
      continue;
    WbStmt st;
    std::string q =
        "SELECT titleId FROM \"" + tbl +
        "\" WHERE titleId LIKE 'NPXS%' "
        "AND (titleName LIKE '%rowser%' OR titleName LIKE '%Web%') LIMIT 1;";
    if (sqlite3_prepare_v2(db, q.c_str(), -1, &st.s, nullptr) == SQLITE_OK &&
        sqlite3_step(st.s) == SQLITE_ROW) {
      std::string id = colText(st, 0);
      if (!id.empty())
        return {tbl, id};
    }
  }
  return {};
}

} // namespace

namespace WebkitBrowser {

std::string resolvedTitleId() {
  WbHandle h;
  std::string err;
  if (!openDb(kDbPath, h, err))
    return "";
  BrowserLoc loc = findBrowser(h.db);
  return loc.titleId;
}

bool backupExists() { return fileExists(kStateFile); }

bool isInstalled() {
  if (!backupExists())
    return false;
  std::string state = readFile(kStateFile);
  return jsonGet(state, "installed") == "1";
}

bool startInstall(const std::string &autoloaderUrl) {
  return GhPlugins::Jobs::instance().start(
      "PS4Webkit", [autoloaderUrl](const auto &progress, std::string &result) {
        progress("Detecting system browser...");

        WbHandle h;
        std::string err;
        if (!openDb(kDbPath, h, err)) {
          result = "Cannot open app.db: " + err;
          return false;
        }
        BrowserLoc loc = findBrowser(h.db);
        if (loc.titleId.empty()) {
          result =
              "[ERROR] System browser not found in any tbl_appbrowse table.";
          return false;
        }
        std::string titleId = loc.titleId;
        std::string browse = loc.table;

        auto cols = tableColumns(h.db, browse);
        bool hasUrl = cols.count("launch_url") > 0;

        std::string origUrl;
        if (hasUrl) {
          progress("Reading original browser record...");
          WbStmt st;
          std::string q = "SELECT \"launch_url\" FROM \"" + browse +
                          "\" WHERE titleId=?1 LIMIT 1;";
          if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) !=
                  SQLITE_OK ||
              sqlite3_bind_text(st.s, 1, titleId.c_str(), -1, SQLITE_STATIC) !=
                  SQLITE_OK ||
              sqlite3_step(st.s) != SQLITE_ROW) {
            result = "Cannot read browser row for " + titleId + " in " + browse;
            return false;
          }
          origUrl = colText(st, 0);
        }

        if (!WebkitBrowser::backupExists()) {
          progress("Saving browser backup state...");
          mkdir(APP_WORK_DIR, 0777);
          mkdir(kBackupDir, 0777);

          std::string json = "{\n"
                             "  \"titleId\": \"" +
                             je(titleId) +
                             "\",\n"
                             "  \"launch_url\": \"" +
                             je(origUrl) +
                             "\",\n"
                             "  \"browseTable\": \"" +
                             je(browse) +
                             "\",\n"
                             "  \"installed\": \"0\"\n"
                             "}\n";
          int fd = open(kStateFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
          if (fd < 0) {
            result = "Cannot write state file";
            return false;
          }
          write(fd, json.c_str(), json.size());
          close(fd);
        }

        if (hasUrl) {
          progress("Updating browser launch URL...");
          if (sqlite3_exec(h.db, "BEGIN IMMEDIATE;", nullptr, nullptr,
                           nullptr) != SQLITE_OK) {
            result = "Cannot begin transaction: " +
                     std::string(sqlite3_errmsg(h.db));
            return false;
          }

          {
            WbStmt st;
            std::string q = "UPDATE \"" + browse +
                            "\" SET \"launch_url\"=?1 WHERE titleId=?2;";
            if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) !=
                SQLITE_OK) {
              sqlite3_exec(h.db, "ROLLBACK;", nullptr, nullptr, nullptr);
              result =
                  "Cannot prepare UPDATE: " + std::string(sqlite3_errmsg(h.db));
              return false;
            }
            sqlite3_bind_text(st.s, 1, autoloaderUrl.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_bind_text(st.s, 2, titleId.c_str(), -1, SQLITE_TRANSIENT);

            if (sqlite3_step(st.s) != SQLITE_DONE) {
              sqlite3_exec(h.db, "ROLLBACK;", nullptr, nullptr, nullptr);
              result = "Cannot update browse table: " +
                       std::string(sqlite3_errmsg(h.db));
              return false;
            }
          }

          if (sqlite3_exec(h.db, "COMMIT;", nullptr, nullptr, nullptr) !=
              SQLITE_OK) {
            result = "Commit failed: " + std::string(sqlite3_errmsg(h.db));
            return false;
          }
        }

        {
          std::string s2 = readFile(kStateFile);
          size_t p = s2.find("\"installed\"");
          if (p != std::string::npos) {
            size_t q2 = s2.find('"', s2.find(':', p) + 1) + 1;
            size_t r = s2.find('"', q2);
            if (r != std::string::npos)
              s2.replace(q2, r - q2, "1");
          }
          int fd = open(kStateFile, O_WRONLY | O_TRUNC, 0644);
          if (fd >= 0) {
            write(fd, s2.c_str(), s2.size());
            close(fd);
          }
        }

        result = "[SUCCESS] PS4Webkit installed (" + titleId +
                 ", autoloader + bookmark ready; stock browser preserved).";
        return true;
      });
}

bool startRestore() {
  if (!backupExists())
    return false;
  return GhPlugins::Jobs::instance().start(
      "Restore Browser", [](const auto &progress, std::string &result) {
        progress("Reading backup state...");
        std::string state = readFile(kStateFile);
        if (state.empty()) {
          result = "Backup state file missing or empty";
          return false;
        }

        std::string titleId = jsonGet(state, "titleId");
        std::string origName = jsonGet(state, "titleName");
        std::string origUrl = jsonGet(state, "launch_url");
        std::string iconOnDisk = jsonGet(state, "iconOnDisk");
        std::string savedTable = jsonGet(state, "browseTable");
        std::string titleTableName = jsonGet(state, "titleTable");
        std::string origTitle0 = jsonGet(state, "titleI18n");

        if (titleId.empty()) {
          result = "Backup is incomplete - titleId missing";
          return false;
        }

        if (!iconOnDisk.empty() && fileExists(kIconBackup)) {
          progress("Restoring original icon...");
          copyBinary(kIconBackup, iconOnDisk.c_str());
        }

        progress("Restoring app.db entries...");
        WbHandle h;
        std::string err;
        if (!openDb(kDbPath, h, err)) {
          result = "Cannot open app.db: " + err;
          return false;
        }

        BrowserLoc loc = findBrowser(h.db, titleId);
        std::string browse = loc.table;
        if (browse.empty())
          browse = savedTable;
        if (browse.empty()) {
          result = "Cannot find browse table for " + titleId;
          return false;
        }

        auto cols = tableColumns(h.db, browse);
        bool hasName = cols.count("titleName") > 0;
        bool hasUrl = cols.count("launch_url") > 0;

        bool needTx = (hasName && !origName.empty()) ||
                      (hasUrl && !origUrl.empty()) ||
                      (!titleTableName.empty() && !origTitle0.empty());

        if (needTx) {
          if (sqlite3_exec(h.db, "BEGIN IMMEDIATE;", nullptr, nullptr,
                           nullptr) != SQLITE_OK) {
            result = "Cannot begin transaction: " +
                     std::string(sqlite3_errmsg(h.db));
            return false;
          }

          {
            std::string setClauses;
            int paramIdx = 1;
            int nameParam = 0, urlParam = 0, idParam = 0;
            if (hasName && !origName.empty()) {
              setClauses += "\"titleName\"=?" + std::to_string(paramIdx);
              nameParam = paramIdx++;
            }
            if (hasUrl && !origUrl.empty()) {
              if (!setClauses.empty())
                setClauses += ", ";
              setClauses += "\"launch_url\"=?" + std::to_string(paramIdx);
              urlParam = paramIdx++;
            }
            idParam = paramIdx;

            if (!setClauses.empty()) {
              WbStmt st;
              std::string q = "UPDATE \"" + browse + "\" SET " + setClauses +
                              " WHERE titleId=?" + std::to_string(idParam) +
                              ";";
              if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) !=
                  SQLITE_OK) {
                sqlite3_exec(h.db, "ROLLBACK;", nullptr, nullptr, nullptr);
                result = "Cannot prepare UPDATE: " +
                         std::string(sqlite3_errmsg(h.db));
                return false;
              }
              if (nameParam)
                sqlite3_bind_text(st.s, nameParam, origName.c_str(), -1,
                                  SQLITE_TRANSIENT);
              if (urlParam)
                sqlite3_bind_text(st.s, urlParam, origUrl.c_str(), -1,
                                  SQLITE_TRANSIENT);
              sqlite3_bind_text(st.s, idParam, titleId.c_str(), -1,
                                SQLITE_TRANSIENT);

              if (sqlite3_step(st.s) != SQLITE_DONE) {
                sqlite3_exec(h.db, "ROLLBACK;", nullptr, nullptr, nullptr);
                result = "Cannot restore browse table: " +
                         std::string(sqlite3_errmsg(h.db));
                return false;
              }
            }
          }

          if (!titleTableName.empty() && !origTitle0.empty()) {
            auto ttCols = tableColumns(h.db, titleTableName);
            std::string titleCol =
                ttCols.count("title")
                    ? "title"
                    : (ttCols.count("titleName") ? "titleName" : "");
            if (!titleCol.empty()) {
              WbStmt st;
              std::string q = "UPDATE \"" + titleTableName + "\" SET \"" +
                              titleCol + "\"=?1 WHERE titleId=?2;";
              if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) ==
                  SQLITE_OK) {
                sqlite3_bind_text(st.s, 1, origTitle0.c_str(), -1,
                                  SQLITE_TRANSIENT);
                sqlite3_bind_text(st.s, 2, titleId.c_str(), -1,
                                  SQLITE_TRANSIENT);
                sqlite3_step(st.s);
              }
            }
          }

          if (sqlite3_exec(h.db, "COMMIT;", nullptr, nullptr, nullptr) !=
              SQLITE_OK) {
            result = "Commit failed: " + std::string(sqlite3_errmsg(h.db));
            return false;
          }
        }

        unlink(kIconBackup);
        unlink(kStateFile);
        rmdir(kBackupDir);

        result = "[SUCCESS] Browser settings restored to factory defaults.";
        return true;
      });
}

namespace {

// Tag strings embedded in the thumbnail_path column so we can find our own
// bookmarks later on and remove them without touching user-added ones.
const char *kBookmarkTag = "ps4hub-loader";
const char *kBookmarkTagFallback = "ps4hub-loader-fallback";
const char *kOnlineUrl = "https://laroping.github.io/ps4webkit/";

template <typename Fn> void forEachBookmarkDb(Fn cb) {
  DIR *d = opendir("/user/home");
  if (!d)
    return;
  while (struct dirent *e = readdir(d)) {
    std::string nm = e->d_name;
    if (nm == "." || nm == "..")
      continue;
    std::string path =
        std::string("/user/home/") + nm + "/webbrowser/webbrowser_bookmark.db";
    struct stat st;
    if (Compat::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
      continue;

    installWorkarounds();
    sqlite3 *db = nullptr;
    if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) !=
        SQLITE_OK) {
      if (db)
        sqlite3_close(db);
      continue;
    }
    sqlite3_busy_timeout(db, 3000);
    cb(db, path);
    sqlite3_exec(db, "PRAGMA wal_checkpoint(FULL);", nullptr, nullptr, nullptr);
    sqlite3_close(db);
  }
  closedir(d);
}

std::string isoTimestampNow() {
  time_t t = time(nullptr);
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
                tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour,
                tmv.tm_min, tmv.tm_sec);
  return buf;
}

} // namespace

static bool upsertBookmark(sqlite3 *db, const char *title,
                           const std::string &url, const char *tag) {
  {
    WbStmt st;
    const char *q = "DELETE FROM tbl_browserbookmark WHERE thumbnail_path=?1;";
    if (sqlite3_prepare_v2(db, q, -1, &st.s, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(st.s, 1, tag, -1, SQLITE_STATIC);
      sqlite3_step(st.s);
    }
  }
  WbStmt ins;
  const char *iq =
      "INSERT INTO tbl_browserbookmark "
      "(attribute, title, url, thumbnail_path, directory_id, order_id, "
      " create_time, update_time, lastaccess_time) "
      "VALUES (0, ?1, ?2, ?3, 0, 0, ?4, ?4, ?4);";
  if (sqlite3_prepare_v2(db, iq, -1, &ins.s, nullptr) != SQLITE_OK)
    return false;
  std::string ts = isoTimestampNow();
  sqlite3_bind_text(ins.s, 1, title, -1, SQLITE_STATIC);
  sqlite3_bind_text(ins.s, 2, url.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(ins.s, 3, tag, -1, SQLITE_STATIC);
  sqlite3_bind_text(ins.s, 4, ts.c_str(), -1, SQLITE_TRANSIENT);
  return sqlite3_step(ins.s) == SQLITE_DONE;
}

std::string addLoaderBookmark(const std::string &url) {
  int users = 0, errors = 0;

  forEachBookmarkDb([&](sqlite3 *db, const std::string & /*path*/) {
    ++users;
    if (!upsertBookmark(db, "PS4Webkit Loader", url, kBookmarkTag))
      ++errors;
    if (!upsertBookmark(db, "PS4Webkit Loader (Online)", kOnlineUrl,
                        kBookmarkTagFallback))
      ++errors;
  });

  if (users == 0)
    return "No bookmark DB found (sign in to a user first?)";
  std::string msg = "Bookmarks saved (" + std::to_string(users) + " user(s))";
  if (errors)
    msg += ", " + std::to_string(errors) + " error(s)";
  return msg;
}

std::string removeLoaderBookmark() {
  int removed = 0;
  forEachBookmarkDb([&](sqlite3 *db, const std::string & /*path*/) {
    const char *tags[] = {kBookmarkTag, kBookmarkTagFallback};
    for (const char *tag : tags) {
      WbStmt st;
      const char *q =
          "DELETE FROM tbl_browserbookmark WHERE thumbnail_path=?1;";
      if (sqlite3_prepare_v2(db, q, -1, &st.s, nullptr) != SQLITE_OK)
        continue;
      sqlite3_bind_text(st.s, 1, tag, -1, SQLITE_STATIC);
      if (sqlite3_step(st.s) == SQLITE_DONE)
        removed += sqlite3_changes(db);
    }
  });
  return "Removed " + std::to_string(removed) + " bookmark entry(ies)";
}

} // namespace WebkitBrowser
