#include "app_controller.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>

#include "config.h"
#include "platform.h"
#include "sfo_parser.h"
#include "title_inspector.h"

#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

AppController::AppController() : db_(DEFAULT_DB_PATH) {}

void AppController::setStatus(const std::string &s, bool error) {
  status_ = s;
  statusError_ = error;
  ++statusSerial_;
}

namespace {

std::string lowerAscii(std::string s) {
  for (auto &c : s)
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
  return s;
}

// Built-in home screen tiles the user may want gone, matched by id or name.
bool isBuiltinTile(const AppItem &it) {
  if (it.id.rfind("NPXS", 0) != 0)
    return false;
  if (it.id == "NPXS20001")
    return true; // PlayStation Store
  std::string n = lowerAscii(it.name);
  return n.find("what's new") != std::string::npos ||
         n.find("whats new") != std::string::npos ||
         n.find("what\xE2\x80\x99s new") != std::string::npos ||
         n.find("playstation store") != std::string::npos ||
         n.find("playstation\xE2\x84\xA2store") != std::string::npos ||
         n.find("playstation\xE2\x84\xA2 store") != std::string::npos;
}

// Original (launch time, ui_pos, visible) of every tile PS4Hub has touched,
// written before the first change, so "Restore default layout" can put each
// tile back exactly as the system had it.
struct Original {
  std::string time;
  int uiPos = 0;
  bool visible = true;
};
const char *kJournal = APP_WORK_DIR "/layout_journal.tsv";

std::map<std::string, Original> loadJournal() {
  std::map<std::string, Original> j;
  FILE *f = std::fopen(kJournal, "rb");
  if (!f)
    return j;
  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    char id[64] = {0}, date[32] = {0}, time[32] = {0};
    int pos = 0, vis = 1;
    // id <TAB> YYYY-MM-DD <SP> hh:mm:ss.mmm <TAB> pos <TAB> visible
    if (std::sscanf(line, "%63[^\t]\t%31s %31[^\t]\t%d\t%d", id, date, time,
                    &pos, &vis) == 5)
      j[id] = Original{std::string(date) + " " + time, pos, vis != 0};
  }
  std::fclose(f);
  return j;
}

bool saveJournal(const std::map<std::string, Original> &j) {
  mkdir(APP_WORK_DIR, 0777);
  std::string tmp = std::string(kJournal) + ".tmp";
  FILE *f = std::fopen(tmp.c_str(), "wb");
  if (!f)
    return false;
  for (auto &kv : j)
    std::fprintf(f, "%s\t%s\t%d\t%d\n", kv.first.c_str(),
                 kv.second.time.c_str(), kv.second.uiPos,
                 kv.second.visible ? 1 : 0);
  bool ok = std::fclose(f) == 0;
  return ok && std::rename(tmp.c_str(), kJournal) == 0;
}

// Fixed positions file: one title id per line, line 1 = position 1.
const char *kSlotsFile = APP_WORK_DIR "/fixed_positions.txt";
const char *kDefaultDb = APP_WORK_DIR "/default_layout.db";
const char *kDefaultSlots = APP_WORK_DIR "/default_positions.txt";

int64_t parseTime(const std::string &s);
std::string formatTime(int64_t t);

bool copyBytes(const char *src, const char *dst) {
  FILE *in = std::fopen(src, "rb");
  if (!in)
    return false;
  std::string tmp = std::string(dst) + ".tmp";
  FILE *out = std::fopen(tmp.c_str(), "wb");
  if (!out) {
    std::fclose(in);
    return false;
  }
  char buf[64 * 1024];
  size_t n;
  bool ok = true;
  while ((n = std::fread(buf, 1, sizeof(buf), in)) > 0)
    if (std::fwrite(buf, 1, n, out) != n) {
      ok = false;
      break;
    }
  std::fclose(in);
  ok = (std::fclose(out) == 0) && ok;
  if (!ok) {
    unlink(tmp.c_str());
    return false;
  }
  return std::rename(tmp.c_str(), dst) == 0;
}

// Add-ons and patches share the catalog table but are not home screen tiles.
bool isAddonOrPatch(const std::string &category) {
  return category.rfind("ac", 0) == 0 || category.rfind("gp", 0) == 0;
}

// True when every character of a UTF-8 string is one the UI font draws
// (Latin, Greek, Cyrillic, punctuation, currency, letterlike symbols).
bool fontCanDraw(const std::string &u) {
  for (size_t i = 0; i < u.size();) {
    unsigned char c = (unsigned char)u[i];
    uint32_t cp;
    int n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c >> 5) == 6) {
      cp = c & 0x1F;
      n = 2;
    } else if ((c >> 4) == 14) {
      cp = c & 0x0F;
      n = 3;
    } else
      return false; // 4-byte (emoji etc.) or broken
    if (i + n > u.size())
      return false;
    for (int k = 1; k < n; ++k)
      cp = (cp << 6) | ((unsigned char)u[i + k] & 0x3F);
    bool ok = (cp >= 0x20 && cp <= 0x24F) || (cp >= 0x370 && cp <= 0x4FF) ||
              (cp >= 0x2010 && cp <= 0x205E) ||
              (cp >= 0x20A0 && cp <= 0x20BF) ||
              (cp >= 0x2100 && cp <= 0x2153) || (cp >= 0x2190 && cp <= 0x2199);
    if (!ok)
      return false;
    i += (size_t)n;
  }
  return true;
}

