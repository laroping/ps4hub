#include "catalog_db.h"
#include "config.h"
#include "libc_compat.h"

#include <sqlite3.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

struct CatalogDb::Handle {
  sqlite3 *db = nullptr;
  ~Handle() {
    if (db)
      sqlite3_close(db);
  }
};

namespace {

struct Stmt {
  sqlite3_stmt *s = nullptr;
  ~Stmt() {
    if (s)
      sqlite3_finalize(s);
  }
};

// The OpenOrbis libc implements lstat() on top of a stubbed fstatat() that
// always fails with ENOSYS. SQLite's unix VFS lstat()s every component of the
// database path while canonicalising it, so every open ended in
// SQLITE_CANTOPEN ("unable to open database file"). Route it to the kernel's
// stat(); SQLite only uses the result to detect symlinks.
//
// stat()/fstat() themselves work but fill the FreeBSD 9 struct layout, which
// differs from the header's (see libc_compat.h): SQLite read st_blocks as the
// file size and reported SQLITE_CORRUPT ("database disk image is malformed").
int statFixed(const char *path, struct stat *st) {
  return Compat::stat(path, st);
}
int fstatFixed(int fd, struct stat *st) { return Compat::fstat(fd, st); }

// The OpenOrbis <fcntl.h> declares struct flock in musl/Linux order
// (l_type, l_whence, l_start, l_len, l_pid) but fcntl() hands the pointer to
// the FreeBSD kernel unchanged, which expects the layout below. Every SQLite
// lock therefore failed with EINVAL -> SQLITE_IOERR_LOCK ("disk I/O error").
struct KernelFlock {
  int64_t l_start;
  int64_t l_len;
  int32_t l_pid;
  int16_t l_type;
  int16_t l_whence;
  int32_t l_sysid;
};
static_assert(sizeof(KernelFlock) == 32, "FreeBSD struct flock is 32 bytes");

// The lock *types* differ as well: <fcntl.h> here has the Linux values
// (RDLCK 0, WRLCK 1, UNLCK 2), FreeBSD wants RDLCK 1, UNLCK 2, WRLCK 3.
// Sending 0 is what produced EINVAL (SQLITE_IOERR_LOCK, code 3850).
constexpr int16_t kBsdRdLck = 1, kBsdUnLck = 2, kBsdWrLck = 3;

int16_t toKernelLockType(short t) {
  if (t == F_RDLCK)
    return kBsdRdLck;
  if (t == F_WRLCK)
    return kBsdWrLck;
  return kBsdUnLck;
}

short fromKernelLockType(int16_t t) {
  if (t == kBsdRdLck)
    return F_RDLCK;
  if (t == kBsdWrLck)
    return F_WRLCK;
  return F_UNLCK;
}

int fcntlWithKernelFlock(int fd, int cmd, ...) {
  va_list ap;
  va_start(ap, cmd);
  void *arg = va_arg(ap, void *);
  va_end(ap);
  if (cmd != F_GETLK && cmd != F_SETLK && cmd != F_SETLKW)
    return fcntl(fd, cmd, arg);

  struct flock *user = static_cast<struct flock *>(arg);
  KernelFlock k{};
  k.l_start = user->l_start;
  k.l_len = user->l_len;
  k.l_pid = user->l_pid;
  k.l_type = toKernelLockType(user->l_type);
  k.l_whence = user->l_whence;
  int rc = fcntl(fd, cmd, &k);
  if (rc == 0 && cmd == F_GETLK) {
    user->l_start = k.l_start;
    user->l_len = k.l_len;
    user->l_pid = k.l_pid;
    user->l_type = fromKernelLockType(k.l_type);
    user->l_whence = k.l_whence;
  }
  return rc;
}

void installLibcWorkarounds() {
  static bool done = false;
  if (done)
    return;
  done = true;
  sqlite3_initialize();
  sqlite3_vfs *vfs = sqlite3_vfs_find(nullptr);
  if (vfs && vfs->iVersion >= 3 && vfs->xSetSystemCall) {
    vfs->xSetSystemCall(vfs, "stat", (sqlite3_syscall_ptr)statFixed);
    vfs->xSetSystemCall(vfs, "fstat", (sqlite3_syscall_ptr)fstatFixed);
    vfs->xSetSystemCall(vfs, "lstat", (sqlite3_syscall_ptr)statFixed);
    vfs->xSetSystemCall(vfs, "fcntl",
                        (sqlite3_syscall_ptr)fcntlWithKernelFlock);
  }
}

// "message (extended code N, errno M)" for any failing connection.
std::string describe(sqlite3 *db, int rc) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), " (code %d, errno %d)",
                db ? sqlite3_extended_errcode(db) : rc,
                db ? sqlite3_system_errno(db) : 0);
  return std::string(db ? sqlite3_errmsg(db) : sqlite3_errstr(rc)) + buf;
}

