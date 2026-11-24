#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "catalog_db.h"

struct LayoutProfile {
  int id = 0;
  std::string name;
};

struct AppItem {
  std::string id;
  std::string name;
  std::string date;
  int pos = 0;
  bool visible = true;
  bool isFolder = false;
  bool pinned = false;    // has a fixed position (shown as "Pinned")
  bool synthetic = false; // launch time written by PS4Hub (2099 range)
  bool sentToEnd = false; // timestamp in the 2000 "end" range
  int fixedSlot = 0; // 1-based fixed home position, 0 = follows last played
};

enum class Tab {
  Library = 0,
  Downloads = 1,
  System = 2,
  Plugins = 3,
  Ntfs = 4,
  Count = 5
};

class AppController {
public:
  AppController();

  void reload();

  const std::vector<AppItem> &items() const { return items_; }
  int cursor() const { return cursor_; }
  bool isMarked(const std::string &id) const { return marked_.count(id) != 0; }
  size_t markedCount() const { return marked_.size(); }
  bool reorderMode() const { return reorderMode_; }
  bool dbReady() const { return db_.isReady(); }
  bool loaded() const { return loaded_; }
  const std::string &dbTable() const { return db_.tableName(); }
  const std::string &statusText() const { return status_; }
  bool statusIsError() const { return statusError_; }
  unsigned statusSerial() const { return statusSerial_; }
  DefaultTiles defaultTiles() const { return defaultTiles_; }
  const std::vector<AppItem> &hiddenItems() const { return hidden_; }
  size_t leftoverCount() const { return leftovers_.size(); }
  Tab tab() const { return tab_; }
  bool cursorChanged() {
    bool c = cursorChanged_;
    cursorChanged_ = false;
    return c;
  }

  void moveCursor(int delta);
  void nextTab(int delta);
  void toggleMark();
  void clearMarks() { marked_.clear(); }

  void pinSelectionToTop();
  void sendSelectionToEnd();
  void enterReorder();
  void reorderMove(int delta);
  void commitReorder();
  void cancelReorder();
  void toggleDefaultTiles(); // What's New + PlayStation Store: hide when
                             // visible, show when hidden
  void hideSelection();
  void showHidden(const std::string &id);
  void removeLeftovers();
  void restoreDefaultLayout(); // back to the saved default layout (or the state
                               // before PS4Hub)
  void resetToSystemOrder();
  std::vector<LayoutProfile> profiles() const;
  void saveProfile(int id);
  void loadProfile(int id);
  void deleteProfile(int id);
  bool hasSavedDefault() const;
  std::vector<std::string> catalogIds() { return db_.allTitleIds(); }
  bool makeBackup() { return backupFirst(); }

  // Fixed positions: the front of the home screen, in this order.
  void setFixedPosition(int slot);
  void clearFixedPosition();
  void unpinSelection(); // back to its last-played place (fixed / front / end
                         // released)
  int fixedCount() const;
  int firstFreeSlot() const;
  void requestReboot();
  size_t layoutChanges() const { return layoutChanges_; }

  void notify(const std::string &s, bool error = false) { setStatus(s, error); }
  void uninstallCurrent();
  void restoreBackup();

  const AppItem *currentItem() const;
  bool canUninstallCurrent() const;

private:
  std::vector<std::string> selectionInListOrder() const;
  void rememberOriginals(const std::vector<std::string> &ids);
  const AppItem *findAny(const std::string &id) const;
  bool backupFirst();
  void setStatus(const std::string &s, bool error = false);

  CatalogDb db_;
  std::vector<AppItem> items_; // exactly what the home screen shows
  std::vector<AppItem> hidden_;
  std::vector<std::string> leftovers_;
  std::vector<std::string> builtinIds_;
  std::vector<AppItem> orderBeforeReorder_;
  std::set<std::string> movedIds_;
  std::vector<std::string>
      slots_; // fixed tiles, index 0 = position 1, "" = free
  void trimSlots();
  bool unfix(const std::vector<std::string> &ids);
  bool applyingSlots_ = false;
  // Writes the whole order as ui_pos 1..N with fixed tiles at their slots.
  // `order` (title ids) overrides the current order of the other tiles.
  bool applySlots(const std::vector<std::string> *order = nullptr);
  std::map<std::string, std::string> knownRealTimes();
  bool applySlotsAll(const std::vector<std::string> &order);
  bool restoreSnapshot(const std::string &dbFile, const std::string &slotsFile,
                       const std::string &label);
  // ui_pos 1..N rows for an order; only the unbroken run of "end" tiles at
  // the very back keeps the end marks (ui_pos 900 + i, 2000-01-01).
  std::vector<CatalogDb::SeqRow>
  sequenceRows(const std::vector<std::string> &order,
               const std::set<std::string> &notEnd) const;
  std::set<std::string> releasedEnd_; // tiles whose "end" pin is being removed
  void loadSlots();
  void saveSlots() const;
  size_t layoutChanges_ = 0;
  std::set<std::string> marked_;
  int cursor_ = 0;
  bool cursorChanged_ = true;
  bool reorderMode_ = false;
  bool loaded_ = false;
  Tab tab_ = Tab::Library;
  std::string status_;
  bool statusError_ = false;
  unsigned statusSerial_ = 0;
  DefaultTiles defaultTiles_ = DefaultTiles::Unknown;
};