// app.db stores some symbols (e.g. the trademark sign) as '?'. The home
// screen takes titles from the game's param.sfo, so do the same for names
// that contain '?'. Cached per run; only drawable titles are used.
std::string sfoTitle(const std::string &id) {
  static std::map<std::string, std::string> cache;
  auto it = cache.find(id);
  if (it != cache.end())
    return it->second;
  std::string title;
  SfoFile sfo;
  if (Sfo::loadInstalled(id, false, sfo))
    title = sfo.str("TITLE");
  if (!fontCanDraw(title))
    title.clear();
  cache[id] = title;
  return title;
}

} // namespace

void AppController::reload() {
  loaded_ = true;
  loadSlots();
  if (!db_.isReady() && !db_.open(Platform::userId())) {
    items_.clear();
    hidden_.clear();
    setStatus("Cannot open app database: " + db_.lastError(), true);
    return;
  }

  std::string keepId = currentItem() ? currentItem()->id : "";
  items_.clear();
  hidden_.clear();
  leftovers_.clear();
  builtinIds_.clear();
  const Platform::AppInstApi *ai = Platform::appInst();
  std::set<std::string> seen;

  static bool dumped = false;
  if (!dumped) { // one diagnostic snapshot per run (for folder rows)
    dumped = true;
    mkdir(APP_WORK_DIR, 0777);
    db_.dumpNonTitleRows(APP_WORK_DIR "/catalog_rows.txt");
  }

  for (auto &e : db_.fetchEntries()) {
    if (isAddonOrPatch(e.category))
      continue;
    // Folders are stored under their own ids (not ABCD12345). Any row
    // that is neither a regular title nor a content id (add-on / patch
    // rows such as "UP0000-CUSA00000_00-...") is a home screen container.
    if (!e.isFolder && !CatalogDb::isPlainTitleId(e.titleId)) {
      if (e.titleId.find('-') != std::string::npos)
        continue;
      e.isFolder = true;
    }
    if (!seen.insert(e.titleId).second)
      continue; // duplicate row

    // Rows left behind by uninstalled apps: not on the home screen.
    if (!e.isFolder && ai && ai->exists && e.titleId.rfind("NPXS", 0) != 0) {
      int32_t exists = 1;
      if (ai->exists(e.titleId.c_str(), &exists) == 0 && !exists) {
        leftovers_.push_back(e.titleId);
        continue;
      }
    }

    AppItem it;
    it.id = e.titleId;
    it.name = e.titleName;
    if (it.name.empty() ||
        it.name.find_first_not_of('?') == std::string::npos) {
      // Folders keep their real name elsewhere; never show a bare "?".
      if (e.isFolder) {
        size_t us = e.titleId.find('_');
        std::string tail =
            us == std::string::npos ? std::string() : e.titleId.substr(us + 1);
        it.name =
            "Folder" + (tail.empty() ? std::string() : " " + tail.substr(0, 6));
      } else {
        it.name = e.titleId;
      }
    }
    if (!e.isFolder && e.titleName.find('?') != std::string::npos &&
        CatalogDb::isPlainTitleId(it.id)) {
      std::string t = sfoTitle(it.id);
      if (!t.empty() && t.find('?') == std::string::npos)
        it.name = t;
    }
    {
      std::string shown =
          Sfo::cleanTitle(it.name); // no TM sign, no '?', only drawable text
      it.name = shown.empty() ? it.id : shown;
    }
    it.date = e.lastLaunch;
    it.pos = e.uiPos;
    // Folder rows carry visible = 0 in the catalog yet show on the home
    // screen, so they always belong to the home list.
    it.visible = e.isFolder ? true : e.visible;
    it.isFolder = e.isFolder;
    it.synthetic = e.lastLaunch.rfind("2099-", 0) == 0;
    it.sentToEnd = e.lastLaunch.rfind("2000-01-01", 0) == 0 ||
                   e.uiPos >= 900; // "sticky end"
    for (size_t k = 0; k < slots_.size(); ++k)
      if (slots_[k] == it.id)
        it.fixedSlot = (int)k + 1;
    it.pinned = it.fixedSlot > 0;
    if (isBuiltinTile(it))
      builtinIds_.push_back(it.id);
    (it.visible ? items_ : hidden_).push_back(std::move(it));
  }

  int shown = 0, total = (int)builtinIds_.size();
  for (auto &it : items_)
    if (std::find(builtinIds_.begin(), builtinIds_.end(), it.id) !=
        builtinIds_.end())
      ++shown;
  defaultTiles_ = total == 0       ? DefaultTiles::Missing
                  : shown == total ? DefaultTiles::Visible
                  : shown == 0     ? DefaultTiles::Hidden
                                   : DefaultTiles::Mixed;

  for (auto it = marked_.begin(); it != marked_.end();) {
    bool found = std::any_of(items_.begin(), items_.end(),
                             [&](const AppItem &a) { return a.id == *it; });
    it = found ? std::next(it) : marked_.erase(it);
  }

  cursor_ = 0;
  for (size_t i = 0; i < items_.size(); ++i)
    if (items_[i].id == keepId)
      cursor_ = (int)i;
  cursorChanged_ = true;

  // First run: remember the console's own layout so "Restore default
  // layout" can always go back to it.
  if (!hasSavedDefault()) {
    mkdir(APP_WORK_DIR, 0777);
    copyBytes(DEFAULT_DB_PATH, kDefaultDb);
  }

  // Fixed tiles drop out of their slot whenever the system records a new
  // launch; put them back (and forget tiles that no longer exist).
  // Slots may have gaps (""): a tile fixed at position 5 with nothing fixed
  // at 3-4 lets the most recently played tiles fill those places.
  if (!applyingSlots_ && !slots_.empty()) {
    bool pruned = false, drift = false;
    for (auto &id : slots_) {
      if (id.empty())
        continue;
      bool present = std::any_of(items_.begin(), items_.end(),
                                 [&](const AppItem &a) { return a.id == id; });
      if (!present) {
        id.clear();
        pruned = true;
      } // keep the others where they are
    }
    while (!slots_.empty() && slots_.back().empty())
      slots_.pop_back();
    for (size_t k = 0; k < slots_.size() && k < items_.size(); ++k) {
      if (slots_[k].empty())
        continue;
      if (items_[k].id != slots_[k])
        drift = true; // fixed tile not at its place
    }
    if (pruned)
      saveSlots();
    if (drift || pruned) {
      applyingSlots_ = true;
      applySlots();
      reload();
      applyingSlots_ = false;
      return;
    }
  }

  layoutChanges_ = loadJournal().size() + (size_t)fixedCount();
  if (!layoutChanges_)
    for (auto *list : {&items_, &hidden_})
      for (auto &it : *list)
        if (it.synthetic || it.sentToEnd)
          ++layoutChanges_;

  if (items_.empty() && !db_.lastError().empty())
    setStatus(db_.lastError(), true);
  else
    setStatus("Library loaded: " + std::to_string(items_.size()) +
              " items on the home screen");
}