bool openHandle(const std::string &path, sqlite3 **out, std::string &err) {
  installLibcWorkarounds();
  int rc = sqlite3_open_v2(path.c_str(), out, SQLITE_OPEN_READWRITE, nullptr);
  if (rc != SQLITE_OK) {
    err = "sqlite open failed: " + describe(*out, rc);
    return false;
  }
  // SceShellUI keeps app.db open; wait instead of failing on SQLITE_BUSY.
  sqlite3_busy_timeout(*out, 3000);
  return true;
}

// Identifiers coming from sqlite_master / PRAGMA are restricted to [A-Za-z0-9_]
// before they are spliced into SQL (values are always bound parameters).
bool isSafeIdent(const std::string &s) {
  if (s.empty())
    return false;
  for (char c : s)
    if (!(std::isalnum((unsigned char)c) || c == '_'))
      return false;
  return true;
}

std::string quoteIdent(const std::string &s) { return "\"" + s + "\""; }

const char *textOr(sqlite3_stmt *s, int col, const char *fallback) {
  const unsigned char *t = sqlite3_column_text(s, col);
  return t ? reinterpret_cast<const char *>(t) : fallback;
}

// One slot per minute of the day: minuteOfDay 1439 -> "23:59:00.000",
// 0 -> "00:00:00.000". Clamped so it never wraps into another day.
std::string makeTimestamp(const char *date, long minuteOfDay) {
  if (minuteOfDay < 0)
    minuteOfDay = 0;
  if (minuteOfDay > 1439)
    minuteOfDay = 1439;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%s %02ld:%02ld:00.000", date,
                minuteOfDay / 60, minuteOfDay % 60);
  return buf;
}

bool copyFile(const std::string &src, const std::string &dst) {
  FILE *in = std::fopen(src.c_str(), "rb");
  if (!in)
    return false;
  std::string tmp = dst + ".tmp";
  FILE *out = std::fopen(tmp.c_str(), "wb");
  if (!out) {
    std::fclose(in);
    return false;
  }

  static char buf[64 * 1024];
  bool ok = true;
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0) {
    if (std::fwrite(buf, 1, n, out) != n) {
      ok = false;
      break;
    }
  }
  if (std::ferror(in))
    ok = false;
  std::fclose(in);
  if (std::fflush(out) != 0)
    ok = false;
  std::fclose(out);

  if (!ok) {
    unlink(tmp.c_str());
    return false;
  }
  // Atomic replace so a half-written backup never overwrites a good one.
  return std::rename(tmp.c_str(), dst.c_str()) == 0;
}

// Recursive delete without lstat() (stubbed in this libc). Entries are
// unlink()ed first; only when that fails because the entry is a directory do
// we descend, so a symlink is removed itself and never followed.
bool removeEntry(const std::string &path, unsigned char type);

bool removeDirectory(const std::string &path) {
  DIR *d = opendir(path.c_str());
  if (!d)
    return errno == ENOENT;
  bool ok = true;
  while (struct dirent *e = readdir(d)) {
    if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
      continue;
    if (!removeEntry(path + "/" + e->d_name, e->d_type))
      ok = false;
  }
  closedir(d);
  if (rmdir(path.c_str()) != 0 && errno != ENOENT)
    ok = false;
  return ok;
}

bool removeEntry(const std::string &path, unsigned char type) {
  if (type == DT_DIR)
    return removeDirectory(path);
  if (unlink(path.c_str()) == 0 || errno == ENOENT)
    return true;
  if (errno == EPERM || errno == EISDIR)
    return removeDirectory(path);
  return false;
}

bool removeTree(const std::string &path) {
  struct stat st;
  if (Compat::stat(path.c_str(), &st) != 0)
    return errno == ENOENT;
  return S_ISDIR(st.st_mode) ? removeDirectory(path)
                             : unlink(path.c_str()) == 0;
}

} // namespace

CatalogDb::CatalogDb(std::string dbPath) : dbPath_(std::move(dbPath)) {}

bool CatalogDb::exec(Handle &h, const char *sql) {
  char *msg = nullptr;
  if (sqlite3_exec(h.db, sql, nullptr, nullptr, &msg) != SQLITE_OK) {
    sqlite3_free(msg);
    error_ = describe(h.db, SQLITE_ERROR);
    return false;
  }
  return true;
}

bool CatalogDb::hasColumn(Handle &h, const std::string &column) {
  Stmt st;
  std::string q = "PRAGMA table_info(" + quoteIdent(table_) + ");";
  if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK)
    return false;
  while (sqlite3_step(st.s) == SQLITE_ROW)
    if (column == textOr(st.s, 1, ""))
      return true;
  return false;
}

bool CatalogDb::open(int userId) {
  table_.clear();
  error_.clear();

  // Without the GoldHEN jailbreak the sandbox denies /system_data: report
  // it instead of letting SQLite fail later.
  if (access(dbPath_.c_str(), R_OK | W_OK) != 0) {
    error_ = dbPath_ + ": " + std::strerror(errno) + " (jailbreak required)";
    return false;
  }

  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return false;

  // Firmware variants use either a single table or one per user.
  char perUser[64];
  std::snprintf(perUser, sizeof(perUser), "tbl_appbrowse_%010d", userId);

  std::vector<std::string> tables;
  {
    Stmt st;
    const char *q = "SELECT name FROM sqlite_master WHERE type='table' AND "
                    "(name='tbl_appbrowse' OR name LIKE 'tbl_appbrowse_%');";
    if (sqlite3_prepare_v2(h.db, q, -1, &st.s, nullptr) != SQLITE_OK) {
      error_ = describe(h.db, SQLITE_ERROR);
      return false;
    }
    while (sqlite3_step(st.s) == SQLITE_ROW) {
      std::string n = textOr(st.s, 0, "");
      if (isSafeIdent(n))
        tables.push_back(n);
    }
  }
  if (tables.empty()) {
    error_ = "No tbl_appbrowse table found";
    return false;
  }
  tables_ = tables;

  // The table the home screen actually uses is the one where the system
  // has just recorded this launch of PS4Hub (newest time on our own row).
  // Fallbacks: the current user's table, then tbl_appbrowse.
  std::string chosen = tables.front();
  for (auto &t : tables)
    if (t == "tbl_appbrowse")
      chosen = t;
  for (auto &t : tables)
    if (t == perUser)
      chosen = t;
  std::string liveTime;
  for (auto &t : tables) {
    for (const char *col : {"last_launch_time", "lastAccessTime"}) {
      Stmt st;
      std::string q = "SELECT " + quoteIdent(col) + " FROM " + quoteIdent(t) +
                      " WHERE titleId = ?1;";
      if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK)
        continue;
      sqlite3_bind_text(st.s, 1, APP_TITLE_ID, -1, SQLITE_STATIC);
      if (sqlite3_step(st.s) != SQLITE_ROW)
        continue;
      std::string v = textOr(st.s, 0, "");
      if (!v.empty() && v.rfind("2099", 0) != 0 && v > liveTime) {
        liveTime = v;
        chosen = t;
      }
    }
  }
  table_ = chosen;
  std::printf("[PS4Hub] catalog table %s (of %zu, user %d)\n", table_.c_str(),
              tables.size(), userId);

  if (!loadColumns(h)) {
    error_ = "Unsupported schema in " + table_;
    table_.clear();
    return false;
  }
  if (FILE *lf = std::fopen(APP_WORK_DIR "/catalog_writes.log",
                            "w")) { // one log per run
    std::fprintf(lf, "main table %s, time %s%s%s, pos %s\n", table_.c_str(),
                 timeCol_.c_str(), time2Col_.empty() ? "" : " + ",
                 time2Col_.c_str(), posCol_.c_str());
    for (auto &t : tables_)
      std::fprintf(lf, "table %s\n", t.c_str());
    std::fclose(lf);
  }
  return true;
}