const AppItem *AppController::currentItem() const {
  if (cursor_ < 0 || cursor_ >= (int)items_.size())
    return nullptr;
  return &items_[cursor_];
}

bool AppController::canUninstallCurrent() const {
  const AppItem *it = currentItem();
  if (!it || it->isFolder)
    return false;
  if (!CatalogDb::isPlainTitleId(it->id))
    return false;
  if (it->id.rfind("NPXS", 0) == 0)
    return false; // system applications
  return it->id != APP_TITLE_ID;
}

void AppController::moveCursor(int delta) {
  if (items_.empty())
    return;
  int n = (int)items_.size();
  int next = std::max(0, std::min(n - 1, cursor_ + delta));
  if (next != cursor_) {
    cursor_ = next;
    cursorChanged_ = true;
  }
}

void AppController::nextTab(int delta) {
  if (reorderMode_)
    return; // L1/R1 move items while reordering
  int n = (int)Tab::Count;
  tab_ = (Tab)(((int)tab_ + delta + n) % n);
}

void AppController::toggleMark() {
  const AppItem *it = currentItem();
  if (!it)
    return;
  if (!marked_.erase(it->id))
    marked_.insert(it->id);
}

std::vector<std::string> AppController::selectionInListOrder() const {
  std::vector<std::string> ids;
  for (auto &it : items_)
    if (marked_.count(it.id))
      ids.push_back(it.id);
  if (ids.empty() && currentItem())
    ids.push_back(currentItem()->id);
  return ids;
}

bool AppController::backupFirst() {
  if (!db_.backup(BACKUP_DB_PATH)) {
    setStatus(db_.lastError() + " - change aborted", true);
    return false;
  }
  return true;
}

void AppController::pinSelectionToTop() { setFixedPosition(1); }

void AppController::sendSelectionToEnd() {
  auto ids = selectionInListOrder();
  if (ids.empty() || !backupFirst())
    return;
  // The DB layer gives the first id the oldest timestamp (= very last tile),
  // so reverse to keep the items' relative order at the end of the list.
  std::reverse(ids.begin(), ids.end());
  rememberOriginals(ids);
  if (unfix(ids)) {
    saveSlots();
    applySlots();
  }
  if (db_.commitOrder({}, ids)) {
    marked_.clear();
    reload();
    setStatus("Moved " + std::to_string(ids.size()) + " item(s) to the end");
  } else {
    setStatus("Move failed: " + db_.lastError(), true);
  }
}

void AppController::enterReorder() {
  if (items_.empty())
    return;
  orderBeforeReorder_ = items_;
  movedIds_.clear();
  reorderMode_ = true;
  tab_ = Tab::Library;
  setStatus("Reorder mode: L1/R1 move, Cross saves, Circle cancels");
}

void AppController::reorderMove(int delta) {
  if (!reorderMode_ || items_.empty())
    return;
  int target = cursor_ + delta;
  if (target < 0 || target >= (int)items_.size())
    return;
  movedIds_.insert(
      items_[cursor_].id); // only tiles the user grabbed count as moved
  std::swap(items_[cursor_], items_[target]);
  cursor_ = target;
  cursorChanged_ = true;
}

namespace {

constexpr int64_t kUnknownTime = INT64_MIN;

// Howard Hinnant's civil-date algorithms (proleptic Gregorian, UTC).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

void civilFromDays(int64_t z, int &y, unsigned &m, unsigned &d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp + (mp < 10 ? 3 : -9);
  y = (int)(yoe + era * 400 + (m <= 2));
}

// "YYYY-MM-DD hh:mm:ss[.mmm]" <-> milliseconds since the epoch.
int64_t parseTime(const std::string &s) {
  int y, mo, d, h, mi, sec, ms = 0;
  int n = std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d.%d", &y, &mo, &d, &h, &mi,
                      &sec, &ms);
  if (n < 6 || mo < 1 || mo > 12 || d < 1 || d > 31)
    return kUnknownTime;
  return ((daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec) * 1000) +
         ms;
}