// Resolves the layout columns of table_ (they can differ between copies).
bool CatalogDb::loadColumns(Handle &h) {
  auto pick = [&](std::initializer_list<const char *> names) -> std::string {
    for (const char *n : names)
      if (hasColumn(h, n))
        return n;
    return "";
  };
  nameCol_ = pick({"titleName"});
  timeCol_ = pick({"last_launch_time", "lastAccessTime"});
  posCol_ = pick({"ui_pos", "sortPriority"});
  visibleCol_ = pick({"visible"});
  categoryCol_ = pick({"category"});
  folderCol_ = pick({"folderType", "isFolder", "folder_type", "is_folder"});
  // Some firmwares keep both time columns; the home screen may sort by
  // either, so both are written - only if they hold the same date format.
  time2Col_.clear();
  if (timeCol_ == "last_launch_time" && hasColumn(h, "lastAccessTime")) {
    Stmt st;
    std::string q = "SELECT \"lastAccessTime\" FROM " + quoteIdent(table_) +
                    " WHERE typeof(\"lastAccessTime\") = 'text' LIMIT 1;";
    if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) == SQLITE_OK &&
        sqlite3_step(st.s) == SQLITE_ROW) {
      std::string v = textOr(st.s, 0, "");
      if (v.size() >= 19 && v[4] == '-' && v[7] == '-' && v[10] == ' ' &&
          v[13] == ':')
        time2Col_ = "lastAccessTime";
    }
  }
  return hasColumn(h, "titleId") && !timeCol_.empty();
}

std::string CatalogDb::timeAssign() const {
  std::string a = quoteIdent(timeCol_) + " = ?1";
  if (!time2Col_.empty())
    a += ", " + quoteIdent(time2Col_) + " = ?1";
  return a;
}

bool CatalogDb::backup(const std::string &backupPath) {
  if (!copyFile(dbPath_, backupPath)) {
    error_ = "Backup failed: " + backupPath;
    return false;
  }
  return true;
}

bool CatalogDb::restore(const std::string &backupPath) {
  if (!copyFile(backupPath, dbPath_)) {
    error_ = "Restore failed: " + backupPath;
    return false;
  }
  return true;
}

std::vector<CatalogEntry> CatalogDb::fetchEntries() {
  std::vector<CatalogEntry> list;
  if (!isReady())
    return list;

  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return list;

  std::string q =
      "SELECT titleId, " +
      (nameCol_.empty() ? std::string("''") : quoteIdent(nameCol_)) + ", " +
      quoteIdent(timeCol_) + ", " +
      (posCol_.empty() ? std::string("0") : quoteIdent(posCol_)) + ", " +
      (visibleCol_.empty() ? std::string("1") : quoteIdent(visibleCol_)) +
      ", " +
      (categoryCol_.empty() ? std::string("''") : quoteIdent(categoryCol_)) +
      ", " + (folderCol_.empty() ? std::string("0") : quoteIdent(folderCol_)) +
      " FROM " +
      quoteIdent(table_)
      // The home screen sorts by position first; the launch time only
      // breaks ties. (Hidden rows and folders, which carry visible = 0,
      // are filtered by the caller, not here.)
      + " ORDER BY " +
      (posCol_.empty() ? std::string() : quoteIdent(posCol_) + " ASC, ") +
      quoteIdent(timeCol_) + " DESC;";

  Stmt st;
  if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK) {
    error_ = describe(h.db, SQLITE_ERROR);
    return list;
  }
  while (sqlite3_step(st.s) == SQLITE_ROW) {
    CatalogEntry e;
    e.titleId = textOr(st.s, 0, "");
    if (e.titleId.empty())
      continue;
    e.titleName = textOr(st.s, 1, "");
    e.lastLaunch = textOr(st.s, 2, "");
    e.uiPos = sqlite3_column_int(st.s, 3);
    e.visible = sqlite3_column_int(st.s, 4) != 0;
    e.category = textOr(st.s, 5, "");
    e.folderFlag = sqlite3_column_int(st.s, 6);
    std::string lowId = e.titleId, lowCat = e.category;
    for (auto &c : lowId)
      c = (char)std::tolower((unsigned char)c);
    for (auto &c : lowCat)
      c = (char)std::tolower((unsigned char)c);
    e.isFolder = e.folderFlag != 0 ||
                 lowId.find("folder") != std::string::npos ||
                 lowCat.find("folder") != std::string::npos;
    if (e.titleName.empty())
      e.titleName = e.titleId;
    list.push_back(std::move(e));
  }
  return list;
}

bool CatalogDb::commitOrder(const std::vector<std::string> &pinned,
                            const std::vector<std::string> &sendToEnd) {
  if (!mirroring_)
    return mirrored([&] { return commitOrder(pinned, sendToEnd); });
  if (!isReady())
    return false;
  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return false;

  std::string q =
      "UPDATE " + quoteIdent(table_) + " SET " + timeAssign() + "" +
      (posCol_.empty() ? std::string() : ", " + quoteIdent(posCol_) + " = ?2") +
      " WHERE titleId = ?3;";
  Stmt st;
  if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK) {
    error_ = describe(h.db, SQLITE_ERROR);
    return false;
  }
  if (!exec(h, "BEGIN IMMEDIATE;"))
    return false;

  auto apply = [&](const std::string &id, const std::string &ts, int pos) {
    sqlite3_reset(st.s);
    sqlite3_bind_text(st.s, 1, ts.c_str(), -1, SQLITE_TRANSIENT);
    if (!posCol_.empty())
      sqlite3_bind_int(st.s, 2, pos);
    sqlite3_bind_text(st.s, 3, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.s) != SQLITE_DONE) {
      error_ = describe(h.db, SQLITE_ERROR);
      return false;
    }
    return true;
  };

  bool ok = true;
  for (size_t i = 0; ok && i < pinned.size(); ++i)
    ok = apply(pinned[i], makeTimestamp("2099-12-31", 1439 - (long)i),
               (int)i + 1); // 23:59:00 down
  for (size_t i = 0; ok && i < sendToEnd.size(); ++i)
    ok = apply(sendToEnd[i], makeTimestamp("2000-01-01", (long)i),
               990 + (int)i); // 00:00:00 up

  if (!ok) {
    exec(h, "ROLLBACK;");
    return false;
  }
  return exec(h, "COMMIT;");
}

DefaultTiles CatalogDb::defaultTilesState() {
  if (!isReady())
    return DefaultTiles::Unknown;
  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return DefaultTiles::Unknown;

  std::string q = "SELECT visible FROM " + quoteIdent(table_) +
                  " WHERE titleId IN ('NPXS29999', 'NPXS29998');";
  Stmt st;
  if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK) {
    error_ = describe(h.db, SQLITE_ERROR);
    return DefaultTiles::Unknown;
  }
  int rows = 0, shown = 0;
  while (sqlite3_step(st.s) == SQLITE_ROW) {
    ++rows;
    if (sqlite3_column_int(st.s, 0) != 0)
      ++shown;
  }
  if (rows == 0)
    return DefaultTiles::Missing;
  if (shown == rows)
    return DefaultTiles::Visible;
  if (shown == 0)
    return DefaultTiles::Hidden;
  return DefaultTiles::Mixed;
}

bool CatalogDb::setDefaultTilesVisible(bool visible) {
  if (!mirroring_)
    return mirrored([&] { return setDefaultTilesVisible(visible); });
  if (!isReady())
    return false;
  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return false;

  // Fixed statements; only the table name is resolved (tbl_appbrowse, or the
  // per-user tbl_appbrowse_<id> on firmwares that split it).
  std::string q = "UPDATE " + quoteIdent(table_) +
                  (visible ? " SET visible = 1, canRemove = 0"
                           : " SET visible = 0, canRemove = 1") +
                  " WHERE titleId IN ('NPXS29999', 'NPXS29998');";
  if (!exec(h, q.c_str()))
    return false;
  if (sqlite3_changes(h.db) == 0) {
    error_ = "NPXS29999 / NPXS29998 are not in " + table_;
    return false;
  }
  return true;
}

bool CatalogDb::isPlainTitleId(const std::string &id) {
  if (id.size() != 9)
    return false;
  for (int i = 0; i < 4; ++i)
    if (!std::isupper((unsigned char)id[i]))
      return false;
  for (int i = 4; i < 9; ++i)
    if (!std::isdigit((unsigned char)id[i]))
      return false;
  return true;
}