std::string formatTime(int64_t t) {
  int64_t days = t >= 0 ? t / 86400000 : -((-t + 86399999) / 86400000);
  int64_t rem = t - days * 86400000;
  int y;
  unsigned m, d;
  civilFromDays(days, y, m, d);
  char b[40];
  std::snprintf(b, sizeof(b), "%04d-%02u-%02u %02d:%02d:%02d.%03d", y, m, d,
                (int)(rem / 3600000), (int)(rem / 60000 % 60),
                (int)(rem / 1000 % 60), (int)(rem % 1000));
  return b;
}

} // namespace

void AppController::commitReorder() {
  if (!reorderMode_)
    return;
  reorderMode_ = false;

  // Full sequence commit: the whole visible order is written in one
  // transaction - ui_pos 1, 2, 3... and launch times counting down one
  // minute per place from 2099-12-31 23:59, so both fields agree. Tiles
  // marked "sticky end" that were not moved keep ui_pos 900 + i and
  // 2000-01-01. The home screen sorts by ui_pos first, so old values
  // (such as 990) can no longer pin a tile to the back.
  const int n = (int)items_.size();
  bool changed = (int)orderBeforeReorder_.size() != n;
  for (int i = 0; !changed && i < n; ++i)
    changed = orderBeforeReorder_[i].id != items_[i].id;
  if (!changed) {
    movedIds_.clear();
    setStatus("Order unchanged");
    return;
  }

  // Fixed tiles keep being fixed, at the place they now occupy; tiles
  // moved into the front block become fixed too (as before).
  std::vector<std::string> newSlots;
  bool front = true;
  for (int i = 0; i < n; ++i) {
    const std::string &id = items_[i].id;
    bool fixedBefore =
        std::find(slots_.begin(), slots_.end(), id) != slots_.end();
    if (!fixedBefore && !movedIds_.count(id))
      front = false;
    if (fixedBefore || front) {
      if ((int)newSlots.size() <= i)
        newSlots.resize(i + 1);
      newSlots[i] = id;
    }
  }

  std::vector<std::string> ids;
  for (auto &it : items_)
    ids.push_back(it.id);
  std::set<std::string> notEnd(movedIds_.begin(), movedIds_.end());
  for (auto &id : newSlots)
    if (!id.empty())
      notEnd.insert(id);
  std::vector<CatalogDb::SeqRow> rows = sequenceRows(ids, notEnd);
  movedIds_.clear();

  if (!backupFirst()) {
    items_ = orderBeforeReorder_;
    return;
  }
  rememberOriginals(ids); // values come from the pre-reorder snapshot
  slots_ = newSlots;
  trimSlots();
  saveSlots();
  if (db_.commitSequence(rows)) {
    reload(); // also re-stamps fixed tiles with their slot times
    setStatus("New order saved (" + std::to_string(n) +
              " positions). The home screen updates after a moment.");
  } else {
    items_ = orderBeforeReorder_;
    setStatus("Saving order failed: " + db_.lastError(), true);
  }
}

void AppController::cancelReorder() {
  if (!reorderMode_)
    return;
  std::string keepId = currentItem() ? currentItem()->id : "";
  items_ = orderBeforeReorder_;
  reorderMode_ = false;
  movedIds_.clear();
  for (size_t i = 0; i < items_.size(); ++i)
    if (items_[i].id == keepId)
      cursor_ = (int)i;
  cursorChanged_ = true;
  setStatus("Reorder cancelled");
}

void AppController::toggleDefaultTiles() {
  if (builtinIds_.empty()) {
    setStatus("What's New and PlayStation Store tiles were not found", true);
    return;
  }
  bool show =
      defaultTiles_ == DefaultTiles::Hidden; // Visible or Mixed -> hide all
  if (!backupFirst())
    return;
  rememberOriginals(builtinIds_);
  if (db_.setVisible(builtinIds_, show)) {
    reload();
    setStatus(
        show ? "What's New and PlayStation Store are back on the home screen"
             : "What's New and PlayStation Store hidden from the home screen");
  } else {
    setStatus(std::string(show ? "Show" : "Hide") +
                  " failed: " + db_.lastError(),
              true);
  }
}

void AppController::hideSelection() {
  auto ids = selectionInListOrder();
  if (ids.empty() || !backupFirst())
    return;
  rememberOriginals(ids);
  if (db_.setVisible(ids, false)) {
    marked_.clear();
    reload();
    setStatus("Hidden " + std::to_string(ids.size()) +
              " tile(s) from the home screen");
  } else {
    setStatus("Hide failed: " + db_.lastError(), true);
  }
}

void AppController::showHidden(const std::string &id) {
  if (!backupFirst())
    return;
  rememberOriginals({id});
  if (db_.setVisible({id}, true)) {
    reload();
    setStatus("Tile is back on the home screen");
  } else {
    setStatus("Show failed: " + db_.lastError(), true);
  }
}

void AppController::removeLeftovers() {
  if (leftovers_.empty())
    return;
  size_t n = leftovers_.size();
  if (!backupFirst())
    return;
  if (db_.deleteRows(leftovers_)) {
    reload();
    setStatus("Removed " + std::to_string(n) +
              (n == 1 ? " leftover entry" : " leftover entries"));
  } else {
    setStatus("Cleanup failed: " + db_.lastError(), true);
  }
}