// Runs a write on the main table and then, best effort, on every other
// browse table, so the change reaches whichever copy the home screen reads.
template <typename F> bool CatalogDb::mirrored(F f) {
  mirroring_ = true;
  const std::string primary = table_;
  const std::string cols[] = {nameCol_,    timeCol_,     time2Col_, posCol_,
                              visibleCol_, categoryCol_, folderCol_};
  bool ok = f();
  const std::string err = error_;
  FILE *lf = std::fopen(APP_WORK_DIR "/catalog_writes.log", "a");
  if (lf)
    std::fprintf(lf, "%s: %s\n", primary.c_str(), ok ? "ok" : error_.c_str());
  if (ok)
    for (auto &t : tables_) {
      if (t == primary)
        continue;
      table_ = t;
      bool done = false;
      {
        Handle h;
        done = openHandle(dbPath_, &h.db, error_) && loadColumns(h);
        if (!done)
          error_ = "columns not found";
      }
      if (done)
        done = f();
      if (lf)
        std::fprintf(lf, "  %s (time %s%s%s): %s\n", t.c_str(),
                     timeCol_.c_str(), time2Col_.empty() ? "" : " + ",
                     time2Col_.c_str(), done ? "ok" : error_.c_str());
    }
  if (lf)
    std::fclose(lf);
  table_ = primary;
  nameCol_ = cols[0];
  timeCol_ = cols[1];
  time2Col_ = cols[2];
  posCol_ = cols[3];
  visibleCol_ = cols[4];
  categoryCol_ = cols[5];
  folderCol_ = cols[6];
  error_ = err;
  mirroring_ = false;
  return ok;
}

bool CatalogDb::purgeApplication(const std::string &titleId) {
  if (!isReady())
    return false;
  if (!isPlainTitleId(titleId)) {
    error_ = "Refusing non-standard title id";
    return false;
  }

  // Filesystem first: if it fails the catalog row stays and the user can retry.
  std::string dir = std::string(USER_APP_ROOT) + "/" + titleId;
  if (!removeTree(dir)) {
    error_ = "Could not remove " + dir;
    return false;
  }

  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return false;
  std::string q = "DELETE FROM " + quoteIdent(table_) + " WHERE titleId = ?1;";
  Stmt st;
  if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK) {
    error_ = describe(h.db, SQLITE_ERROR);
    return false;
  }
  sqlite3_bind_text(st.s, 1, titleId.c_str(), -1, SQLITE_TRANSIENT);
  if (sqlite3_step(st.s) != SQLITE_DONE) {
    error_ = describe(h.db, SQLITE_ERROR);
    return false;
  }
  sqlite3_reset(
      st.s); // release this connection's statement before the next write
  deleteRows({titleId}); // the same row in the other browse tables
  return true;
}

bool CatalogDb::setVisible(const std::vector<std::string> &ids, bool visible) {
  if (!mirroring_)
    return mirrored([&] { return setVisible(ids, visible); });
  if (!isReady() || visibleCol_.empty()) {
    error_ = "No visible column";
    return false;
  }
  if (ids.empty())
    return true;
  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return false;
  std::string q = "UPDATE " + quoteIdent(table_) + " SET " +
                  quoteIdent(visibleCol_) + " = ?1 WHERE titleId = ?2;";
  Stmt st;
  if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK) {
    error_ = describe(h.db, SQLITE_ERROR);
    return false;
  }
  if (!exec(h, "BEGIN IMMEDIATE;"))
    return false;
  for (const auto &id : ids) {
    sqlite3_reset(st.s);
    sqlite3_bind_int(st.s, 1, visible ? 1 : 0);
    sqlite3_bind_text(st.s, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.s) != SQLITE_DONE) {
      error_ = describe(h.db, SQLITE_ERROR);
      exec(h, "ROLLBACK;");
      return false;
    }
  }
  return exec(h, "COMMIT;");
}

bool CatalogDb::deleteRows(const std::vector<std::string> &ids) {
  if (!mirroring_)
    return mirrored([&] { return deleteRows(ids); });
  if (!isReady())
    return false;
  if (ids.empty())
    return true;
  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return false;
  std::string q = "DELETE FROM " + quoteIdent(table_) + " WHERE titleId = ?1;";
  Stmt st;
  if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK) {
    error_ = describe(h.db, SQLITE_ERROR);
    return false;
  }
  if (!exec(h, "BEGIN IMMEDIATE;"))
    return false;
  for (const auto &id : ids) {
    sqlite3_reset(st.s);
    sqlite3_bind_text(st.s, 1, id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.s) != SQLITE_DONE) {
      error_ = describe(h.db, SQLITE_ERROR);
      exec(h, "ROLLBACK;");
      return false;
    }
  }
  return exec(h, "COMMIT;");
}

bool CatalogDb::setLaunchTimes(
    const std::vector<std::pair<std::string, std::string>> &idAndTime) {
  if (!mirroring_)
    return mirrored([&] { return setLaunchTimes(idAndTime); });
  if (!isReady())
    return false;
  if (idAndTime.empty())
    return true;
  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return false;
  std::string q = "UPDATE " + quoteIdent(table_) + " SET " + timeAssign() +
                  " WHERE titleId = ?2;";
  Stmt st;
  if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK) {
    error_ = describe(h.db, SQLITE_ERROR);
    return false;
  }
  if (!exec(h, "BEGIN IMMEDIATE;"))
    return false;
  for (const auto &it : idAndTime) {
    sqlite3_reset(st.s);
    sqlite3_bind_text(st.s, 1, it.second.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st.s, 2, it.first.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.s) != SQLITE_DONE) {
      error_ = describe(h.db, SQLITE_ERROR);
      exec(h, "ROLLBACK;");
      return false;
    }
  }
  return exec(h, "COMMIT;");
}

bool CatalogDb::restoreRows(const std::vector<RowState> &rows) {
  if (!mirroring_)
    return mirrored([&] { return restoreRows(rows); });
  if (!isReady())
    return false;
  if (rows.empty())
    return true;
  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return false;
  std::string q =
      "UPDATE " + quoteIdent(table_) + " SET " + timeAssign() + "" +
      (posCol_.empty() ? std::string() : ", " + quoteIdent(posCol_) + " = ?2") +
      (visibleCol_.empty() ? std::string()
                           : ", " + quoteIdent(visibleCol_) + " = ?3") +
      " WHERE titleId = ?4;";
  Stmt st;
  if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK) {
    error_ = describe(h.db, SQLITE_ERROR);
    return false;
  }
  if (!exec(h, "BEGIN IMMEDIATE;"))
    return false;
  for (const auto &r : rows) {
    sqlite3_reset(st.s);
    sqlite3_bind_text(st.s, 1, r.time.c_str(), -1, SQLITE_TRANSIENT);
    if (!posCol_.empty())
      sqlite3_bind_int(st.s, 2, r.uiPos);
    if (!visibleCol_.empty())
      sqlite3_bind_int(st.s, 3, r.visible ? 1 : 0);
    sqlite3_bind_text(st.s, 4, r.id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.s) != SQLITE_DONE) {
      error_ = describe(h.db, SQLITE_ERROR);
      exec(h, "ROLLBACK;");
      return false;
    }
  }
  return exec(h, "COMMIT;");
}

std::vector<CatalogDb::RowState>
CatalogDb::readLayout(const std::string &dbFile) {
  std::vector<RowState> rows;
  if (!isReady())
    return rows;
  installLibcWorkarounds();
  Handle h;
  if (sqlite3_open_v2(dbFile.c_str(), &h.db, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    error_ = "cannot open " + dbFile;
    return rows;
  }
  std::string q =
      "SELECT titleId, " + quoteIdent(timeCol_) + ", " +
      (posCol_.empty() ? std::string("0") : quoteIdent(posCol_)) + ", " +
      (visibleCol_.empty() ? std::string("1") : quoteIdent(visibleCol_)) +
      " FROM " + quoteIdent(table_) + ";";
  Stmt st;
  if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK) {
    error_ = describe(h.db, SQLITE_ERROR);
    return rows;
  }
  while (sqlite3_step(st.s) == SQLITE_ROW) {
    RowState r;
    r.id = textOr(st.s, 0, "");
    r.time = textOr(st.s, 1, "");
    r.uiPos = sqlite3_column_int(st.s, 2);
    r.visible = sqlite3_column_int(st.s, 3) != 0;
    if (!r.id.empty() && !r.time.empty())
      rows.push_back(std::move(r));
  }
  return rows;
}

std::vector<std::string> CatalogDb::allTitleIds() {
  std::vector<std::string> ids;
  if (!isReady())
    return ids;
  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return ids;
  std::string q = "SELECT titleId FROM " + quoteIdent(table_) + ";";
  Stmt st;
  if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK) {
    error_ = describe(h.db, SQLITE_ERROR);
    return ids;
  }
  while (sqlite3_step(st.s) == SQLITE_ROW)
    ids.push_back(textOr(st.s, 0, ""));
  return ids;
}