void AppController::uninstallCurrent() {
  if (!canUninstallCurrent()) {
    setStatus("This entry cannot be uninstalled", true);
    return;
  }
  std::string id = currentItem()->id;
  if (!backupFirst())
    return;

  // Snapshot the catalog so we can detect wholesale damage after the call
  // (seen on some firmwares: sceAppInstUtilAppUnInstall of one app wipes
  // most / all rows from the shell's browse tables).
  size_t rowsBefore = db_.allTitleIds().size();

  int32_t rc = Platform::uninstallTitle(id);
  if (rc == 0) {
    // Verify the catalog is still intact. If almost everything vanished,
    // restore the backup we took at the top of this function.
    size_t rowsAfter = db_.allTitleIds().size();
    bool catastrophic = rowsBefore >= 3 && rowsAfter + 1 < rowsBefore / 2;
    if (catastrophic) {
      if (db_.restore(BACKUP_DB_PATH)) {
        reload();
        setStatus("Uninstall damaged app.db - restored backup. Reboot PS4 if "
                  "tiles look wrong.",
                  true);
      } else {
        setStatus("Uninstall damaged app.db AND backup restore failed: " +
                      db_.lastError(),
                  true);
      }
      return;
    }
    // Only touch our row; do NOT mirrored-delete across every browse
    // table (the shell writes those itself, mirroring can race with it).
    TitleInspector::instance().invalidate(id);
    reload();
    setStatus("Uninstalled " + id);
    return;
  }
  if (db_.purgeApplication(id)) {
    reload();
    char buf[96];
    std::snprintf(buf, sizeof(buf),
                  "Removed %s (AppInstUtil 0x%08X, manual cleanup used)",
                  id.c_str(), (unsigned)rc);
    setStatus(buf);
  } else {
    setStatus("Uninstall failed: " + db_.lastError(), true);
  }
}

void AppController::restoreBackup() {
  if (db_.restore(BACKUP_DB_PATH)) {
    reload();
    setStatus("app.db restored from backup");
  } else
    setStatus(db_.lastError(), true);
}

const AppItem *AppController::findAny(const std::string &id) const {
  for (auto *list : {&items_, &hidden_, &orderBeforeReorder_})
    for (auto &it : *list)
      if (it.id == id)
        return &it;
  return nullptr;
}

void AppController::rememberOriginals(const std::vector<std::string> &ids) {
  auto j = loadJournal();
  bool changed = false;
  for (const auto &id : ids) {
    if (j.count(id))
      continue; // keep the very first original
    const AppItem *it = findAny(id);
    if (!it || it->date.empty())
      continue;
    // Values already written by an older PS4Hub are not "originals".
    if (it->synthetic || it->sentToEnd)
      continue;
    j[id] = Original{it->date, it->pos, it->visible};
    changed = true;
  }
  if (changed)
    saveJournal(j);
}

void AppController::restoreDefaultLayout() {
  if (hasSavedDefault() &&
      restoreSnapshot(kDefaultDb, kDefaultSlots, "Default layout")) {
    unlink(kJournal); // back at the baseline
    return;
  }
  // No snapshot: undo what the journal remembers.
  std::vector<CatalogDb::RowState> rows;
  auto j = loadJournal();
  for (auto &kv : j)
    rows.push_back(
        {kv.first, kv.second.time, kv.second.uiPos, kv.second.visible});
  for (auto *list : {&items_, &hidden_})
    for (auto &it : *list)
      if ((it.synthetic || it.sentToEnd) && !j.count(it.id))
        rows.push_back({it.id, "2020-01-01 00:00:00.000", 0, it.visible});
  if (rows.empty()) {
    setStatus("The layout is already the default");
    return;
  }
  if (!backupFirst())
    return;
  if (!db_.restoreRows(rows)) {
    setStatus("Restore failed: " + db_.lastError(), true);
    return;
  }
  unlink(kJournal);
  slots_.clear();
  saveSlots();
  reorderMode_ = false;
  marked_.clear();
  reload();
  setStatus("Default layout restored. The home screen updates after a moment.");
}

// Puts back the order, positions and visibility stored in a copy of app.db
// plus the fixed positions saved next to it.
bool AppController::restoreSnapshot(const std::string &dbFile,
                                    const std::string &slotsFile,
                                    const std::string &label) {
  std::vector<CatalogDb::RowState> rows = db_.readLayout(dbFile);
  if (rows.empty()) {
    setStatus(label + " could not be read", true);
    return false;
  }
  if (!backupFirst())
    return false;
  if (!db_.restoreRows(rows)) {
    setStatus("Restore failed: " + db_.lastError(), true);
    return false;
  }
  slots_.clear();
  if (FILE *f = std::fopen(slotsFile.c_str(), "rb")) {
    char line[128];
    while (std::fgets(line, sizeof(line), f)) {
      std::string id(line);
      while (!id.empty() && (id.back() == '\n' || id.back() == '\r'))
        id.pop_back();
      slots_.push_back(id); // empty line = free position
    }
    std::fclose(f);
  }
  trimSlots();
  saveSlots();
  reorderMode_ = false;
  marked_.clear();
  reload();
  setStatus(label + " loaded. The home screen updates after a moment.");
  return true;
}

// Extra saved layouts next to the default one (which is never overwritten).
// profiles/index.txt: "<id>\t<name>" per line; profile_<id>.db is a copy of
// app.db, profile_<id>.slots the fixed positions at that moment.