bool CatalogDb::dumpNonTitleRows(const std::string &path) {
  if (!isReady())
    return false;
  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return false;
  std::string q = "SELECT * FROM " + quoteIdent(table_) + ";";
  Stmt st;
  if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK)
    return false;
  FILE *f = std::fopen(path.c_str(), "w");
  if (!f)
    return false;
  std::fprintf(f, "table %s\n", table_.c_str());
  for (auto &t : tables_)
    std::fprintf(f, "available %s\n", t.c_str());
  int cols = sqlite3_column_count(st.s);
  for (int c = 0; c < cols; ++c)
    std::fprintf(f, "%s%s", c ? "\t" : "", sqlite3_column_name(st.s, c));
  std::fprintf(f, "\n");
  int rows = 0;
  while (sqlite3_step(st.s) == SQLITE_ROW && rows < 400) {
    std::string id;
    for (int c = 0; c < cols; ++c)
      if (std::strcmp(sqlite3_column_name(st.s, c), "titleId") == 0)
        id = textOr(st.s, c, "");
    if (isPlainTitleId(id))
      continue; // only the unusual rows are of interest
    for (int c = 0; c < cols; ++c) {
      std::string v = sqlite3_column_type(st.s, c) == SQLITE_BLOB
                          ? "<blob>"
                          : textOr(st.s, c, "");
      if (v.size() > 120)
        v.resize(120);
      for (auto &ch : v)
        if (ch == '\t' || ch == '\n' || ch == '\r')
          ch = ' ';
      std::fprintf(f, "%s%s", c ? "\t" : "", v.c_str());
    }
    std::fprintf(f, "\n");
    ++rows;
  }
  std::fclose(f);
  return true;
}

bool CatalogDb::commitSequence(const std::vector<SeqRow> &rows) {
  if (!mirroring_)
    return mirrored([&] { return commitSequence(rows); });
  if (!isReady())
    return false;
  if (rows.empty())
    return true;
  if (posCol_.empty()) {
    error_ = "No position column in " + table_;
    return false;
  }
  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return false;
  // UPDATE tbl SET ui_pos = ?, last_launch_time = ? WHERE titleId = ?
  std::string q = "UPDATE " + quoteIdent(table_) + " SET " +
                  quoteIdent(posCol_) + " = ?2, " + timeAssign() +
                  " WHERE titleId = ?3;";
  Stmt st;
  if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK) {
    error_ = describe(h.db, SQLITE_ERROR);
    return false;
  }
  if (!exec(h, "BEGIN IMMEDIATE;"))
    return false;
  for (const auto &r : rows) {
    sqlite3_reset(st.s);
    sqlite3_bind_text(st.s, 1, r.time.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st.s, 2, r.uiPos);
    sqlite3_bind_text(st.s, 3, r.id.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st.s) != SQLITE_DONE) {
      error_ = describe(h.db, SQLITE_ERROR);
      exec(h, "ROLLBACK;");
      return false;
    }
  }
  return exec(h, "COMMIT;");
}

bool CatalogDb::isMarkerTime(const std::string &t) {
  if (t.size() < 16 || t[4] != '-' || t[7] != '-' || t[10] != ' ' ||
      t[13] != ':')
    return true; // not a date
  if (t.rfind("2099", 0) == 0 || t.rfind("2000-01-01", 0) == 0)
    return true; // PS4Hub front / end marks
  if (t.rfind("2020-01-01 00:00:00", 0) == 0)
    return true; // PS4Hub fallback value
  return false;
}

std::map<std::string, std::string> CatalogDb::realTimes() {
  std::map<std::string, std::string> out;
  if (!isReady())
    return out;
  Handle h;
  if (!openHandle(dbPath_, &h.db, error_))
    return out;
  std::vector<std::string> cols;
  for (const std::string &c :
       {timeCol_, time2Col_, std::string("lastAccessTime"),
        std::string("last_launch_time"), std::string("lastUpdateTime"),
        std::string("installDate"), std::string("mTime")})
    if (!c.empty() && std::find(cols.begin(), cols.end(), c) == cols.end() &&
        hasColumn(h, c))
      cols.push_back(c);
  for (const auto &c : cols) {
    Stmt st;
    std::string q = "SELECT titleId, " + quoteIdent(c) + " FROM " +
                    quoteIdent(table_) + ";";
    if (sqlite3_prepare_v2(h.db, q.c_str(), -1, &st.s, nullptr) != SQLITE_OK)
      continue;
    while (sqlite3_step(st.s) == SQLITE_ROW) {
      std::string id = textOr(st.s, 0, ""), v = textOr(st.s, 1, "");
      if (id.empty() || out.count(id) || isMarkerTime(v))
        continue;
      out[id] = v; // earlier columns win (the real launch time first)
    }
  }
  return out;
}