namespace {
const char *kProfileDir = APP_WORK_DIR "/profiles";
std::string profileDb(int id) {
  return std::string(kProfileDir) + "/profile_" + std::to_string(id) + ".db";
}
std::string profileSlots(int id) {
  return std::string(kProfileDir) + "/profile_" + std::to_string(id) + ".slots";
}
std::string profileIndex() { return std::string(kProfileDir) + "/index.txt"; }

void writeProfileIndex(const std::vector<LayoutProfile> &list) {
  mkdir(APP_WORK_DIR, 0777);
  mkdir(kProfileDir, 0777);
  std::string tmp = profileIndex() + ".tmp";
  FILE *f = std::fopen(tmp.c_str(), "wb");
  if (!f)
    return;
  for (auto &p : list)
    std::fprintf(f, "%d\t%s\n", p.id, p.name.c_str());
  if (std::fclose(f) == 0)
    std::rename(tmp.c_str(), profileIndex().c_str());
}
} // namespace

std::vector<LayoutProfile> AppController::profiles() const {
  std::vector<LayoutProfile> out;
  FILE *f = std::fopen(profileIndex().c_str(), "rb");
  if (!f)
    return out;
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    std::string l(line);
    while (!l.empty() && (l.back() == '\n' || l.back() == '\r'))
      l.pop_back();
    size_t tab = l.find('\t');
    if (tab == std::string::npos)
      continue;
    LayoutProfile p;
    p.id = std::atoi(l.substr(0, tab).c_str());
    p.name = l.substr(tab + 1);
    if (p.id > 0)
      out.push_back(p);
  }
  std::fclose(f);
  return out;
}

void AppController::saveProfile(int id) {
  auto list = profiles();
  bool isNew = id <= 0;
  if (isNew) {
    id = 1;
    for (auto &p : list)
      id = std::max(id, p.id + 1);
  }
  mkdir(APP_WORK_DIR, 0777);
  mkdir(kProfileDir, 0777);
  if (!copyBytes(DEFAULT_DB_PATH, profileDb(id).c_str())) {
    setStatus("Could not save the profile", true);
    return;
  }
  if (FILE *f = std::fopen(profileSlots(id).c_str(), "wb")) {
    for (auto &sid : slots_)
      std::fprintf(f, "%s\n", sid.c_str());
    std::fclose(f);
  }
  char when[32] = "";
  std::time_t now = std::time(nullptr);
  std::tm lt{};
  localtime_r(&now, &lt);
  std::strftime(when, sizeof(when), "%d %b %H:%M", &lt);
  std::string name = "Profile " + std::to_string(id) + " - " + when;
  bool found = false;
  for (auto &p : list)
    if (p.id == id) {
      p.name = name;
      found = true;
    }
  if (!found)
    list.push_back({id, name});
  writeProfileIndex(list);
  setStatus(isNew ? "Saved as " + name
                  : name + " updated with the current layout");
}

void AppController::loadProfile(int id) {
  for (auto &p : profiles())
    if (p.id == id) {
      restoreSnapshot(profileDb(id), profileSlots(id), p.name);
      return;
    }
  setStatus("Profile not found", true);
}

void AppController::deleteProfile(int id) {
  auto list = profiles();
  std::string name;
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const LayoutProfile &p) {
                              if (p.id == id)
                                name = p.name;
                              return p.id == id;
                            }),
             list.end());
  unlink(profileDb(id).c_str());
  unlink(profileSlots(id).c_str());
  writeProfileIndex(list);
  setStatus(name.empty() ? "Profile deleted" : name + " deleted");
}

// Undoes every ordering change: all tiles get the same (system default)
// position value and their genuine last-played time back, so the console
// orders the home screen by itself again. Visibility is left alone.
void AppController::resetToSystemOrder() {
  if (!db_.isReady())
    return;
  std::vector<CatalogDb::RowState> current = db_.readLayout(DEFAULT_DB_PATH);
  if (current.empty()) {
    setStatus("Cannot read app.db: " + db_.lastError(), true);
    return;
  }
  std::vector<CatalogDb::RowState> snapshot;
  if (hasSavedDefault())
    snapshot = db_.readLayout(kDefaultDb);
  auto journal = loadJournal();
  auto real = db_.realTimes();

  // The position value the system gives untouched tiles: the most common
  // one among rows that still carry a genuine time.
  std::map<int, int> votes;
  for (auto *list : {&snapshot, &current})
    for (auto &r : *list)
      if (!CatalogDb::isMarkerTime(r.time) && r.uiPos < 900)
        ++votes[r.uiPos];
  for (auto &kv : journal)
    if (!CatalogDb::isMarkerTime(kv.second.time) && kv.second.uiPos < 900)
      ++votes[kv.second.uiPos];
  int neutral = 0, best = -1;
  for (auto &kv : votes)
    if (kv.second > best) {
      best = kv.second;
      neutral = kv.first;
    }

  std::map<std::string, std::string> snapTime;
  for (auto &r : snapshot)
    if (!CatalogDb::isMarkerTime(r.time))
      snapTime[r.id] = r.time;

  std::vector<CatalogDb::SeqRow> rows;
  int guessed = 0;
  for (auto &r : current) {
    std::string t;
    auto j = journal.find(r.id);
    if (j != journal.end() && !CatalogDb::isMarkerTime(j->second.time))
      t = j->second.time;
    else if (snapTime.count(r.id))
      t = snapTime[r.id];
    else if (real.count(r.id))
      t = real[r.id];
    else if (!CatalogDb::isMarkerTime(r.time))
      t = r.time;
    else {
      t = "2015-01-01 00:00:00.000";
      ++guessed;
    } // unknown: behind everything played
    rows.push_back({r.id, t, neutral});
  }
  if (!backupFirst())
    return;
  if (!db_.commitSequence(rows)) {
    setStatus("Reset failed: " + db_.lastError(), true);
    return;
  }
  slots_.clear();
  saveSlots();
  unlink(kJournal);
  reorderMode_ = false;
  marked_.clear();
  reload();
  std::string msg = "System order restored (" + std::to_string(rows.size()) +
                    " entries, position " + std::to_string(neutral) + ")";
  if (guessed)
    msg += "; " + std::to_string(guessed) + " without a known date go last";
  setStatus(msg + ". The home screen updates after a moment.");
}

bool AppController::hasSavedDefault() const {
  struct stat st;
  return ::stat(kDefaultDb, &st) == 0;
}

void AppController::loadSlots() {
  slots_.clear();
  FILE *f = std::fopen(kSlotsFile, "rb");
  if (!f)
    return;
  char line[128];
  while (std::fgets(line, sizeof(line), f)) {
    std::string id(line);
    while (!id.empty() && (id.back() == '\n' || id.back() == '\r'))
      id.pop_back();
    if (!id.empty() &&
        std::find(slots_.begin(), slots_.end(), id) != slots_.end())
      id.clear();
    slots_.push_back(id); // empty line = free position
  }
  std::fclose(f);
  trimSlots();
}

void AppController::trimSlots() {
  while (!slots_.empty() && slots_.back().empty())
    slots_.pop_back();
}

bool AppController::unfix(const std::vector<std::string> &ids) {
  bool any = false;
  for (auto &s : slots_)
    if (!s.empty() && std::find(ids.begin(), ids.end(), s) != ids.end()) {
      s.clear();
      any = true;
    }
  trimSlots();
  return any;
}

int AppController::fixedCount() const {
  int n = 0;
  for (auto &s : slots_)
    if (!s.empty())
      ++n;
  return n;
}

int AppController::firstFreeSlot() const {
  for (size_t k = 0; k < slots_.size(); ++k)
    if (slots_[k].empty())
      return (int)k + 1;
  return (int)slots_.size() + 1;
}

void AppController::saveSlots() const {
  mkdir(APP_WORK_DIR, 0777);
  if (slots_.empty()) {
    unlink(kSlotsFile);
    return;
  }
  std::string tmp = std::string(kSlotsFile) + ".tmp";
  FILE *f = std::fopen(tmp.c_str(), "wb");
  if (!f)
    return;
  for (auto &id : slots_)
    std::fprintf(f, "%s\n", id.c_str());
  if (std::fclose(f) == 0)
    std::rename(tmp.c_str(), kSlotsFile);
}

// Writes the whole home screen order as one sequence (ui_pos 1, 2, 3...
// with launch times counting down from 2099-12-31 23:59), placing every
// fixed tile exactly at its position. Tiles not fixed keep their current
// relative order around them; "sticky end" tiles stay at the back
// (ui_pos 900 + i, 2000-01-01).
bool AppController::applySlots(const std::vector<std::string> *preferred) {
  if ((!preferred && fixedCount() == 0) || items_.empty())
    return true;
  auto isFixed = [&](const std::string &id) {
    return std::find(slots_.begin(), slots_.end(), id) != slots_.end();
  };
  std::vector<const AppItem *> source;
  if (preferred) {
    for (auto &id : *preferred)
      for (auto &it : items_)
        if (it.id == id) {
          source.push_back(&it);
          break;
        }
    for (auto &it :
         items_) // anything the caller did not list keeps its place at the back
      if (std::find(preferred->begin(), preferred->end(), it.id) ==
          preferred->end())
        source.push_back(&it);
  } else {
    for (auto &it : items_)
      source.push_back(&it);
  }
  std::vector<const AppItem *> normal;
  for (auto *it : source)
    if (!isFixed(it->id))
      normal.push_back(it);
  std::vector<std::string> order;
  size_t ni = 0;
  for (size_t pos = 0; pos < slots_.size() || ni < normal.size(); ++pos) {
    const AppItem *fixed = nullptr;
    if (pos < slots_.size() && !slots_[pos].empty())
      fixed = findAny(slots_[pos]);
    if (fixed && fixed->visible) {
      order.push_back(fixed->id);
      continue;
    }
    if (ni < normal.size())
      order.push_back(normal[ni++]->id);
  }
  std::set<std::string> notEnd(releasedEnd_);
  for (auto &id : slots_)
    if (!id.empty())
      notEnd.insert(id);
  std::vector<CatalogDb::SeqRow> rows = sequenceRows(order, notEnd);
  if (!db_.commitSequence(rows)) {
    setStatus("Saving fixed positions failed: " + db_.lastError(), true);
    return false;
  }
  return true;
}

void AppController::setFixedPosition(int slot) {
  auto ids = selectionInListOrder();
  if (ids.empty() || !backupFirst())
    return;
  rememberOriginals(ids);
  unfix(ids);
  int at = std::max(1, std::min(slot, std::max(1, (int)items_.size()))) - 1;
  // A free place is simply taken; an occupied one pushes the fixed tiles
  // behind it one place back (up to the next free place).
  for (size_t i = 0; i < ids.size(); ++i) {
    size_t pos = (size_t)at + i;
    if (pos >= slots_.size())
      slots_.resize(pos + 1);
    if (!slots_[pos].empty()) {
      size_t gap = pos;
      while (gap < slots_.size() && !slots_[gap].empty())
        ++gap;
      if (gap < slots_.size())
        slots_.erase(slots_.begin() + gap);
      slots_.insert(slots_.begin() + pos, std::string());
    }
    slots_[pos] = ids[i];
  }
  trimSlots();
  saveSlots();
  if (applySlots()) {
    marked_.clear();
    reload();
    setStatus(ids.size() == 1
                  ? "Fixed at position " + std::to_string(at + 1) +
                        ". The home screen updates after a moment."
                  : std::to_string(ids.size()) + " tiles fixed from position " +
                        std::to_string(at + 1));
  }
}

// Real last-played time of every title PS4Hub knows: first value from the
// journal of originals, then the saved default layout, then any genuine
// date column still in app.db.
std::map<std::string, std::string> AppController::knownRealTimes() {
  std::map<std::string, std::string> out;
  for (auto &kv : loadJournal())
    if (!CatalogDb::isMarkerTime(kv.second.time))
      out[kv.first] = kv.second.time;
  if (hasSavedDefault())
    for (auto &r : db_.readLayout(kDefaultDb))
      if (!out.count(r.id) && !CatalogDb::isMarkerTime(r.time))
        out[r.id] = r.time;
  for (auto &kv : db_.realTimes())
    if (!out.count(kv.first))
      out[kv.first] = kv.second;
  return out;
}

// Unpin: the tile loses its fixed position (and a "front" / "end" pin) and
// goes back to where its last-played date puts it among the tiles that are
// not fixed. Fixed tiles stay exactly where they are.
void AppController::unpinSelection() {
  auto ids = selectionInListOrder();
  // Only tiles that are pinned (fixed position or stuck at the end) move.
  ids.erase(std::remove_if(ids.begin(), ids.end(),
                           [&](const std::string &id) {
                             const AppItem *it = findAny(id);
                             return !it ||
                                    (it->fixedSlot <= 0 && !it->sentToEnd);
                           }),
            ids.end());
  if (ids.empty()) {
    setStatus("Nothing to unpin");
    return;
  }
  if (!backupFirst())
    return;
  rememberOriginals(ids);
  unfix(ids);
  saveSlots();
  auto real = knownRealTimes();
  auto isFixed = [&](const std::string &id) {
    return std::find(slots_.begin(), slots_.end(), id) != slots_.end();
  };

  std::vector<std::string> order;
  for (auto &it : items_)
    if (std::find(ids.begin(), ids.end(), it.id) == ids.end())
      order.push_back(it.id);
  releasedEnd_.clear();
  for (auto &id : ids) {
    releasedEnd_.insert(id);
    const AppItem *me = findAny(id);
    auto rt = real.find(id);
    size_t at = order.size();
    // Before the first tile (not fixed, not stuck at the end) that was
    // played earlier; unknown dates go behind the played ones.
    for (size_t i = 0; i < order.size(); ++i) {
      const AppItem *other = findAny(order[i]);
      if (!other || isFixed(other->id))
        continue;
      if (other->sentToEnd) {
        at = std::min(at, i);
        break;
      }
      auto ot = real.find(order[i]);
      if (rt != real.end() && (ot == real.end() || ot->second < rt->second)) {
        at = i;
        break;
      }
    }
    (void)me;
    order.insert(order.begin() + (std::ptrdiff_t)at, id);
  }
  bool ok = applySlotsAll(order);
  releasedEnd_.clear();
  if (ok) {
    marked_.clear();
    reload();
    setStatus(
        ids.size() == 1
            ? "Unpinned - back to its last-played place. The home screen "
              "updates after a moment."
            : std::to_string(ids.size()) +
                  " tiles unpinned. The home screen updates after a moment.");
  }
}

std::vector<CatalogDb::SeqRow>
AppController::sequenceRows(const std::vector<std::string> &order,
                            const std::set<std::string> &notEnd) const {
  // Tiles keep the end marks only while they form the unbroken run at the
  // very back of the list; anything placed after them makes them ordinary.
  size_t endFrom = order.size();
  while (endFrom > 0) {
    const AppItem *it = findAny(order[endFrom - 1]);
    if (!it || !it->sentToEnd || notEnd.count(it->id))
      break;
    --endFrom;
  }
  const int64_t base = parseTime("2099-12-31 23:59:00.000");
  std::vector<CatalogDb::SeqRow> rows;
  for (size_t i = 0; i < order.size(); ++i) {
    if (i < endFrom)
      rows.push_back(
          {order[i], formatTime(base - (int64_t)i * 60000), (int)i + 1});
    else
      rows.push_back({order[i], "2000-01-01 00:00:00.000", 900 + (int)i});
  }
  return rows;
}

bool AppController::applySlotsAll(const std::vector<std::string> &order) {
  // applySlots with an explicit order writes even when nothing is fixed.
  return applySlots(&order);
}

void AppController::clearFixedPosition() {
  auto ids = selectionInListOrder();
  if (ids.empty())
    return;
  auto j = loadJournal();
  std::vector<std::pair<std::string, std::string>> writes;
  for (auto &id : ids) {
    auto it = std::find(slots_.begin(), slots_.end(), id);
    if (it == slots_.end())
      continue;
    it->clear(); // the other fixed tiles keep their numbers
    // Back to following "last played": original time if known.
    auto o = j.find(id);
    writes.emplace_back(id, o != j.end()
                                ? o->second.time
                                : std::string("2020-01-01 00:00:00.000"));
  }
  if (writes.empty())
    return;
  if (!backupFirst())
    return;
  trimSlots();
  saveSlots();
  if (db_.setLaunchTimes(writes) && applySlots()) {
    marked_.clear();
    reload();
    setStatus("Fixed position removed");
  } else {
    setStatus("Update failed: " + db_.lastError(), true);
  }
}

void AppController::requestReboot() {
  if (!Platform::reboot())
    setStatus("Restart is not available; restart the console manually", true);
}
