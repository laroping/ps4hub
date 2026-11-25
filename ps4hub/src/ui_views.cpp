#include "ui_views.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "icon_cache.h"
#include "imgui.h"
#include "libc_compat.h"
#include "network_service.h"
#include "ntfs_install.h"
#include "ntfs_manager.h"

#include "app_catalog.h"
#include "catalog_db.h"
#include "gh_plugins.h"
#include "net_profiles.h"
#include "pkg_installer.h"
#include "platform.h"
#include "self_updater.h"
#include "sfo_parser.h"
#include "system_tools.h"
#include "title_inspector.h"
#include "ui_theme.h"
#include "webkit_browser.h"
#include "yt_adblock.h"
#include <orbis/SystemService.h>
#include <orbis/_types/sys_service.h>

using Theme::Glyph;
using Theme::Icon;

namespace {

constexpr float SW = (float)SCREEN_WIDTH, SH = (float)SCREEN_HEIGHT;
constexpr float M = 96.0f;
constexpr float kTopBarY = 40.0f, kTopBarH = 58.0f;
constexpr float kContentTop = 270.0f, kContentBot = 990.0f;

constexpr int kCols = 6;
constexpr float kTile =
    (float)IconCache::kTilePx; // drawn 1:1 with the cached icon
constexpr float kGap = 34.0f;
constexpr float kPitch = kTile + 64.0f; // tile + focused label + spacing
constexpr float kGridX = M + 8.0f;
constexpr float kGridY = kContentTop + 26.0f;
constexpr float kGridW = kCols * kTile + (kCols - 1) * kGap;
constexpr float kPanelX0 = 1340.0f, kPanelX1 = SW - M;

ImVec2 measure(float size, const std::string &s) {
  return Theme::Measure(size, s.c_str());
}

// Top of a card title so it sits vertically centred in the card's title band
// (text stays left aligned).
float bandTextY(float cardTop, float size) {
  return cardTop + (Theme::kBandH - Theme::Measure(size, "Ag").y) * 0.5f;
}

void text(ImDrawList *dl, float x, float y, float size, ImU32 col,
          const std::string &s) {
  Theme::Text(dl, x, y, size, col, s.c_str());
}
void textRight(ImDrawList *dl, float right, float y, float size, ImU32 col,
               const std::string &s) {
  text(dl, right - measure(size, s).x, y, size, col, s);
}
void textCenter(ImDrawList *dl, float cx, float y, float size, ImU32 col,
                const std::string &s) {
  text(dl, cx - measure(size, s).x * 0.5f, y, size, col, s);
}

// Shortens to fit `maxW`, cutting on UTF-8 boundaries.
std::string ellipsize(const std::string &s, float size, float maxW) {
  if (measure(size, s).x <= maxW)
    return s;
  std::string t = s;
  while (!t.empty()) {
    // Remove one whole UTF-8 character (continuation bytes + lead byte),
    // never leaving a broken sequence that renders as '?' / garbage.
    while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80)
      t.pop_back();
    if (!t.empty())
      t.pop_back();
    if (measure(size, t + "...").x <= maxW)
      return t + "...";
  }
  return "...";
}

// Greedy word wrap into at most `maxLines` lines (last one ellipsized).
std::vector<std::string> wrap(const std::string &s, float size, float maxW,
                              int maxLines) {
  std::vector<std::string> lines;
  std::string cur;
  size_t i = 0;
  while (i <= s.size()) {
    size_t sp = s.find(' ', i);
    std::string word =
        s.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
    std::string trial = cur.empty() ? word : cur + " " + word;
    if (!cur.empty() && measure(size, trial).x > maxW) {
      lines.push_back(cur);
      cur = word;
      if ((int)lines.size() == maxLines - 1) {
        std::string rest = cur;
        if (sp != std::string::npos)
          rest += s.substr(sp);
        lines.push_back(ellipsize(rest, size, maxW));
        return lines;
      }
    } else {
      cur = trial;
    }
    if (sp == std::string::npos)
      break;
    i = sp + 1;
  }
  if (!cur.empty())
    lines.push_back(ellipsize(cur, size, maxW));
  return lines;
}

std::string formatBytes(uint64_t b) {
  char buf[32];
  double v = (double)b;
  const char *units[] = {"B", "KB", "MB", "GB", "TB"};
  int u = 0;
  while (v >= 1024.0 && u < 4) {
    v /= 1024.0;
    ++u;
  }
  std::snprintf(buf, sizeof(buf), u >= 3 ? "%.1f %s" : "%.0f %s", v, units[u]);
  return buf;
}

std::string formatEta(uint32_t sec) {
  char b[32];
  if (sec >= 3600)
    std::snprintf(b, sizeof(b), "%uh %02um left", sec / 3600, (sec / 60) % 60);
  else if (sec >= 60)
    std::snprintf(b, sizeof(b), "%um %02us left", sec / 60, sec % 60);
  else
    std::snprintf(b, sizeof(b), "%us left", sec);
  return b;
}

std::string prettyDate(const AppItem &it) {
  if (it.pinned)
    return "Fixed at position " + std::to_string(it.fixedSlot);
  if (it.sentToEnd)
    return "Moved to the end";
  if (it.synthetic)
    return "Placed by " APP_TITLE;
  int y, mo, da, h, mi;
  if (std::sscanf(it.date.c_str(), "%d-%d-%d %d:%d", &y, &mo, &da, &h, &mi) ==
          5 &&
      mo >= 1 && mo <= 12) {
    static const char *kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%d %s %d, %02d:%02d", da, kMonths[mo - 1],
                  y, h, mi);
    return buf;
  }
  return it.date.empty() ? "Never" : it.date;
}

std::string initials(const std::string &name) {
  std::string out;
  bool take = true;
  for (char c : name) {
    if (c == ' ' || c == '-' || c == ':') {
      take = true;
      continue;
    }
    if (take && ((unsigned char)c < 0x80) && std::isalnum((unsigned char)c)) {
      out += (char)std::toupper((unsigned char)c);
      if (out.size() == 2)
        break;
    }
    take = false;
  }
  return out.empty() ? "?" : out;
}

ImU32 placeholderColor(const std::string &id) {
  static const ImU32 kPalette[] = {
      IM_COL32(0x1B, 0x4F, 0x9C, 255), IM_COL32(0x4B, 0x3F, 0xA8, 255),
      IM_COL32(0x0E, 0x6F, 0x86, 255), IM_COL32(0x7E, 0x3A, 0x6E, 255),
      IM_COL32(0x2E, 0x3F, 0x8F, 255), IM_COL32(0x36, 0x55, 0x74, 255),
  };
  unsigned h = 2166136261u;
  for (char c : id)
    h = (h ^ (unsigned char)c) * 16777619u;
  return kPalette[h % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

bool fileExists(const char *path) {
  struct stat st;
  return Compat::stat(path, &st) == 0;
}

struct Hint {
  Glyph g;
  const char *label;
};

void drawHintRow(ImDrawList *dl, float right, float cy,
                 const std::vector<Hint> &hints, float a = 1.0f) {
  const float d = 32.0f, gap = 10.0f, spacing = 34.0f, fs = 22.0f;
  float x = right;
  for (auto it = hints.rbegin(); it != hints.rend(); ++it) {
    float tw = Theme::Measure(fs, it->label).x;
    float gw = (it->g == Glyph::L1 || it->g == Glyph::R1) ? d * 1.55f : d;
    x -= tw;
    Theme::Text(dl, x, cy - fs * 0.49f, fs, Theme::alpha(Theme::textMuted(), a),
                it->label);
    x -= gap + gw;
    Theme::ButtonGlyph(dl, ImVec2(x, cy - d * 0.5f), d, it->g, a);
    x -= spacing;
  }
}

// Card with an icon badge on the left and a title + message to its right.
void noticeCard(ImDrawList *dl, ImVec2 center, float w, Icon icon, ImU32 tone,
                const std::string &title, const std::string &message) {
  auto lines = wrap(message, 22.0f, w - 172.0f, 3);
  float h = std::max(150.0f, 96.0f + lines.size() * 30.0f);
  ImVec2 a(center.x - w * 0.5f, center.y - h * 0.5f),
      b(center.x + w * 0.5f, center.y + h * 0.5f);
  Theme::Glass(dl, a, b);
  ImVec2 c(a.x + 76.0f, a.y + 75.0f);
  dl->AddCircleFilled(c, 34.0f, Theme::alpha(tone, 0.18f), 40);
  Theme::DrawIcon(dl, ImVec2(c.x - 18.0f, c.y - 18.0f), 36.0f, icon, tone);
  float x = a.x + 136.0f, y = a.y + 40.0f;
  text(dl, x, y, 30.0f, Theme::textPrimary(),
       ellipsize(title, 30.0f, w - 172.0f));
  y += 46.0f;
  for (auto &l : lines) {
    text(dl, x, y, 22.0f, Theme::textMuted(), l);
    y += 30.0f;
  }
}

} // namespace

UiRoot::UiRoot(AppController &ctl) : ctl_(ctl) {}

void UiRoot::openConfirm(const std::string &txt, std::function<void()> action) {
  confirmText_ = txt;
  confirmAction_ = std::move(action);
  overlay_ = Overlay::Confirm;
  overlayClosing_ = false;
  overlayAnim_ = 0.0f;
}

void UiRoot::closeOverlay() {
  if (overlay_ != Overlay::None)
    overlayClosing_ = true;
}

void UiRoot::runEntry(const Entry &e) {
  if (!e.enabled)
    return;
  invalidatePluginStatus();
  // Copy first: an action may rebuild options_ (open another list), which
  // destroys the entry - and the lambda's captured values - it came from.
  std::function<void()> action = e.action;
  std::string confirm = e.confirm;
  if (e.stay &&
      confirm.empty()) { // toggle: the list stays open and is rebuilt in place
    if (action)
      action();
    return;
  }
  if (!confirm.empty()) {
    openConfirm(confirm, std::move(action));
  } else {
    closeOverlay();
    if (action)
      action();
  }
}

void UiRoot::openOptions() {
  const AppItem *cur = ctl_.currentItem();
  if (!cur)
    return;
  options_.clear();
  size_t n = ctl_.markedCount();
  optionsTitle_ =
      n ? std::to_string(n) + (n == 1 ? " app selected" : " apps selected")
        : cur->name;

  const bool fixed = cur->fixedSlot > 0;
  const char *what = cur->isFolder && !n ? "folder" : "tile";
  Entry front{"Move to start",
              std::string("Put this ") + what + " first (fixed position 1)",
              [this] { ctl_.pinSelectionToTop(); }};
  Entry end{"Move to end",
            std::string("Put this ") + what + " at the back of the home screen",
            [this] { ctl_.sendSelectionToEnd(); }};
  if (cur->isFolder) {
    options_.push_back(front);
    options_.push_back(end);
  } // folders: the main actions first
  options_.push_back(
      {"Set fixed position",
       fixed ? "Now at position " + std::to_string(cur->fixedSlot) +
                   " - choose another"
             : std::string("Keep this ") + what + " at any chosen place",
       [this] { openPositionPicker(); }});
  if (!cur->isFolder)
    options_.push_back(front);
  // Unpin only for tiles that really are pinned: a fixed position or the
  // end of the list (for a selection: if any selected tile is).
  bool pinnedSel = fixed || cur->sentToEnd;
  if (n)
    for (const AppItem &m : ctl_.items())
      if (ctl_.isMarked(m.id) && (m.fixedSlot > 0 || m.sentToEnd))
        pinnedSel = true;
  if (pinnedSel)
    options_.push_back({"Unpin",
                        fixed && !n
                            ? "Remove fixed position " +
                                  std::to_string(cur->fixedSlot) +
                                  " and go back to the last-played place"
                        : cur->sentToEnd && !n
                            ? std::string("Release it from the end of the list")
                            : std::string("Back to the last-played place"),
                        [this] { ctl_.unpinSelection(); }});
  if (!cur->isFolder)
    options_.push_back(end);
  options_.push_back({"Rearrange", "Move tiles freely with L1 / R1",
                      [this] { ctl_.enterReorder(); }});
  if (!cur->isFolder && !n && CatalogDb::isPlainTitleId(cur->id) &&
      cur->id.rfind("NPXS", 0) != 0) {
    std::string id = cur->id, name = cur->name;
    options_.push_back({"Updates & DLC",
                        "Installed update and add-ons, remove them one by one",
                        [this, id, name] { openAddonsList(id, name); }});
  }
  if (!cur->isFolder)
    options_.push_back({"Hide from home screen",
                        "Can be shown again from System > Hidden tiles",
                        [this] { ctl_.hideSelection(); }});
  options_.push_back({"Refresh", "Read the order again from app.db",
                      [this] { ctl_.reload(); }});
  size_t changes = ctl_.layoutChanges();
  bool saved = ctl_.hasSavedDefault();
  options_.push_back({"Restore default layout",
                      saved ? "Back to the saved default layout"
                            : (changes ? "Undo the changes made by " APP_TITLE
                                       : "The layout is already the default"),
                      [this] { ctl_.restoreDefaultLayout(); }, true,
                      saved || changes > 0,
                      "Restore the default layout?\nOrder, fixed positions and "
                      "hidden tiles return to the saved "
                      "default."});
  if (!n && ctl_.canUninstallCurrent())
    options_.push_back({"Delete", "Uninstall this application",
                        [this] { ctl_.uninstallCurrent(); }, true, true,
                        "Delete " + cur->name +
                            "?\nThe application and its data will be removed "
                            "from the console."});
  optionsCursor_ = 0;
  optionsHighlightY_ = Anim::Spring(0.0f);
  optionsScroll_ = Anim::Spring(0.0f);
  optionsScrollTarget_ = 0.0f;
  overlay_ = Overlay::Options;
  overlayClosing_ = false;
  overlayAnim_ = 0.0f;
}

std::vector<UiRoot::Entry> UiRoot::systemEntries() const {
  std::vector<Entry> v;
  AppController &c = ctl_;
  UiRoot *self = const_cast<UiRoot *>(this);
  {
    SelfUpdater::Snapshot up = SelfUpdater::instance().snapshot();
    Entry ue;
    ue.category = "PS4HUB UPDATE (v" APP_VERSION ")";
    if (up.busy) {
      ue.label = up.state == SelfUpdater::State::Downloading
                     ? "Downloading PS4Hub update..."
                 : up.state == SelfUpdater::State::Verifying
                     ? "Verifying SHA-256 checksum..."
                     : "Checking for PS4Hub updates...";
      ue.detail = up.message;
      ue.enabled = false;
    } else if (up.updateAvailable) {
      ue.label = "Download PS4Hub v" + up.release.version;
      ue.detail = !up.message.empty()
                      ? up.message
                      : "Downloads to /data/pkg/, verifies SHA-256, and "
                        "requires manual installation in GoldHEN";
      ue.action = [self] {
        if (!SelfUpdater::instance().startUpdate())
          self->ctl_.notify("Updater is already busy", true);
      };
      ue.confirm =
          "Download PS4Hub v" + up.release.version +
          "?\nDownloads ps4hub.pkg to /data/pkg/, verifies SHA-256, "
          "and shows manual installation instructions for GoldHEN. "
          "If the file is missing in Package Installer, set Package Source "
          "to include all drives.";
    } else {
      ue.label = "Check for PS4Hub updates";
      ue.detail =
          !up.message.empty()
              ? up.message
              : "Checks GitHub Releases (laroping/ps4hub) for newer versions";
      ue.action = [self] {
        if (!SelfUpdater::instance().startCheck())
          self->ctl_.notify("Updater is already busy", true);
      };
    }
    v.push_back(std::move(ue));
    if (!up.busy && !up.savedPkgPath.empty()) {
      const std::string instructions =
          "Manual update installation\n" +
          SelfUpdater::manualInstallInstructions(up.savedPkgPath);
      v.push_back({"How to install the downloaded update",
                   up.savedPkgPath + " - install manually in GoldHEN",
                   [self, instructions] {
                     self->openConfirm(instructions, [] {});
                   }});
    }
  }
  const size_t firstHomeIdx = v.size();
  switch (ctl_.defaultTiles()) {
  case DefaultTiles::Visible:
  case DefaultTiles::Mixed:
    v.push_back({"Hide What's New & Store",
                 "Remove both tiles from the home screen",
                 [&c] { c.toggleDefaultTiles(); }, false, true,
                 "Hide What's New and PlayStation Store?\nThey can be shown "
                 "again from this menu at any time."});
    break;
  case DefaultTiles::Hidden:
    v.push_back({"Show What's New & Store",
                 "Both tiles are hidden from the home screen",
                 [&c] { c.toggleDefaultTiles(); }});
    break;
  default:
    v.push_back({"What's New & Store",
                 "These tiles were not found in the library", nullptr, false,
                 false});
    break;
  }
  if (v.size() > firstHomeIdx)
    v[firstHomeIdx].category = "HOME SCREEN & SYSTEM";
  size_t hidden = ctl_.hiddenItems().size();
  v.push_back({"Hidden tiles" + (hidden ? " (" + std::to_string(hidden) + ")"
                                        : std::string()),
               hidden ? "Show a hidden tile on the home screen again"
                      : "No tiles are hidden",
               [self] { self->openHiddenList(); }, false, hidden > 0});
  size_t left = ctl_.leftoverCount();
  if (left)
    v.push_back({"Clean up leftovers (" + std::to_string(left) + ")",
                 "Entries of apps that are no longer installed",
                 [&c] { c.removeLeftovers(); }, false, true,
                 "Remove " + std::to_string(left) + " leftover entr" +
                     (left == 1 ? "y" : "ies") +
                     "?\nOnly catalog rows of uninstalled apps are deleted; a "
                     "backup is made first."});
  v.push_back({"Reload library", "Read the home screen order again from app.db",
               [&c] { c.reload(); }});
  v.push_back(
      {"Reset to system order",
       "Undo every order change; the console sorts by last played again",
       [&c] { c.resetToSystemOrder(); }, true, ctl_.dbReady(),
       "Reset to the system order?\nAll fixed positions and moves are removed "
       "and every tile gets its "
       "real last-played date back. Hidden tiles stay hidden. A backup of "
       "app.db is made first."});
  {
    size_t np = (size_t)layoutProfiles_;
    v.push_back({"Layout profiles" +
                     (np ? " (" + std::to_string(np) + ")" : std::string()),
                 "Save the current order as a profile, load or delete profiles",
                 [self] { self->openProfilesList(); }, false, ctl_.dbReady()});
  }
  {
    auto st = FpkgRebuilder::instance().state();
    std::string detail =
        st == FpkgRebuilder::State::Running
            ? "Working..."
            : "Untested! Rebuild can break database - use at own risk";
    // Always clickable (even when the cached count says zero): after a
    // uninstall that damaged app.db the home screen may be empty but the
    // user still needs to run this. The worker itself is a no-op if
    // nothing is actually missing.
    v.push_back(
        {"Rebuild game database (Untested)", detail,
         [self] {
           std::set<std::string> ids;
           for (const auto &id : self->ctl_.catalogIds())
             ids.insert(id);
           if (self->ctl_.makeBackup())
             FpkgRebuilder::instance().start(ids);
         },
         false, st != FpkgRebuilder::State::Running,
         "WARNING: Rebuild can break database, use at own risk!\nThis function "
         "is the only untested one in PS4Hub.\n\nRescan /user/app and "
         "re-register missing games?\nA backup of app.db is made first."});
  }
  {
    struct stat st;
    bool haveBackup = Compat::stat(BACKUP_DB_PATH, &st) == 0 && st.st_size > 0;
    v.push_back({"Restore app.db from backup",
                 haveBackup ? "Revert the catalog to the last PS4Hub backup"
                            : "No backup file found (" BACKUP_DB_PATH ")",
                 [&c] { c.restoreBackup(); }, true, haveBackup,
                 "Restore app.db from the previous backup?\nAll changes since "
                 "that backup are undone."});
  }
  v.push_back({"Exit", "Return to the home screen",
               [self] { self->quitRequested_ = true; }, false, true,
               "Exit " APP_TITLE "?\nYou will return to the home screen."});
  return v;
}

void UiRoot::openHiddenList() {
  const auto &hidden = ctl_.hiddenItems();
  if (hidden.empty())
    return;
  options_.clear();
  optionsTitle_ = "Hidden tiles";
  for (const AppItem &it : hidden) {
    std::string id = it.id;
    options_.push_back({it.name, "Show " + id + " on the home screen again",
                        [this, id] { ctl_.showHidden(id); }});
  }
  optionsCursor_ = 0;
  optionsHighlightY_ = Anim::Spring(0.0f);
  optionsScroll_ = Anim::Spring(0.0f);
  optionsScrollTarget_ = 0.0f;
  overlay_ = Overlay::Options;
  overlayClosing_ = false;
  overlayAnim_ = 0.0f;
}

void UiRoot::openPositionPicker() {
  const AppItem *cur = ctl_.currentItem();
  if (!cur)
    return;
  size_t marked = ctl_.markedCount();
  bool alreadyFixed = cur->fixedSlot > 0 && !marked;
  // Any place on the home screen; positions between fixed tiles are filled
  // by the most recently played ones.
  pickerMax_ = std::max(1, (int)ctl_.items().size());
  pickerValue_ = std::min(pickerMax_,
                          alreadyFixed ? cur->fixedSlot : ctl_.firstFreeSlot());
  pickerTitle_ =
      marked ? std::to_string(marked) + " selected tiles" : cur->name;
  overlay_ = Overlay::Picker;
  overlayClosing_ = false;
  overlayAnim_ = 0.0f;
}

void UiRoot::showList(const std::string &title) {
  optionsTitle_ = title;
  optionsCursor_ = 0;
  optionsHighlightY_ = Anim::Spring(0.0f);
  optionsScroll_ = Anim::Spring(0.0f);
  optionsScrollTarget_ = 0.0f;
  overlay_ = Overlay::Options;
  overlayClosing_ = false;
  overlayAnim_ = 0.0f;
}

void UiRoot::openAddonsList(std::string id, std::string name) {
  options_.clear();
  auto addons = Addons::list(id);
  for (const AddonEntry &e : addons) {
    std::string label =
        e.isPatch
            ? "Update" + (e.version.empty() ? std::string() : " v" + e.version)
            : e.name + (e.version.empty() || e.version == "01.00"
                            ? std::string()
                            : " v" + e.version);
    std::string what = e.isPatch ? "the update" : "\"" + e.name + "\"";
    AddonEntry copy = e;
    options_.push_back(
        {label,
         formatBytes(e.bytes) + " - select to uninstall" +
             (e.isPatch ? "" : " this add-on"),
         [this, id, name, copy] {
           std::string err;
           if (Addons::uninstall(id, copy, err))
             ctl_.notify((copy.isPatch ? std::string("Update") : copy.name) +
                         " removed from " + name);
           else
             ctl_.notify("Uninstall failed: " + err, true);
         },
         true, true,
         "Uninstall " + what + "?\n" + name + " itself stays installed. " +
             (e.isPatch ? "The game goes back to its base version."
                        : "The add-on can be installed again later.")});
  }
  if (addons.empty())
    options_.push_back({"Nothing installed",
                        "This title has no update or add-ons", nullptr, false,
                        false});
  showList("Updates & DLC");
}

void UiRoot::openProfilesList() {
  options_.clear();
  options_.push_back(
      {"Save as new profile",
       "Store the current order, fixed positions and hidden tiles", [this] {
         ctl_.saveProfile(0);
         layoutProfiles_ = (int)ctl_.profiles().size();
       }});
  options_.push_back(
      {"Default layout",
       ctl_.hasSavedDefault() ? "The layout from the first start of " APP_TITLE
                              : "Undo the changes made by " APP_TITLE,
       [this] { ctl_.restoreDefaultLayout(); }, true, true,
       "Load the default layout?\nThe home screen returns to the layout it had "
       "before " APP_TITLE " changed it. A backup of app.db is made first."});
  for (const LayoutProfile &p : ctl_.profiles()) {
    int id = p.id;
    std::string name = p.name;
    options_.push_back({name, "Load, update or delete",
                        [this, id, name] { openProfileActions(id, name); }});
  }
  showList("Layout profiles");
}

void UiRoot::openProfileActions(int id, std::string name) {
  options_.clear();
  options_.push_back({"Load this profile",
                      "Order, fixed positions and hidden tiles from " + name,
                      [this, id] { ctl_.loadProfile(id); }, false, true,
                      "Load " + name +
                          "?\nThe current layout is replaced. A backup of "
                          "app.db is made first."});
  options_.push_back(
      {"Update with current layout",
       "Replace this profile with the order shown now",
       [this, id] { ctl_.saveProfile(id); }, true, true,
       "Update " + name +
           "?\nIts saved layout is replaced by the current one."});
  options_.push_back(
      {"Delete profile", "Remove it; the default layout is not affected",
       [this, id] {
         ctl_.deleteProfile(id);
         layoutProfiles_ = (int)ctl_.profiles().size();
       },
       true, true, "Delete " + name + "?\nThis cannot be undone."});
  options_.push_back(
      {"Back", "Return to the profile list", [this] { openProfilesList(); }});
  showList(name);
}

void UiRoot::openServicesList() {
  options_.clear();
  optionsTitle_ = "Background services";
  for (const ServiceInfo &s : Services::scan()) {
    std::string label = s.name + "  :" + std::to_string(s.port);
    std::string detail;
    bool enabled = false;
    std::string confirm;
    std::function<void()> act;
    if (s.ours) {
      detail = s.listening ? "Running - select to stop"
                           : "Stopped - select to start";
      enabled = true;
      act = [this] {
        auto &svc = NetworkService::instance();
        if (svc.running())
          svc.stop();
        else
          svc.start(SERVER_PORT, Platform::userId());
        ctl_.notify(svc.running() ? "Remote install server started"
                                  : "Remote install server stopped");
      };
    } else if (!s.listening) {
      detail = "Not running";
    } else if (s.stoppable) {
      detail =
          "Running in " + PortOwner::describe(s.owner) + " - select to stop";
      enabled = true;
      confirm = "Stop " + s.name + "?\nThe app " +
                PortOwner::describe(s.owner) +
                " will be closed to free its memory.";
      ServiceInfo copy = s;
      act = [this, copy] {
        std::string err;
        if (Services::stop(copy, err))
          ctl_.notify(copy.name + " stopped");
        else
          ctl_.notify(err, true);
      };
    } else {
      detail = "Running (system / GoldHEN) - change it in GoldHEN settings";
    }
    options_.push_back({label, detail, act, false, enabled, confirm});
  }
  optionsCursor_ = 0;
  optionsHighlightY_ = Anim::Spring(0.0f);
  optionsScroll_ = Anim::Spring(0.0f);
  optionsScrollTarget_ = 0.0f;
  overlay_ = Overlay::Options;
  overlayClosing_ = false;
  overlayAnim_ = 0.0f;
}

void UiRoot::refreshSystemCaches() {
  std::set<std::string> known;
  for (const auto &id : ctl_.catalogIds())
    known.insert(id);
  fpkgMissing_ =
      ctl_.dbReady() ? (int)FpkgRebuilder::findMissing(known).size() : -1;
  blockerState_ = (int)UpdateBlocker::state();
  layoutProfiles_ = (int)ctl_.profiles().size();
}

void UiRoot::gridMove(int dx, int dy) {
  int n = (int)ctl_.items().size();
  if (!n)
    return;
  int cur = ctl_.cursor();
  if (dx) {
    ctl_.moveCursor(dx);
    return;
  }
  if (dy < 0 && cur >= kCols)
    ctl_.moveCursor(-kCols);
  if (dy > 0) {
    if (cur + kCols < n)
      ctl_.moveCursor(kCols);
    else if (cur / kCols < (n - 1) / kCols)
      ctl_.moveCursor(n - 1 - cur); // into the short last row
  }
}

bool UiRoot::handleInput(const Gamepad &pad) {
  if (quitRequested_)
    return outro_ >=
           0.85f; // start exiting as soon as the screen is mostly black
  if (overlayClosing_)
    return false;
  auto P = [&](Pad b) { return pad.pressed(b); };

  if (overlay_ == Overlay::Confirm) {
    if (P(Pad::Cross)) {
      auto act = confirmAction_;
      invalidatePluginStatus();
      confirmAction_ = nullptr;
      closeOverlay();
      if (act)
        act();
    } else if (P(Pad::Circle)) {
      closeOverlay();
    }
    return false;
  }

  if (overlay_ == Overlay::Picker) {
    if (P(Pad::Left) || P(Pad::Up))
      pickerValue_ = std::max(1, pickerValue_ - 1);
    if (P(Pad::Right) || P(Pad::Down))
      pickerValue_ = std::min(pickerMax_, pickerValue_ + 1);
    if (P(Pad::L1))
      pickerValue_ = std::max(1, pickerValue_ - 10);
    if (P(Pad::R1))
      pickerValue_ = std::min(pickerMax_, pickerValue_ + 10);
    if (P(Pad::Cross)) {
      int v = pickerValue_;
      closeOverlay();
      ctl_.setFixedPosition(v);
    } else if (P(Pad::Circle))
      closeOverlay();
    return false;
  }

  if (overlay_ == Overlay::Options) {
    int n = (int)options_.size();
    if (P(Pad::Up))
      optionsCursor_ = (optionsCursor_ + n - 1) % n;
    if (P(Pad::Down))
      optionsCursor_ = (optionsCursor_ + 1) % n;
    if (P(Pad::Cross))
      runEntry(options_[optionsCursor_]);
    else if (P(Pad::Circle) || P(Pad::Square))
      closeOverlay();
    return false;
  }

  if (ctl_.reorderMode()) {
    if (P(Pad::Up))
      gridMove(0, -1);
    if (P(Pad::Down))
      gridMove(0, +1);
    if (P(Pad::Left))
      gridMove(-1, 0);
    if (P(Pad::Right))
      gridMove(+1, 0);
    if (P(Pad::L1))
      ctl_.reorderMove(-1);
    if (P(Pad::R1))
      ctl_.reorderMove(+1);
    if (P(Pad::Cross))
      ctl_.commitReorder();
    if (P(Pad::Circle))
      ctl_.cancelReorder();
    return false;
  }

  if (P(Pad::L1))
    ctl_.nextTab(-1);
  if (P(Pad::R1))
    ctl_.nextTab(+1);

  switch (ctl_.tab()) {
  case Tab::Library:
    if (P(Pad::Up))
      gridMove(0, -1);
    if (P(Pad::Down))
      gridMove(0, +1);
    if (P(Pad::Left))
      gridMove(-1, 0);
    if (P(Pad::Right))
      gridMove(+1, 0);
    if (P(Pad::Cross))
      ctl_.toggleMark();
    if (P(Pad::Square))
      openOptions();
    if (P(Pad::Triangle))
      ctl_.reload();
    break;
  case Tab::Downloads: {
    auto &svc = NetworkService::instance();
    auto tasks = svc.tasks();
    int n = (int)tasks.size();
    if (n) {
      downloadsCursor_ = std::min(downloadsCursor_, n - 1);
      if (P(Pad::Up))
        downloadsCursor_ = std::max(0, downloadsCursor_ - 1);
      if (P(Pad::Down))
        downloadsCursor_ = std::min(n - 1, downloadsCursor_ + 1);
      const InstallTask &t =
          tasks[n - 1 - downloadsCursor_]; // newest first on screen
      if (P(Pad::Cross) && !t.complete && !t.stopped)
        svc.togglePause(t.taskId);
      if (P(Pad::Square) && !t.complete && !t.stopped)
        svc.stopTask(t.taskId);
    }
    if (P(Pad::Triangle)) {
      if (svc.running())
        svc.stop();
      else
        svc.start(SERVER_PORT, Platform::userId());
    }
    break;
  }
  case Tab::Plugins: {
    if (catalogExpanded_) {
      const auto &apps = AppCatalog::entries();
      int n = (int)apps.size();
      if (n > 0) {
        if (P(Pad::Up))
          catalogCursor_ = (catalogCursor_ + n - 1) % n;
        if (P(Pad::Down))
          catalogCursor_ = (catalogCursor_ + 1) % n;
      }
      if (P(Pad::Cross) && catalogCursor_ < n) {
        std::string tid = apps[catalogCursor_].titleId;
        std::string err;
        if (!AppCatalog::startInstall(tid, err))
          ctl_.notify(err, true);
      }
      if (P(Pad::Triangle)) {
        GhPlugins::Jobs::instance().start(
            "Catalog refresh", [](const auto &progress, std::string &result) {
              progress("Downloading manifest...");
              std::string err;
              if (!AppCatalog::refreshVersions(err)) {
                result = "Refresh failed: " + err;
                return false;
              }
              result = "Catalog versions updated";
              return true;
            });
      }
      break;
    }
    auto entries = pluginEntries();
    int n = (int)entries.size();
    if (P(Pad::Up))
      pluginsCursor_ = (pluginsCursor_ + n - 1) % n;
    if (P(Pad::Down))
      pluginsCursor_ = (pluginsCursor_ + 1) % n;
    if (P(Pad::Cross))
      runEntry(entries[pluginsCursor_]);
    break;
  }
  case Tab::System: {
    auto entries = systemEntries();
    int n = (int)entries.size();
    if (P(Pad::Up))
      systemCursor_ = (systemCursor_ + n - 1) % n;
    if (P(Pad::Down))
      systemCursor_ = (systemCursor_ + 1) % n;
    if (P(Pad::Cross))
      runEntry(entries[systemCursor_]);
    break;
  }
  case Tab::Ntfs: {
    auto &ntfs = NtfsManager::instance();

    auto openPicker = [&]() {
      ntfsPickerList_ = ntfs.scan();
      ntfsPickerCursor_ = 0;
      ntfsPickerShown_ = true;
    };

    if (ntfsPickerShown_) {
      int n = (int)ntfsPickerList_.size();
      if (n > 0) {
        if (P(Pad::Up))
          ntfsPickerCursor_ = (ntfsPickerCursor_ + n - 1) % n;
        if (P(Pad::Down))
          ntfsPickerCursor_ = (ntfsPickerCursor_ + 1) % n;
      }
      if (P(Pad::Triangle))
        openPicker();
      if (P(Pad::Cross) && n > 0) {
        const NtfsDeviceInfo &picked = ntfsPickerList_[ntfsPickerCursor_];
        if (ntfs.mountDevice(picked)) {
          std::vector<NtfsEntry> tmp;
          ntfs.listDir("/", tmp);
          ntfs.refresh();
          ntfsCursor_ = 0;
          ntfsPickerShown_ = false;
        } else {
          ctl_.notify("Cannot mount " + picked.path, true);
        }
      }
      // Circle is handled below (see the global Circle handler).
      break;
    }

    if (!ntfs.mounted()) {
      if (P(Pad::Triangle) || P(Pad::Cross)) {
        ntfsMountTried_ = true;
        openPicker();
      }
      break;
    }

    if (P(Pad::Square)) {
      openPicker();
      break;
    }

    const auto &es = ntfs.entries();
    int n = (int)es.size();
    if (n == 0) {
      if (P(Pad::Triangle))
        ntfs.refresh();
      break;
    }
    ntfsCursor_ = std::min(ntfsCursor_, n - 1);
    if (P(Pad::Up))
      ntfsCursor_ = std::max(0, ntfsCursor_ - 1);
    if (P(Pad::Down))
      ntfsCursor_ = std::min(n - 1, ntfsCursor_ + 1);
    if (P(Pad::L1))
      ntfsCursor_ = std::max(0, ntfsCursor_ - 10);
    if (P(Pad::R1))
      ntfsCursor_ = std::min(n - 1, ntfsCursor_ + 10);
    if (P(Pad::Triangle))
      ntfs.refresh();
    if (P(Pad::Cross)) {
      const NtfsEntry &e = es[ntfsCursor_];
      if (e.isDir) {
        if (ntfs.navigate(e.name))
          ntfsCursor_ = 0;
      } else {
        std::string lower = e.name;
        for (auto &c : lower)
          c = (char)std::tolower((unsigned char)c);
        if (lower.size() >= 4 && lower.substr(lower.size() - 4) == ".pkg") {
          if (ntfs.isShellMount()) {
            // exFAT / FAT32 USB auto-mounted by the PS4 shell
            // is read-only and lacks the extended attributes
            // BGFT requires - installs from here produce
            // unplayable games. Block it up front.
            ctl_.notify("Install from exFAT/FAT32 is blocked. Use an "
                        "NTFS-formatted drive.",
                        true);
          } else if (NtfsInstall::instance().busy()) {
            ctl_.notify("Another install from USB is in progress", true);
          } else {
            std::string cur = ntfs.currentDir();
            std::string srcPath = (cur == "/" ? "/" : cur + "/") + e.name;
            NtfsInstall::instance().start(srcPath, e.name, Platform::userId());
          }
        }
      }
    }
    break;
  }
  default:
    break;
  }

  if (P(Pad::Circle) && overlay_ == Overlay::None) {
    auto &ntfs = NtfsManager::instance();
    if (ctl_.tab() == Tab::Plugins && catalogExpanded_) {
      catalogExpanded_ = false;
    }
    // Priority 2 (USB tab):
    //   a) close the drive picker if it's open,
    //   b) walk up one folder if we're not at the drive root,
    //   c) only ask about exiting the app when there's nothing left to go back
    //   to.
    else if (ctl_.tab() == Tab::Ntfs && ntfsPickerShown_) {
      ntfsPickerShown_ = false;
    } else if (ctl_.tab() == Tab::Ntfs && ntfs.mounted() &&
               ntfs.currentDir() != "/") {
      ntfs.navigateUp();
      ntfsCursor_ = 0;
    } else if (ctl_.tab() == Tab::Library && ctl_.markedCount() > 0) {
      ctl_.clearMarks();
    } else {
      openConfirm("Exit " APP_TITLE "?\nYou will return to the home screen.",
                  [this] { quitRequested_ = true; });
    }
  }
  return false;
}

bool UiRoot::draw(float dt) {
  Anim::beginFrame();
  time_ += dt;

  if (ctl_.tab() != shownTab_) {
    pageDir_ = (int)ctl_.tab() > (int)shownTab_ ? 1 : -1;
    shownTab_ = ctl_.tab();
    pageAnim_ = 0.0f;
    if (shownTab_ == Tab::System) {
      storageFill_ = 0.0f;
      refreshSystemCaches();
    }
    if (shownTab_ == Tab::Plugins) {
      refreshSystemCaches();
      GhPlugins::repairPermissions();
    }
    if (shownTab_ == Tab::Ntfs && !ntfsMountTried_) {
      ntfsMountTried_ = true;
      auto &ntfs = NtfsManager::instance();
      if (!ntfs.mounted()) {
        // Prompt immediately if multiple USB drives/partitions exist.
        ntfsPickerList_ = ntfs.scan();
        ntfsPickerCursor_ = 0;
        ntfsPickerShown_ = !ntfsPickerList_.empty();
      }
    }
  }
  if (FpkgRebuilder::instance().serial() != rebuildSerial_) {
    rebuildSerial_ = FpkgRebuilder::instance().serial();
    if (FpkgRebuilder::instance().restoredCount() > 0)
      ctl_.reload();
    ctl_.notify(FpkgRebuilder::instance().summary(),
                FpkgRebuilder::instance().restoredCount() == 0);
    refreshSystemCaches();
  }
  if (FpkgRebuilder::instance().state() == FpkgRebuilder::State::Running)
    Anim::activeFlag() = true;
  {
    auto &jobs = GhPlugins::Jobs::instance();
    if (jobs.serial() != jobSerial_) {
      jobSerial_ = jobs.serial();
      invalidatePluginStatus();
      ctl_.notify(jobs.message(),
                  jobs.state() == GhPlugins::Jobs::State::Failed);
      bool ok = jobs.state() != GhPlugins::Jobs::State::Failed;
      Platform::notify(jobs.name() + (ok ? " ready" : " failed"));
    }
    if (jobs.running())
      Anim::activeFlag() = true;
  }
  {
    auto up = SelfUpdater::instance().snapshot();
    if (up.eventSerial != updaterSerial_) {
      updaterSerial_ = up.eventSerial;
      if (up.state == SelfUpdater::State::ReadyToInstall) {
        openConfirm("Manual update installation\n" + up.eventMessage, [] {});
        Platform::notify("PS4Hub update downloaded. Install it manually in GoldHEN.");
      } else {
        ctl_.notify(up.eventMessage, up.eventError);
        Platform::notify(up.eventMessage);
      }
    }
    if (up.busy)
      Anim::activeFlag() = true;
  }
  Anim::approach(pageAnim_, 1.0f, 8.0f, dt);
  Anim::approach(intro_, 1.0f, 3.2f, dt);
  if (quitRequested_)
    Anim::approach(outro_, 1.0f, 18.0f, dt);

  ImGuiViewport *vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->Pos);
  ImGui::SetNextWindowSize(vp->Size);
  ImGui::Begin(
      "##root", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
          ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
          ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNav);
  ImDrawList *dl = ImGui::GetWindowDrawList();

  drawTopBar(dl, dt);

  {
    Anim::VtxScope page(dl);
    drawPageTitle(dl);
    switch (shownTab_) {
    case Tab::Library:
      drawLibrary(dl, dt);
      break;
    case Tab::Downloads:
      drawDownloads(dl, dt);
      break;
    case Tab::System:
      drawSystem(dl, dt);
      break;
    case Tab::Plugins:
      drawPlugins(dl, dt);
      break;
    case Tab::Ntfs:
      drawNtfs(dl, dt);
      break;
    default:
      break;
    }
    float e = Anim::easeOutCubic(pageAnim_);
    if (e < 1.0f)
      page.apply(e, ImVec2((1.0f - e) * 40.0f * pageDir_, 0.0f));
  }

  drawHints(dl);
  ImGui::End();

  ImDrawList *fg = ImGui::GetForegroundDrawList();
  if (overlay_ != Overlay::None) {
    Anim::approach(overlayAnim_, overlayClosing_ ? 0.0f : 1.0f,
                   overlayClosing_ ? 16.0f : 10.0f, dt);
    if (overlay_ == Overlay::Options)
      drawOptions(fg, dt);
    if (overlay_ == Overlay::Confirm)
      drawConfirm(fg, dt);
    if (overlay_ == Overlay::Picker)
      drawPicker(fg, dt);
    if (overlayClosing_ && overlayAnim_ <= 0.01f) {
      overlay_ = Overlay::None;
      overlayClosing_ = false;
    }
  }
  drawToast(fg, dt);

  if (intro_ < 1.0f)
    fg->AddRectFilled(ImVec2(0, 0), ImVec2(SW, SH),
                      IM_COL32(0, 0, 0, (int)(255 * (1.0f - intro_))));
  if (outro_ > 0.0f)
    fg->AddRectFilled(ImVec2(0, 0), ImVec2(SW, SH),
                      IM_COL32(0, 0, 0, (int)(255 * outro_)));

  if (IconCache::instance().busy())
    Anim::activeFlag() = true;
  return Anim::active();
}

void UiRoot::drawTopBar(ImDrawList *dl, float dt) {
  static const char *kTabs[] = {"Library", "Downloads", "System", "Plugins",
                                "USB"};
  static const Icon kIcons[] = {Icon::Library, Icon::Download, Icon::Gear,
                                Icon::Plug, Icon::Disk};
  constexpr int kTabCount = 5;
  const float fs = 27.0f, iconS = 24.0f, padX = 24.0f, h = kTopBarH,
              y = kTopBarY;
  const bool locked = ctl_.reorderMode();

  float x = M;
  x += Theme::ButtonGlyph(dl, ImVec2(x, y + (h - 32.0f) * 0.5f), 32.0f,
                          Glyph::L1, locked ? 0.3f : 0.75f) +
       14.0f;

  float tabX[kTabCount], tabW[kTabCount];
  for (int i = 0; i < kTabCount; ++i) {
    tabW[i] = padX * 2 + iconS + 12.0f + Theme::Measure(fs, kTabs[i]).x;
    tabX[i] = x;
    x += tabW[i] + 4.0f;
  }
  int active = (int)ctl_.tab();
  if (!tabPillInit_) {
    tabPillX_ = Anim::Spring(tabX[active]);
    tabPillW_ = Anim::Spring(tabW[active]);
    tabPillInit_ = true;
  }
  float px = tabPillX_.step(tabX[active], dt, 5.0f, 0.82f);
  float pw = tabPillW_.step(tabW[active], dt, 5.0f, 0.82f);
  Theme::FillRoundRect(dl, ImVec2(px, y), ImVec2(px + pw, y + h),
                       IM_COL32(255, 255, 255, 36), h * 0.5f);

  for (int i = 0; i < kTabCount; ++i) {
    bool on = i == active;
    ImU32 col =
        on ? Theme::white() : (locked ? Theme::textDim() : Theme::textMuted());
    Theme::DrawIcon(dl, ImVec2(tabX[i] + padX, y + (h - iconS) * 0.5f), iconS,
                    kIcons[i], col);
    Theme::Text(dl, tabX[i] + padX + iconS + 12.0f, y + (h - fs) * 0.5f - 2.0f,
                fs, col, kTabs[i]);
    if (i == (int)Tab::System &&
        SelfUpdater::instance().snapshot().updateAvailable) {
      dl->AddCircleFilled(ImVec2(tabX[i] + tabW[i] - 12.0f, y + 14.0f), 4.5f,
                          Theme::accentBright(), 12);
    }
  }
  Theme::ButtonGlyph(dl, ImVec2(x + 10.0f, y + (h - 32.0f) * 0.5f), 32.0f,
                     Glyph::R1, locked ? 0.3f : 0.75f);

  // The libc localtime path relies on a TZ env var that Orbis doesn't set,
  // so it would otherwise display UTC. Apply the console's configured
  // time-zone offset + summer-time flag from SystemService instead.
  char clock[16] = "--:--";
  {
    std::time_t now = std::time(nullptr);
    int tzOffsetMin = 0;
    int summer = 0;
    sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_TIME_ZONE,
                                &tzOffsetMin);
    sceSystemServiceParamGetInt(ORBIS_SYSTEM_SERVICE_PARAM_ID_SUMMERTIME,
                                &summer);
    std::time_t local =
        now + (std::time_t)tzOffsetMin * 60 + (summer ? 3600 : 0);
    std::tm lt{};
    gmtime_r(&local, &lt);
    std::strftime(clock, sizeof(clock), "%H:%M", &lt);
  }
  const float cy = y + h * 0.5f;
  float right = SW - M;
  textRight(dl, right, cy - 20.0f, 34.0f, Theme::textPrimary(), clock);
  right -= measure(34.0f, clock).x + 36.0f;
  auto chipText = [&](const std::string &s, Icon icon) {
    textRight(dl, right, cy - 14.0f, 23.0f, Theme::textMuted(), s);
    right -= measure(23.0f, s).x + 10.0f + 24.0f;
    Theme::DrawIcon(dl, ImVec2(right, cy - 12.0f), 24.0f, icon,
                    Theme::textMuted());
    right -= 30.0f;
  };
  if (storage_.ok)
    chipText(formatBytes(storage_.freeBytes) + " free", Icon::Disk);
  if (stats_.cpuTempC > 0)
    chipText(std::to_string(stats_.cpuTempC) + "\xC2\xB0"
                                               "C",
             Icon::Thermo);
  if (stats_.ramTotal) {
    char b[32];
    std::snprintf(
        b, sizeof(b), "RAM %d%%",
        (int)(100.0 * (double)stats_.ramUsed / (double)stats_.ramTotal + 0.5));
    chipText(b, Icon::Network);
  }
  if (stats_.uptimeSec) {
    uint64_t u = stats_.uptimeSec;
    char b[32];
    if (u >= 86400)
      std::snprintf(b, sizeof(b), "Up %llud %lluh",
                    (unsigned long long)(u / 86400),
                    (unsigned long long)(u / 3600 % 24));
    else
      std::snprintf(b, sizeof(b), "Up %lluh %02llum",
                    (unsigned long long)(u / 3600),
                    (unsigned long long)(u / 60 % 60));
    chipText(b, Icon::Gear);
  }
}

void UiRoot::drawPageTitle(ImDrawList *dl) {
  std::string title, sub;
  switch (shownTab_) {
  case Tab::Library: {
    title = ctl_.reorderMode() ? "Rearrange" : "Library";
    size_t n = ctl_.items().size(), k = ctl_.markedCount();
    if (ctl_.reorderMode())
      sub = "Use L1 and R1 to move the highlighted tile";
    else if (!ctl_.loaded())
      sub = "Reading the home screen...";
    else
      sub = std::to_string(n) + " items on the home screen" +
            (k ? "    " + std::to_string(k) + " selected" : "");
    break;
  }
  case Tab::Downloads:
    title = "Remote Install";
    sub = "Send packages from a computer or phone on your network";
    break;
  case Tab::System:
    title = "System";
    sub = "Storage, status and maintenance";
    break;
  case Tab::Plugins:
    title = "Plugins";
    sub = "GoldHEN plugins, game patches and system tools";
    break;
  case Tab::Ntfs:
    title = "USB Drive";
    sub = "Browse an NTFS USB drive and install PKG files directly";
    break;
  default:
    break;
  }
  text(dl, M, 132.0f, 58.0f, Theme::textPrimary(), title);
  text(dl, M + 2.0f, 202.0f, 25.0f, Theme::textMuted(), sub);
  if (ctl_.reorderMode()) {
    float x = M + measure(58.0f, title).x + 26.0f;
    Theme::Pill(dl, x, 154.0f, 19.0f, "EDITING", Theme::accent(),
                Theme::white());
  }
}

void UiRoot::drawTile(ImDrawList *dl, const AppItem &it, ImVec2 p, float focus,
                      float lift, float a) {
  const float s = 1.0f + 0.1f * focus + 0.05f * lift;
  const float size = kTile * s;
  ImVec2 c(p.x + kTile * 0.5f, p.y + kTile * 0.5f - 8.0f * lift);
  ImVec2 p0(c.x - size * 0.5f, c.y - size * 0.5f),
      p1(c.x + size * 0.5f, c.y + size * 0.5f);
  const float r = 20.0f * s;
  const float contentA = a * (it.visible ? 1.0f : 0.42f);
  const float f = Anim::clamp01(focus);

  if (f > 0.02f) // soft shadow under the raised tile
    Theme::FillRoundRect(dl, ImVec2(p0.x + 8, p0.y + 18),
                         ImVec2(p1.x - 8, p1.y + 20),
                         IM_COL32(0, 4, 16, (int)(90 * f * a)), r);

  ImTextureID tex =
      it.isFolder ? 0 : IconCache::instance().get(it.id, IconSize::Tile);
  float &iconA = tileFade_.get("icon:" + it.id, 0.0f);
  if (tex)
    Anim::approach(iconA, 1.0f, 9.0f, ImGui::GetIO().DeltaTime);

  if (!tex || iconA < 1.0f) {
    ImU32 bg =
        it.isFolder ? Theme::rgba(0x1E, 0x5F, 0xB8) : placeholderColor(it.id);
    Theme::FillRoundRect(dl, p0, p1, Theme::alpha(bg, contentA), r);
    if (it.isFolder) {
      float is = size * 0.42f;
      Theme::DrawIcon(dl, ImVec2(c.x - is * 0.5f, c.y - is * 0.5f), is,
                      Icon::Folder,
                      Theme::alpha(Theme::white(), contentA * 0.9f));
    } else {
      std::string ini = initials(it.name);
      float fs = 52.0f * s;
      ImVec2 ts = measure(fs, ini);
      text(dl, c.x - ts.x * 0.5f, c.y - ts.y * 0.5f, fs,
           Theme::alpha(Theme::white(), contentA * 0.9f), ini);
    }
  }
  if (tex)
    dl->AddImage(ImTextureRef(tex), p0, p1, ImVec2(0, 0), ImVec2(1, 1),
                 Theme::alpha(Theme::white(), contentA * iconA));

  // Small status badges sit inside the tile corners.
  if (it.pinned) {
    ImVec2 bc(p0.x + 22.0f * s, p1.y - 22.0f * s);
    dl->AddCircleFilled(bc, 15.0f * s, IM_COL32(0, 8, 24, (int)(200 * a)), 24);
    Theme::DrawIcon(dl, ImVec2(bc.x - 9 * s, bc.y - 10 * s), 19.0f * s,
                    Icon::Pin, Theme::alpha(Theme::accentBright(), a));
  }
  if (!it.visible) {
    ImVec2 bc(p1.x - 22.0f * s, p1.y - 22.0f * s);
    dl->AddCircleFilled(bc, 15.0f * s, IM_COL32(0, 8, 24, (int)(200 * a)), 24);
    Theme::DrawIcon(dl, ImVec2(bc.x - 9 * s, bc.y - 9 * s), 18.0f * s,
                    Icon::EyeOff, Theme::alpha(Theme::white(), a));
  }
  float &mark = tileMark_.get(it.id, 0.0f);
  Anim::approach(mark, ctl_.isMarked(it.id) ? 1.0f : 0.0f, 16.0f,
                 ImGui::GetIO().DeltaTime);
  if (mark > 0.01f) {
    float br = 17.0f * (1.0f + 0.25f * std::sin(mark * 3.14159265f)) * mark;
    ImVec2 bc(p1.x - 20.0f * s, p0.y + 20.0f * s);
    dl->AddCircleFilled(bc, br + 2.5f, Theme::alpha(Theme::white(), a), 28);
    dl->AddCircleFilled(bc, br, Theme::alpha(Theme::accent(), a), 28);
    Theme::DrawIcon(dl, ImVec2(bc.x - br * 0.6f, bc.y - br * 0.6f), br * 1.2f,
                    Icon::Check, Theme::alpha(Theme::white(), a));
  }

  Theme::FocusRing(dl, ImVec2(p0.x - 6, p0.y - 6), ImVec2(p1.x + 6, p1.y + 6),
                   r + 6.0f, f * a);

  if (f > 0.02f) {
    std::string &label = labelCache_[it.id];
    if (label.empty())
      label = ellipsize(it.name, 24.0f, kTile + kGap * 2.0f - 8.0f);
    textCenter(dl, c.x, p1.y + 14.0f, 24.0f,
               Theme::alpha(Theme::white(), a * f), label);
  }
}

void UiRoot::drawLibrary(ImDrawList *dl, float dt) {
  const auto &items = ctl_.items();
  const int n = (int)items.size();
  const float viewH = kContentBot - kGridY;
  const ImVec2 gridCenter(kGridX + kGridW * 0.5f,
                          (kContentTop + kContentBot) * 0.5f - 40.0f);

  if (!ctl_.loaded()) {
    Theme::Spinner(dl, gridCenter, 30.0f, time_, Theme::accentBright());
    textCenter(dl, gridCenter.x, gridCenter.y + 56.0f, 25.0f,
               Theme::textMuted(), "Loading library");
    Anim::activeFlag() = true;
    return;
  }
  if (n == 0) {
    bool err = !ctl_.dbReady() || ctl_.statusIsError();
    noticeCard(dl, gridCenter, 900.0f, err ? Icon::Alert : Icon::Library,
               err ? Theme::danger() : Theme::accentBright(),
               ctl_.dbReady() ? "No items found"
                              : "The home screen database is not accessible",
               err ? ctl_.statusText() : std::string("app.db has no entries."));
    return;
  }

  // Keep the focused row in view; spring the scroll.
  int cur = ctl_.cursor();
  float rowTop = (cur / kCols) * kPitch;
  if (rowTop < scrollTarget_)
    scrollTarget_ = rowTop;
  if (rowTop + kPitch > scrollTarget_ + viewH)
    scrollTarget_ = rowTop + kPitch - viewH;
  int rows = (n + kCols - 1) / kCols;
  scrollTarget_ = std::max(
      0.0f, std::min(scrollTarget_, std::max(0.0f, rows * kPitch - viewH)));
  float scroll = scroll_.step(scrollTarget_, dt, 3.6f, 1.0f);

  int firstRow = std::max(0, (int)(scroll / kPitch) - 1);
  int lastRow = std::min(rows - 1, (int)((scroll + viewH) / kPitch) + 1);

  dl->PushClipRect(ImVec2(kGridX - 40.0f, kContentTop - 14.0f),
                   ImVec2(kGridX + kGridW + 40.0f, kContentBot + 20.0f), true);
  int focusIdx = -1;
  ImVec2 focusPos;
  float focusAmt = 0.0f, focusAlpha = 1.0f;
  for (int i = firstRow * kCols; i < std::min(n, (lastRow + 1) * kCols); ++i) {
    const AppItem &it = items[i];
    ImVec2 target((i % kCols) * (kTile + kGap), (i / kCols) * kPitch);
    ImVec2 pos = tilePos_.get(it.id, Anim::Spring2(target))
                     .step(target, dt, 4.2f, 0.86f);
    float f = tileFocus_.get(it.id, Anim::Spring(0.0f))
                  .step(i == cur ? 1.0f : 0.0f, dt, 5.5f, 0.62f);
    float &appear = tileFade_.get("in:" + it.id, 0.0f);
    Anim::approach(appear, 1.0f, 10.0f, dt);
    ImVec2 p(kGridX + pos.x, kGridY + pos.y - scroll);
    if (i == cur) {
      focusIdx = i;
      focusPos = p;
      focusAmt = f;
      focusAlpha = appear;
      continue;
    }
    drawTile(dl, it, p, f, 0.0f, appear);
  }
  if (focusIdx >= 0) // drawn last so the zoomed tile overlaps its neighbours
    drawTile(dl, items[focusIdx], focusPos, focusAmt,
             ctl_.reorderMode() ? 1.0f : 0.0f, focusAlpha);
  dl->PopClipRect();

  if (rows * kPitch > viewH) {
    float trackX = kGridX + kGridW + 30.0f, h = viewH * viewH / (rows * kPitch);
    float y =
        kGridY + (viewH - h) * Anim::clamp01(scroll / (rows * kPitch - viewH));
    Theme::FillRoundRect(dl, ImVec2(trackX, kGridY),
                         ImVec2(trackX + 4, kGridY + viewH),
                         IM_COL32(255, 255, 255, 16), 2.0f);
    Theme::FillRoundRect(dl, ImVec2(trackX, y), ImVec2(trackX + 4, y + h),
                         IM_COL32(255, 255, 255, 110), 2.0f);
  }

  drawDetail(dl, dt);
}

void UiRoot::drawDetail(ImDrawList *dl, float dt) {
  const AppItem *it = ctl_.currentItem();
  ImVec2 P0(kPanelX0, kContentTop), P1(kPanelX1, kContentBot);
  Theme::Glass(dl, P0, P1, Theme::kRadius, 1.0f,
               false); // no title band around the game artwork
  if (!it)
    return;
  if (it->id != detailId_) {
    detailId_ = it->id;
    detailAnim_ = 0.0f;
  }
  Anim::approach(detailAnim_, 1.0f, 11.0f, dt);
  float e = Anim::easeOutCubic(detailAnim_);

  Anim::VtxScope scope(dl);
  const float pad = 36.0f, w = P1.x - P0.x - 2 * pad;
  float x = P0.x + pad, y = P0.y + pad;
  const float hero = (float)IconCache::kHeroPx;

  ImTextureID tex =
      it->isFolder ? 0 : IconCache::instance().get(it->id, IconSize::Hero);
  if (tex) {
    dl->AddImage(ImTextureRef(tex), ImVec2(x, y), ImVec2(x + hero, y + hero));
  } else {
    ImU32 bg =
        it->isFolder ? Theme::rgba(0x1E, 0x5F, 0xB8) : placeholderColor(it->id);
    Theme::FillRoundRect(dl, ImVec2(x, y), ImVec2(x + hero, y + hero), bg,
                         26.0f);
    if (it->isFolder) {
      Theme::DrawIcon(dl, ImVec2(x + hero * 0.28f, y + hero * 0.28f),
                      hero * 0.44f, Icon::Folder, Theme::white());
    } else {
      std::string ini = initials(it->name);
      ImVec2 ts = measure(76.0f, ini);
      text(dl, x + (hero - ts.x) * 0.5f, y + (hero - ts.y) * 0.5f, 76.0f,
           Theme::white(), ini);
    }
  }

  float rx = x + hero + 28.0f;
  text(dl, rx, y + 6.0f, 17.0f, Theme::textDim(), "POSITION");
  text(dl, rx, y + 30.0f, 62.0f, Theme::white(),
       std::to_string(ctl_.cursor() + 1));
  text(dl, rx + 2.0f, y + 102.0f, 22.0f, Theme::textMuted(),
       "of " + std::to_string(ctl_.items().size()));

  TitleInfo info = TitleInspector::instance().get(it->id);
  if (!info.ready)
    Anim::activeFlag() = true;
  const std::string pending = info.ready ? "-" : "...";

  y += hero + 30.0f;
  text(dl, x, y, 34.0f, Theme::textPrimary(), ellipsize(it->name, 34.0f, w));
  y += 52.0f;

  // Chips stay on one line; whatever does not fit is left out.
  float cx = x;
  auto chip = [&](const std::string &label, ImU32 bg, ImU32 fg) {
    float wChip = Theme::Measure(19.0f, label.c_str()).x + 26.0f;
    if (cx + wChip > P1.x - pad)
      return;
    cx += Theme::Pill(dl, cx, y, 19.0f, label.c_str(), bg, fg) + 8.0f;
  };
  chip(it->id, IM_COL32(255, 255, 255, 24), Theme::textMuted());
  const char *cat = Sfo::categoryName(info.category);
  if (it->isFolder)
    chip("Folder", IM_COL32(0, 0xA2, 0xFF, 80), Theme::white());
  else if (*cat)
    chip(cat, IM_COL32(255, 255, 255, 24), Theme::textMuted());
  if (it->fixedSlot)
    chip("Fixed #" + std::to_string(it->fixedSlot), Theme::accent(),
         Theme::white());
  else if (it->pinned)
    chip("Pinned", Theme::accent(), Theme::white());
  if (it->sentToEnd)
    chip("At the end", IM_COL32(255, 255, 255, 24), Theme::textMuted());
  if (ctl_.isMarked(it->id))
    chip("Selected", Theme::accentBright(), Theme::white());
  y += 52.0f;

  std::string ver = !info.appVer.empty() ? "v" + info.appVer : pending;
  std::string verSub =
      !info.version.empty() ? "base v" + info.version : std::string();
  text(dl, x, y, 16.0f, Theme::textDim(), "VERSION");
  text(dl, x, y + 22.0f, 24.0f, Theme::textPrimary(), ver);
  if (!verSub.empty())
    text(dl, x + measure(24.0f, ver).x + 10.0f, y + 26.0f, 18.0f,
         Theme::textMuted(), verSub);
  y += 66.0f;

  text(dl, x, y, 16.0f, Theme::textDim(), "LAST PLAYED");
  text(dl, x, y + 22.0f, 24.0f, Theme::textPrimary(), prettyDate(*it));
  y += 66.0f;

  if (!it->isFolder) {
    uint64_t total = info.baseBytes + info.patchBytes + info.addonBytes;
    text(dl, x, y, 16.0f, Theme::textDim(), "STORAGE");
    if (info.ready)
      textRight(dl, x + w, y - 3.0f, 21.0f, Theme::textPrimary(),
                formatBytes(total));
    ImVec2 ba(x, y + 28.0f), bb(x + w, y + 36.0f);
    Theme::FillRoundRect(dl, ba, bb, IM_COL32(255, 255, 255, 22), 4.0f);
    const ImU32 kBase = Theme::accentBright(),
                kPatch = Theme::rgba(0x8E, 0x7C, 0xFF),
                kAddon = Theme::rgba(0x4A, 0xE3, 0xA4);
    if (total) {
      float bx = ba.x;
      auto seg = [&](uint64_t bytes, ImU32 c) {
        float sw = w * (float)((double)bytes / (double)total);
        if (sw >= 1.0f)
          dl->AddRectFilled(ImVec2(bx, ba.y), ImVec2(bx + sw, bb.y), c);
        bx += sw;
      };
      seg(info.baseBytes, kBase);
      seg(info.patchBytes, kPatch);
      seg(info.addonBytes, kAddon);
    }
    float lx = x, ly = y + 48.0f;
    auto legend = [&](const char *name, uint64_t bytes, ImU32 c) {
      dl->AddCircleFilled(ImVec2(lx + 5.0f, ly + 11.0f), 5.0f, c, 12);
      std::string s =
          std::string(name) + " " + (info.ready ? formatBytes(bytes) : pending);
      text(dl, lx + 16.0f, ly, 19.0f, Theme::textMuted(), s);
      lx += 16.0f + measure(19.0f, s).x + 22.0f;
    };
    legend("Base", info.baseBytes, kBase);
    legend("Patch", info.patchBytes, kPatch);
    legend("DLC", info.addonBytes, kAddon);
  }

  std::vector<Hint> hints =
      ctl_.reorderMode()
          ? std::vector<Hint>{{Glyph::L1, "Back"}, {Glyph::R1, "Forward"}}
          : std::vector<Hint>{
                {Glyph::Square, "Options"},
                {Glyph::Cross, ctl_.isMarked(it->id) ? "Deselect" : "Select"}};
  drawHintRow(dl, P1.x - pad, P1.y - 44.0f, hints);

  if (e < 1.0f)
    scope.apply(e, ImVec2(0.0f, (1.0f - e) * 14.0f));
}

void UiRoot::drawDownloads(ImDrawList *dl, float dt) {
  auto &svc = NetworkService::instance();
  const bool on = svc.running();
  const float pulse = 0.5f + 0.5f * std::sin(time_ * 2.6f);
  const float leftX1 = 1100.0f, pad = 36.0f;

  // Server card: status, address, help text, then chips on their own row.
  // Height follows the content: status, address, then the chips row.
  ImVec2 a(M, kContentTop), b(leftX1, kContentTop + 257.0f);
  Theme::Glass(dl, a, b);
  float x = a.x + pad, y = a.y + 30.0f;
  const float hy =
      bandTextY(a.y, 27.0f); // status line centred in the title band
  ImVec2 dot(x + 8.0f, hy + measure(27.0f, "Ag").y * 0.5f);
  ImU32 dc = on ? Theme::success() : Theme::danger();
  if (on)
    dl->AddCircleFilled(dot, 8.0f + 8.0f * pulse,
                        Theme::alpha(dc, 0.22f * (1.0f - pulse)), 24);
  dl->AddCircleFilled(dot, 7.0f, dc, 20);
  text(dl, x + 28.0f, hy, 27.0f, Theme::textPrimary(),
       on ? "Ready to receive packages" : "Server stopped");
  if (on)
    Anim::activeFlag() = true;

  y += 62.0f;
  std::string host = localIp_.empty() ? "<console-ip>" : localIp_;
  text(dl, x, y, 16.0f, Theme::textDim(), "CONSOLE ADDRESS");
  text(dl, x, y + 24.0f, 44.0f, Theme::white(),
       host + ":" + std::to_string(on ? svc.port() : SERVER_PORT));
  float px = x, py = b.y - pad - 31.0f;
  px += Theme::Pill(
            dl, px, py, 19.0f,
            Platform::bgftReady() ? "Downloads ready" : "Downloads unavailable",
            Platform::bgftReady() ? Theme::alpha(Theme::success(), 0.22f)
                                  : Theme::alpha(Theme::danger(), 0.22f),
            Theme::textPrimary()) +
        8.0f;
  px += Theme::Pill(dl, px, py, 19.0f, "PS4 RPI (flatZ)",
                    IM_COL32(255, 255, 255, 24), Theme::textMuted()) +
        8.0f;
  Theme::Pill(dl, px, py, 19.0f, "Local network only",
              IM_COL32(255, 255, 255, 24), Theme::textMuted());

  // Transfers: icon, title, state, progress bar and sizes, each with room.
  text(dl, M, b.y + 26.0f, 16.0f, Theme::textDim(), "TRANSFERS");
  auto tasks = svc.tasks();
  const int n = (int)tasks.size();
  const float rowH = 122.0f, top = b.y + 54.0f, inner = rowH - 12.0f;
  if (n == 0) {
    Theme::DrawIcon(dl, ImVec2(M, top + 6.0f), 26.0f, Icon::Download,
                    Theme::textDim());
    text(dl, M + 42.0f, top + 4.0f, 23.0f, Theme::textMuted(),
         "Nothing downloading");
  } else {
    downloadsCursor_ = std::min(downloadsCursor_, n - 1);
    int visible = std::max(1, (int)((kContentBot - top + 12.0f) / rowH));
    int first =
        std::max(0, std::min(downloadsCursor_ - visible + 1, n - visible));
    float hy = downloadsHighlightY_.step(
        top + (downloadsCursor_ - first) * rowH, dt, 5.5f, 0.85f);
    for (int i = first; i < std::min(n, first + visible); ++i) {
      const InstallTask &t = tasks[n - 1 - i]; // newest first
      float ry = top + (i - first) * rowH;
      ImVec2 ta(M, ry), tb(leftX1, ry + inner);
      Theme::FillRoundRect(dl, ta, tb, IM_COL32(255, 255, 255, 14), 16.0f);
      if (i == downloadsCursor_)
        Theme::RowHighlight(dl, ImVec2(M, hy), ImVec2(leftX1, hy + inner),
                            1.0f);

      // Package icon (extracted from the pkg header), placeholder otherwise.
      const float is = (float)IconCache::kSmallPx;
      ImVec2 ia(ta.x + 18.0f, ta.y + (inner - is) * 0.5f);
      ImTextureID tex =
          t.iconPath.empty()
              ? 0
              : IconCache::instance().getFile(t.iconPath, IconSize::Small);
      if (tex)
        dl->AddImage(ImTextureRef(tex), ia, ImVec2(ia.x + is, ia.y + is));
      else {
        Theme::FillRoundRect(dl, ia, ImVec2(ia.x + is, ia.y + is),
                             placeholderColor(t.contentId), 12.0f);
        Theme::DrawIcon(dl, ImVec2(ia.x + is * 0.3f, ia.y + is * 0.3f),
                        is * 0.4f, Icon::Download, Theme::white());
      }

      float frac =
          t.length ? (float)((double)t.transferred / (double)t.length) : 0.0f;
      float &shown = taskProgress_.get(std::to_string(t.taskId), 0.0f);
      Anim::approach(shown, std::min(frac, 1.0f), 5.0f, dt);

      std::string state;
      ImU32 sc = Theme::textMuted();
      if (t.error) {
        char e[32];
        std::snprintf(e, sizeof(e), "Error 0x%08X", (unsigned)t.error);
        state = e;
        sc = Theme::danger();
      } else if (t.complete) {
        state = "Complete";
        sc = Theme::success();
      } else if (t.stopped)
        state = "Stopped";
      else if (t.paused)
        state = "Paused";
      else
        state = std::to_string((int)(shown * 100.0f)) + "%";

      float cx0 = ia.x + is + 20.0f, cx1 = tb.x - 24.0f;
      float sw = measure(21.0f, state).x;
      text(dl, cx0, ta.y + 16.0f, 24.0f, Theme::textPrimary(),
           ellipsize(Sfo::cleanTitle(t.title).empty()
                         ? t.contentId
                         : Sfo::cleanTitle(t.title),
                     24.0f, cx1 - cx0 - sw - 24.0f));
      textRight(dl, cx1, ta.y + 18.0f, 21.0f, sc, state);
      ImVec2 ba(cx0, ta.y + 56.0f), bb(cx1, ta.y + 62.0f);
      Theme::FillRoundRect(dl, ba, bb, IM_COL32(255, 255, 255, 26), 3.0f);
      if (shown > 0.0f)
        Theme::FillRoundRect(dl, ba, ImVec2(ba.x + (bb.x - ba.x) * shown, bb.y),
                             t.error                   ? Theme::danger()
                             : (t.paused || t.stopped) ? Theme::textDim()
                                                       : Theme::accentBright(),
                             3.0f);
      text(dl, cx0, ta.y + 74.0f, 20.0f, Theme::textMuted(),
           formatBytes(t.transferred) + " of " + formatBytes(t.length));
      if (t.restSec && !t.complete && !t.stopped && !t.error)
        textRight(dl, cx1, ta.y + 74.0f, 20.0f, Theme::textMuted(),
                  formatEta(t.restSec));
    }
  }

  ImVec2 la(1140.0f, kContentTop), lb(SW - M, kContentBot);
  Theme::Glass(dl, la, lb);
  float lx = la.x + pad;
  text(dl, lx, bandTextY(la.y, 16.0f), 16.0f, Theme::textDim(), "ACTIVITY");
  auto log = svc.log();
  float ly = la.y + 68.0f;
  if (log.empty())
    text(dl, lx, ly, 22.0f, Theme::textMuted(), "No requests yet");
  int shownLines = 0;
  for (auto it = log.rbegin(); it != log.rend() && ly + 30.0f < lb.y - pad;
       ++it, ly += 38.0f, ++shownLines) {
    bool err = it->rfind("ERROR", 0) == 0;
    float fade = std::max(0.35f, 1.0f - shownLines * 0.045f);
    dl->AddCircleFilled(
        ImVec2(lx + 5.0f, ly + 12.0f), 3.5f,
        Theme::alpha(err ? Theme::danger() : Theme::accentBright(), fade), 12);
    text(dl, lx + 20.0f, ly, 20.0f,
         Theme::alpha(err ? Theme::danger() : Theme::textMuted(), fade),
         ellipsize(*it, 20.0f, lb.x - lx - pad - 20.0f));
  }
}

void UiRoot::drawSystem(ImDrawList *dl, float dt) {
  const auto &st = Platform::status();

  ImVec2 a(M, kContentTop), b(900.0f, 520.0f);
  Theme::Glass(dl, a, b);
  text(dl, a.x + 36.0f, bandTextY(a.y, 17.0f), 17.0f, Theme::textDim(),
       "STORAGE  " DISK_MOUNT_POINT);
  if (storage_.ok) {
    float used = 1.0f - (float)((double)storage_.freeBytes /
                                (double)storage_.totalBytes);
    Anim::approach(storageFill_, used, 5.0f, dt);
    std::string big = formatBytes(storage_.freeBytes);
    text(dl, a.x + 36.0f, a.y + 62.0f, 58.0f, Theme::white(), big);
    text(dl, a.x + 46.0f + measure(58.0f, big).x, a.y + 92.0f, 25.0f,
         Theme::textMuted(), "free");
    text(dl, a.x + 36.0f, a.y + 140.0f, 22.0f, Theme::textMuted(),
         "of " + formatBytes(storage_.totalBytes));
    char pct[32];
    std::snprintf(pct, sizeof(pct), "%d%% used", (int)(used * 100.0f + 0.5f));
    textRight(dl, b.x - 36.0f, a.y + 140.0f, 22.0f, Theme::textMuted(), pct);
    ImVec2 ba(a.x + 36.0f, a.y + 192.0f), bb(b.x - 36.0f, a.y + 204.0f);
    Theme::FillRoundRect(dl, ba, bb, IM_COL32(255, 255, 255, 24), 6.0f);
    ImU32 fill = used > 0.95f   ? Theme::danger()
                 : used > 0.85f ? Theme::warning()
                                : Theme::accentBright();
    Theme::FillRoundRect(
        dl, ba, ImVec2(ba.x + (bb.x - ba.x) * storageFill_, bb.y), fill, 6.0f);
  } else {
    text(dl, a.x + 36.0f, a.y + 80.0f, 28.0f, Theme::textMuted(),
         "Storage information unavailable");
  }

  ImVec2 sa(M, 550.0f), sb(900.0f, kContentBot);
  Theme::Glass(dl, sa, sb);
  text(dl, sa.x + 36.0f, bandTextY(sa.y, 17.0f), 17.0f, Theme::textDim(),
       "STATUS");
  const char *tiles = "Unknown";
  switch (ctl_.defaultTiles()) {
  case DefaultTiles::Visible:
    tiles = "Visible";
    break;
  case DefaultTiles::Hidden:
    tiles = "Hidden";
    break;
  case DefaultTiles::Mixed:
    tiles = "Partially hidden";
    break;
  case DefaultTiles::Missing:
    tiles = "Not found";
    break;
  default:
    break;
  }
  auto &svc = NetworkService::instance();
  struct Row {
    const char *k;
    std::string v;
    bool ok;
  };
  std::vector<Row> rows = {
      {"System access", st.jailbroken ? "Granted" : "Not granted",
       st.jailbroken},
      {"Home screen database",
       ctl_.dbReady() ? ctl_.dbTable() : "Not accessible", ctl_.dbReady()},
      {"What's New & Store", tiles,
       ctl_.defaultTiles() != DefaultTiles::Unknown},
      {"Hidden tiles", std::to_string(ctl_.hiddenItems().size()), true},
      {"Leftover entries", std::to_string(ctl_.leftoverCount()),
       ctl_.leftoverCount() == 0},
      {"App management", st.appInstUtil ? "Ready" : "Unavailable",
       st.appInstUtil},
      {"Remote install server",
       svc.running() ? "Port " + std::to_string(svc.port()) : "Stopped",
       svc.running()},
  };
  float y = sa.y + 74.0f;
  for (auto &r : rows) {
    dl->AddCircleFilled(ImVec2(sa.x + 44.0f, y + 14.0f), 5.5f,
                        r.ok ? Theme::success() : Theme::danger(), 16);
    text(dl, sa.x + 64.0f, y, 23.0f, Theme::textPrimary(), r.k);
    textRight(dl, sb.x - 36.0f, y, 23.0f, Theme::textMuted(),
              ellipsize(r.v, 23.0f, 330.0f));
    y += 50.0f;
  }

  auto entries = systemEntries();
  drawEntryList(dl, dt, entries, "MAINTENANCE", systemCursor_, systemScroll_,
                systemScrollTarget_, systemHighlightY_, systemHighlightInit_);
}

// Right-hand list used by System and Plugins: rows with title + detail,
// a moving highlight, scrolling with the cursor and a scroll indicator.
void UiRoot::drawEntryList(ImDrawList *dl, float dt,
                           const std::vector<Entry> &entries,
                           const char *heading, int &cursor,
                           Anim::Spring &scrollSpring, float &scrollTarget,
                           Anim::Spring &highlight, bool &highlightInit) {
  const float lx0 = 940.0f, lx1 = SW - M, rowH = 104.0f,
              top = kContentTop + 44.0f;
  const float categoryH = 46.0f;
  const float viewH = kContentBot - top;
  text(dl, lx0 + 8.0f, kContentTop + 4.0f, 17.0f, Theme::textDim(), heading);
  const int count = (int)entries.size();
  if (!count)
    return;
  cursor = std::min(cursor, count - 1);

  // Reserve category header height when the group changes.
  std::vector<float> yOf(count);
  std::vector<bool> catHere(count, false);
  float cursorPos = 0.0f;
  std::string prev;
  for (int i = 0; i < count; ++i) {
    if (!entries[i].category.empty() && entries[i].category != prev) {
      catHere[i] = true;
      cursorPos += categoryH;
    }
    yOf[i] = cursorPos;
    cursorPos += rowH;
    prev = entries[i].category;
  }
  float totalH = cursorPos;

  float rowTop = yOf[cursor];
  if (rowTop < scrollTarget)
    scrollTarget = rowTop;
  if (rowTop + rowH > scrollTarget + viewH)
    scrollTarget = rowTop + rowH - viewH;
  scrollTarget =
      std::max(0.0f, std::min(scrollTarget, std::max(0.0f, totalH - viewH)));
  float scroll = scrollSpring.step(scrollTarget, dt, 5.0f, 1.0f);
  if (!highlightInit) {
    highlight = Anim::Spring(rowTop);
    highlightInit = true;
  }
  float hy = highlight.step(rowTop, dt, 5.5f, 0.85f) + top - scroll;
  dl->PushClipRect(ImVec2(lx0 - 24.0f, top - 12.0f),
                   ImVec2(SW, kContentBot + 8.0f), true);
  Theme::RowHighlight(dl, ImVec2(lx0, hy), ImVec2(lx1, hy + rowH - 14.0f),
                      1.0f);
  for (int i = 0; i < count; ++i) {
    const Entry &e = entries[i];
    float ry = top + yOf[i] - scroll;
    if (catHere[i]) {
      float cy = ry - categoryH;
      if (cy + categoryH >= top - categoryH && cy <= kContentBot) {
        text(dl, lx0 + 8.0f, cy + 14.0f, 17.0f, Theme::textDim(), e.category);
        dl->AddLine(ImVec2(lx0 + 8.0f, cy + categoryH - 4.0f),
                    ImVec2(lx1 - 8.0f, cy + categoryH - 4.0f),
                    IM_COL32(255, 255, 255, 30), 1.0f);
      }
    }
    if (ry + rowH < top - rowH || ry > kContentBot)
      continue;
    bool f = i == cursor;
    ImU32 title =
        !e.enabled ? Theme::textDim()
                   : (e.danger && !f ? Theme::danger() : Theme::textPrimary());
    text(dl, lx0 + 32.0f, ry + 18.0f, 29.0f, title,
         ellipsize(e.label, 29.0f, lx1 - lx0 - 120.0f));
    text(dl, lx0 + 32.0f, ry + 58.0f, 21.0f,
         e.enabled ? Theme::textMuted() : Theme::textDim(),
         ellipsize(e.detail, 21.0f, lx1 - lx0 - 120.0f));
    if (e.enabled) {
      Icon chevron = e.check == 2 ? Icon::ChevronDown : Icon::Chevron;
      Theme::DrawIcon(dl, ImVec2(lx1 - 56.0f, ry + 36.0f), 26.0f, chevron,
                      f ? Theme::white() : Theme::textDim());
    }
  }
  dl->PopClipRect();
  if (totalH > viewH) {
    float tx = lx1 + 14.0f, h = viewH * viewH / totalH;
    float ty = top + (viewH - h) * Anim::clamp01(scroll / (totalH - viewH));
    Theme::FillRoundRect(dl, ImVec2(tx, top), ImVec2(tx + 4, kContentBot),
                         IM_COL32(255, 255, 255, 16), 2.0f);
    Theme::FillRoundRect(dl, ImVec2(tx, ty), ImVec2(tx + 4, ty + h),
                         IM_COL32(255, 255, 255, 110), 2.0f);
  }
}

void UiRoot::setList(const std::string &title, bool keepPosition) {
  if (!keepPosition || overlay_ != Overlay::Options) {
    showList(title);
    return;
  }
  optionsTitle_ = title;
  optionsCursor_ =
      std::min(optionsCursor_, std::max(0, (int)options_.size() - 1));
}

const UiRoot::PluginStatus &UiRoot::pluginStatus() {
  if (time_ - plugStamp_ < 1.0f)
    return plug_;
  plugStamp_ = time_;
  plug_.ytPlugin = YtAdBlock::pluginInstalled();
  plug_.ytIni = YtAdBlock::iniHasEntry();
  plug_.share = GhPlugins::shareEnabled();
  plug_.patch = GhPlugins::patchSupportInstalled();
  plug_.patchDb = GhPlugins::patchDatabaseVersion();
  plug_.netProfiles = NetProfiles::list().size();
  plug_.autoloader = GhPlugins::autoloaderInstalled();
  plug_.webkitInstalled = WebkitBrowser::isInstalled();
  plug_.webkitBackup = WebkitBrowser::backupExists();
  plug_.webkitTitleId = WebkitBrowser::resolvedTitleId();
  return plug_;
}

std::vector<UiRoot::Entry> UiRoot::pluginEntries() {
  std::vector<Entry> v;
  UiRoot *self = this;
  auto &jobs = GhPlugins::Jobs::instance();
  const bool busy = jobs.running();
  const std::string busyText =
      busy ? jobs.name() + ": " + jobs.message() : std::string();

  // PS4Webkit integration
  const PluginStatus &st = pluginStatus();
  std::string ipNow = localIp_;
  if (ipNow.empty())
    ipNow = "<PS4-IP>";
  {
    bool alOn = st.autoloader;
    bool wkOn = st.webkitInstalled;
    bool wkBackup = st.webkitBackup;
    std::string wkId = st.webkitTitleId;
    bool wkAvail = !wkId.empty();
    bool fullyOn = alOn && wkOn;

    auto doInstall = [self] {
      std::string alMsg;
      bool alOk = GhPlugins::installAutoloaderSync(alMsg);
      self->invalidatePluginStatus();
      if (!alOk) {
        self->ctl_.notify(alMsg, true);
        return;
      }

      std::string ip = Platform::localIp();
      std::string url = "http://" + (ip.empty() ? "<console-ip>" : ip) + ":" +
                        std::to_string(NetworkService::instance().port()) +
                        "/load/";
      WebkitBrowser::addLoaderBookmark(url);

      if (!WebkitBrowser::startInstall(url))
        self->ctl_.notify("Another job is running", true);
      else
        self->ctl_.notify(
            "Autoloader + bookmark ready - updating launch URL...");
    };

    auto doRestore = [self] {
      std::string err;
      GhPlugins::removeAutoloader(err);
      WebkitBrowser::removeLoaderBookmark();
      if (!WebkitBrowser::startRestore())
        self->ctl_.notify("No backup or another job is running", true);
      self->invalidatePluginStatus();
    };

    const char *kBrowserCat = "BROWSER & XMB INTEGRATIONS";
    if (fullyOn) {
      Entry e1{"PS4Webkit: Installed",
               "Open browser and visit http://" + ipNow +
                   ":12800/load/ to cache the exploit",
               nullptr, false, false};
      e1.category = kBrowserCat;
      v.push_back(std::move(e1));
      v.push_back(
          {"Reinstall PS4Webkit",
           busy && (jobs.name() == "PS4Webkit")
               ? busyText
               : "Re-copy autoloader files and update browser launch URL",
           doInstall, false, !busy});
      v.push_back({"Restore Defaults",
                   busy && jobs.name() == "Restore Browser" ? busyText
                   : wkBackup ? "Remove autoloader files, bookmark and revert "
                                "browser launch URL"
                              : "No backup found",
                   doRestore, true, !busy && wkBackup,
                   "Restore Defaults?\nDeletes autoloader files, removes the "
                   "loader bookmark and reverts the browser launch URL to "
                   "factory state."});
    } else {
      Entry e1{"Install PS4Webkit",
               busy && (jobs.name() == "PS4Webkit") ? busyText
               : wkAvail
                   ? "Copy autoloader, add bookmark and set browser launch URL"
                   : "System browser Title ID not found (jailbreak required)",
               doInstall, false, !busy && wkAvail};
      e1.category = kBrowserCat;
      v.push_back(std::move(e1));
      if (wkBackup) {
        v.push_back({"Restore Defaults",
                     "Remove autoloader files, bookmark and revert browser "
                     "launch URL",
                     doRestore, true, !busy,
                     "Restore Defaults?\nDeletes autoloader files, removes the "
                     "loader bookmark and reverts the browser launch URL to "
                     "factory state."});
      }
    }
  }

  // App Catalog - opens its own sub-screen (zoom-in transition), not an
  // inline accordion.
  {
    Entry e{"App Catalog", "Install or update community homebrew apps",
            [self] { self->catalogExpanded_ = true; }};
    e.category = "APP CATALOG";
    v.push_back(std::move(e));
  }

  bool ytOn = st.ytPlugin && st.ytIni;
  {
    Entry e{"YouTube AdBlock + SponsorBlock",
            busy && jobs.name() == "YouTube AdBlock" ? busyText
            : ytOn ? "Installed - settings, update or remove"
                   : "Block ads and skip sponsor segments in YouTube",
            [self] {
              self->yt_ = YtAdBlock::loadSettings();
              self->openYoutubeList(false);
            }};
    e.category = "GOLDHEN PLUGINS";
    v.push_back(std::move(e));
  }
  if (st.ytPlugin || st.ytIni)
    v.push_back({"Uninstall YouTube AdBlock",
                 "Remove the plugin, its SponsorBlock settings and its "
                 "plugins.ini line",
                 [self] {
                   std::string err;
                   if (YtAdBlock::uninstall(err)) {
                     self->yt_ = YtAdBlock::Settings{};
                     self->ctl_.notify("YouTube AdBlock removed");
                   } else
                     self->ctl_.notify("Uninstall failed: " + err, true);
                 },
                 true, !busy,
                 "Uninstall YouTube AdBlock + SponsorBlock?\nThe plugin, its "
                 "SponsorBlock settings and "
                 "its plugins.ini line are deleted. Other GoldHEN plugins are "
                 "not touched."});

  bool shareOn = st.share;
  if (shareOn) {
    v.push_back(
        {"No Share Watermark & Blocks: On",
         "Screenshots and videos everywhere, no watermark - select to turn off",
         [self] {
           std::string err;
           if (GhPlugins::disableShare(err))
             self->ctl_.notify("No Share Watermark & Blocks turned off");
           else
             self->ctl_.notify("Could not turn it off: " + err, true);
         }});
  } else {
    v.push_back({"No Share Watermark & Blocks: Off",
                 busy && jobs.name() == "No Share Watermark & Blocks"
                     ? busyText
                     : "Allow screenshots / video in blocked scenes and remove "
                       "watermarks",
                 [self] {
                   if (!GhPlugins::startShareInstall())
                     self->ctl_.notify("Another download is running", true);
                 },
                 false, !busy});
  }

  bool patchOn = st.patch;
  std::string dbv = st.patchDb;
  v.push_back(
      {patchOn ? "Update game patches" : "Install game patches",
       busy && jobs.name() == "Game patches" ? busyText
       : patchOn
           ? "Patch database " +
                 (dbv.empty() ? std::string("installed") : dbv) +
                 " - download the latest"
           : "game_patch plugin + patch database (60 FPS, no motion blur, ...)",
       [self] {
         if (!GhPlugins::startPatchSupportInstall())
           self->ctl_.notify("Another download is running", true);
       },
       false, !busy});
  v.push_back({"Game patches",
               patchOn ? "Turn patches on or off for your installed games"
                       : "Install game patches first",
               [self] { self->openPatchGames(); }, false, patchOn});
  if (patchOn || !dbv.empty())
    v.push_back(
        {"Remove game patches",
         "Delete the patch database, patch settings and the game_patch plugin",
         [self] {
           std::string err;
           if (GhPlugins::removePatchSupport(err))
             self->ctl_.notify("Game patches removed");
           else
             self->ctl_.notify("Remove failed: " + err, true);
         },
         true, !busy,
         "Remove game patches?\nThe patch database, all patch on/off settings "
         "and the game_patch plugin are "
         "deleted. Games start without patches. You can install them again any "
         "time."});

  {
    size_t np = st.netProfiles;
    Entry e{"Network profiles" +
                (np ? " (" + std::to_string(np) + ")" : std::string()),
            "Save and restore LAN settings (LAN only - Wi-Fi profiles are not "
            "supported)",
            [self] { self->openNetworkList(false); }};
    e.category = "SYSTEM & MAINTENANCE";
    v.push_back(std::move(e));
  }

  v.push_back({"Background services",
               "Status of FTP, klog and payload loaders; stop homebrew ones",
               [self] { self->openServicesList(); }});
  v.push_back({"Clean temporary files",
               "Delete scratch/debug temporaries (does NOT touch browser data, "
               "cookies, cache or savedata)",
               [self] {
                 std::string msg = GhPlugins::cleanTemporaryFiles();
                 self->ctl_.notify(msg);
               },
               false, true});
  {
    // Two explicit rows (block / allow); the one matching the current
    // state is shown as active but greyed out.
    const bool known = blockerState_ >= 0;
    const bool on = blockerState_ == (int)UpdateBlocker::State::On;
    const bool partial = blockerState_ == (int)UpdateBlocker::State::Partial;
    auto apply = [self](bool block) {
      std::string err;
      bool ok =
          block ? UpdateBlocker::enable(err) : UpdateBlocker::disable(err);
      self->ctl_.notify(ok ? (block ? "System updates are blocked"
                                    : "System updates are allowed again")
                           : "Update blocker: " + err,
                        !ok);
      self->blockerState_ = (int)UpdateBlocker::state();
    };
    v.push_back(
        {"Block system updates",
         !known    ? "Checking..."
         : on      ? "Active - firmware updates cannot be downloaded"
         : partial ? "Partly set up - select to finish"
                   : "Stops the console from downloading firmware updates",
         [apply] { apply(true); }, false, known && !on,
         "Block system updates?\nDummy PS4UPDATE.PUP folders are created in "
         "/update so the console "
         "cannot store a firmware update. You can allow updates again here."});
    v.push_back({"Allow system updates",
                 !known            ? "Checking..."
                 : (on || partial) ? "Removes the update blocker folders"
                                   : "Active - updates are not blocked",
                 [apply] { apply(false); }, true, known && (on || partial),
                 "Allow system updates again?\nThe console can download and "
                 "install firmware updates, which "
                 "can remove the jailbreak."});
  }
  return v;
}

void UiRoot::openYoutubeList(bool keep) {
  options_.clear();
  auto &jobs = GhPlugins::Jobs::instance();
  bool installed = YtAdBlock::pluginInstalled();
  int picked = 0;
  for (bool b : yt_.selected)
    picked += b ? 1 : 0;

  options_.push_back(
      {installed ? "Update plugin" : "Install plugin",
       jobs.running()
           ? jobs.message()
           : "Saves the settings and downloads the latest plugin from GitHub",
       [this] {
         if (!YtAdBlock::startInstall(yt_))
           ctl_.notify("Another download is running", true);
       },
       false, !jobs.running()});
  if (installed || yt_.fileExists)
    options_.push_back(
        {"Uninstall",
         "Remove the plugin, its settings and its plugins.ini line",
         [this] {
           std::string err;
           if (YtAdBlock::uninstall(err)) {
             yt_ = YtAdBlock::Settings{};
             ctl_.notify("YouTube AdBlock removed");
           } else
             ctl_.notify("Uninstall failed: " + err, true);
         },
         true, true,
         "Uninstall YouTube AdBlock + SponsorBlock?\nThe plugin, its "
         "SponsorBlock settings and "
         "its plugins.ini line are deleted."});
  options_.push_back({std::string("Skip mode: ") +
                          (yt_.autoSkip ? "Auto Skip" : "Ask for Confirmation"),
                      "Select to switch", [this] {
                        yt_.autoSkip = !yt_.autoSkip;
                        openYoutubeList(true);
                      }});
  options_.back().stay = true;
  Entry e{"Save settings",
          std::to_string(picked) + " of 12 categories, " +
              (yt_.autoSkip ? "auto skip" : "ask first"),
          [this] {
            std::string err;
            if (YtAdBlock::saveSettings(yt_, err))
              ctl_.notify("SponsorBlock settings saved - restart YouTube");
            else
              ctl_.notify("Saving failed: " + err, true);
            openYoutubeList(true);
          }};
  e.stay = true;
  options_.push_back(e);
  Entry all{picked == YtAdBlock::kCategoryCount ? "Clear all categories"
                                                : "Select all categories",
            "SponsorBlock segments to skip", [this, picked] {
              bool v = picked != YtAdBlock::kCategoryCount;
              for (bool &b : yt_.selected)
                b = v;
              openYoutubeList(true);
            }};
  all.stay = true;
  options_.push_back(all);
  for (int i = 0; i < YtAdBlock::kCategoryCount; ++i) {
    Entry c{YtAdBlock::kCategories[i].label,
            yt_.selected[i] ? "Skipped" : "Not skipped", [this, i] {
              yt_.selected[i] = !yt_.selected[i];
              openYoutubeList(true);
            }};
    c.stay = true;
    c.check = yt_.selected[i] ? 1 : 0;
    options_.push_back(c);
  }
  options_.push_back(
      {"Note", "GoldHEN must have plugins enabled before you start YouTube",
       nullptr, false, false});
  setList("YouTube AdBlock + SponsorBlock", keep);
}

void UiRoot::openPatchGames() {
  options_.clear();
  for (const AppItem &it : ctl_.items()) {
    if (it.isFolder || !CatalogDb::isPlainTitleId(it.id) ||
        !GhPlugins::hasPatches(it.id))
      continue;
    auto patches = GhPlugins::patchesFor(it.id);
    if (patches.empty())
      continue;
    int on = 0;
    for (auto &p : patches)
      on += p.enabled ? 1 : 0;
    std::string id = it.id, name = it.name;
    options_.push_back(
        {name,
         std::to_string(patches.size()) + " patches" +
             (on ? ", " + std::to_string(on) + " on" : std::string()),
         [this, id, name] { openGamePatches(id, name, false); }});
  }
  if (options_.empty())
    options_.push_back(
        {"No patches for your games",
         "The patch database has nothing for the installed titles", nullptr,
         false, false});
  showList("Game patches");
}

void UiRoot::openGamePatches(std::string id, std::string name, bool keep) {
  options_.clear();
  TitleInfo info = TitleInspector::instance().get(id);
  std::string installed = info.appVer;
  for (const GhPlugins::GamePatch &p : GhPlugins::patchesFor(id)) {
    bool match = installed.empty() || p.appVer == installed ||
                 p.appVer == "mask" || p.appVer == "all";
    std::string detail =
        "v" + p.appVer + (match ? "" : " (installed v" + installed + ")");
    if (!p.note.empty())
      detail += " - " + p.note;
    GhPlugins::GamePatch copy = p;
    Entry e{Sfo::cleanTitle(p.name).empty() ? p.name : Sfo::cleanTitle(p.name),
            detail, [this, copy, id, name] {
              std::string err;
              if (!GhPlugins::setPatchEnabled(copy, !copy.enabled, err))
                ctl_.notify("Could not change the patch: " + err, true);
              openGamePatches(id, name, true);
            }};
    e.stay = true;
    e.check = p.enabled ? 1 : 0;
    options_.push_back(e);
  }
  options_.push_back(
      {"Back", "All games with patches", [this] { openPatchGames(); }});
  options_.push_back(
      {"Note", "Patches apply at game start; GoldHEN plugins must be enabled",
       nullptr, false, false});
  setList(Sfo::cleanTitle(name).empty() ? id : name, keep);
}

void UiRoot::openNetworkList(bool keep) {
  options_.clear();
  std::string err;
  if (!NetProfiles::available(err)) {
    options_.push_back({"Not available", err, nullptr, false, false});
    setList("Network profiles", keep);
    return;
  }
  Entry saveNow{"Save current settings",
                "Now: " + NetProfiles::currentSummary(), [this] {
                  std::string name, e;
                  if (NetProfiles::saveCurrent(name, e))
                    ctl_.notify("Network profile saved: " + name);
                  else
                    ctl_.notify("Could not save: " + e, true);
                  openNetworkList(true);
                }};
  saveNow.stay = true;
  options_.push_back(saveNow);
  for (const NetProfiles::Profile &pr : NetProfiles::list()) {
    std::string file = pr.file;
    options_.push_back(
        {pr.name, pr.summary, [this, file] { openNetworkActions(file); }});
  }
  options_.push_back(
      {"After applying",
       "Run Settings > Network > Test Internet Connection to use the profile",
       nullptr, false, false});
  options_.push_back({"Note",
                      "LAN only - Wi-Fi network profiles are not supported",
                      nullptr, false, false});
  setList("Network profiles", keep);
}

void UiRoot::openNetworkActions(const std::string &file) {
  NetProfiles::Profile pr;
  for (const auto &x : NetProfiles::list())
    if (x.file == file)
      pr = x;
  options_.clear();
  if (pr.file.empty()) {
    options_.push_back({"Profile not found", "It may have been deleted",
                        nullptr, false, false});
  } else {
    options_.push_back({"Apply this profile", pr.summary,
                        [this, pr] {
                          std::string err;
                          if (NetProfiles::apply(pr, err))
                            ctl_.notify(pr.name +
                                        " applied - run Settings > Network > "
                                        "Test Internet Connection to use it");
                          else
                            ctl_.notify("Apply failed: " + err, true);
                        },
                        false, true,
                        "Apply " + pr.name + "?\n" + pr.summary +
                            ". Then run Settings > Network > Test Internet "
                            "Connection so the console starts using it."});
    options_.push_back(
        {"Delete profile", "Remove this saved setup",
         [this, pr] {
           std::string err;
           if (NetProfiles::remove(pr, err))
             ctl_.notify(pr.name + " deleted");
           else
             ctl_.notify("Delete failed: " + err, true);
         },
         true, true,
         "Delete " + pr.name +
             "?\nThe console's current settings are not changed."});
  }
  options_.push_back(
      {"Back", "All network profiles", [this] { openNetworkList(false); }});
  showList(pr.name.empty() ? "Network profiles" : pr.name);
}

void UiRoot::drawPlugins(ImDrawList *dl, float dt) {
  ImVec2 a(M, kContentTop), b(900.0f, kContentBot);
  Theme::Glass(dl, a, b);
  text(dl, a.x + 36.0f, bandTextY(a.y, 17.0f), 17.0f, Theme::textDim(),
       "GOLDHEN PLUGINS");
  struct Row {
    const char *k;
    std::string v;
    bool ok;
  };
  const PluginStatus &st = pluginStatus();
  const std::string &dbv = st.patchDb;
  std::vector<Row> rows = {
      {"PS4Webkit",
       st.webkitInstalled ? ("Active (" + st.webkitTitleId + ")")
                          : "Stock Browser",
       st.webkitInstalled},
      {"Autoloader",
       st.autoloader ? "Installed - open browser to :12800/load/"
                     : "Not installed",
       st.autoloader},
      {"YouTube AdBlock",
       st.ytPlugin ? (st.ytIni ? "Installed" : "Not in plugins.ini")
                   : "Not installed",
       st.ytPlugin && st.ytIni},
      {"No Share Watermark & Blocks", st.share ? "On" : "Off", st.share},
      {"Game patch plugin", st.patch ? "Installed" : "Not installed", st.patch},
      {"Patch database", dbv.empty() ? "Not installed" : dbv, !dbv.empty()},
      {"Network profiles", std::to_string(st.netProfiles) + " saved",
       st.netProfiles > 0},
  };
  float y = a.y + 84.0f;
  for (auto &r : rows) {
    dl->AddCircleFilled(ImVec2(a.x + 44.0f, y + 14.0f), 5.5f,
                        r.ok ? Theme::success() : Theme::textDim(), 16);
    text(dl, a.x + 64.0f, y, 23.0f, Theme::textPrimary(), r.k);
    textRight(dl, b.x - 36.0f, y, 23.0f, Theme::textMuted(),
              ellipsize(r.v, 23.0f, 300.0f));
    y += 50.0f;
  }
  auto &jobs = GhPlugins::Jobs::instance();
  if (jobs.running()) {
    y += 10.0f;
    Theme::Spinner(dl, ImVec2(a.x + 50.0f, y + 14.0f), 12.0f, time_,
                   Theme::accentBright());
    text(dl, a.x + 74.0f, y, 22.0f, Theme::textPrimary(),
         ellipsize(jobs.message(), 22.0f, b.x - a.x - 120.0f));
    y += 50.0f;
  }
  float ny = b.y - 36.0f - 3 * 30.0f;
  dl->AddLine(ImVec2(a.x + 36.0f, ny - 22.0f), ImVec2(b.x - 36.0f, ny - 22.0f),
              IM_COL32(255, 255, 255, 22), 1.0f);
  Theme::DrawIcon(dl, ImVec2(a.x + 36.0f, ny + 2.0f), 24.0f, Icon::Alert,
                  Theme::warning());
  for (auto &l : wrap("GoldHEN must have plugins enabled (GoldHEN menu > "
                      "Plugins) before you start a game or YouTube. "
                      "Changes apply the next time the game starts.",
                      21.0f, b.x - a.x - 112.0f, 3)) {
    text(dl, a.x + 74.0f, ny, 21.0f, Theme::textMuted(), l);
    ny += 30.0f;
  }

  // App Catalog swap animation (PS4-style): outgoing list zooms OUT and
  // fades, incoming list zooms IN from larger scale and fades in. Same
  // slot, no sideways slide.
  float target = catalogExpanded_ ? 1.0f : 0.0f;
  Anim::approach(catalogExpandAnim_, target, 12.0f, dt);

  // Draw only one list at a time so the two don't visibly stack.
  bool showCatalog = catalogExpandAnim_ > 0.5f;
  // Pivot of the scale: centre of the right-hand list panel (lx0..SW-M).
  const float pivotX = (940.0f + SW - M) * 0.5f;
  const float pivotY = (kContentTop + kContentBot) * 0.5f;

  if (!showCatalog) {
    Anim::VtxScope listScope(dl);
    auto entries = pluginEntries();
    drawEntryList(dl, dt, entries, "PLUGINS & TOOLS", pluginsCursor_,
                  pluginsScroll_, pluginsScrollTarget_, pluginsHighlightY_,
                  pluginsHighlightInit_);
    // 0..0.5 of the animation = plugins zoom OUT (1.0 -> 0.9) + fade.
    float t = Anim::easeOutCubic(catalogExpandAnim_ * 2.0f);
    float alpha = 1.0f - t;
    float scale = 1.0f - 0.1f * t;
    listScope.apply(alpha, ImVec2(0, 0), scale, ImVec2(pivotX, pivotY));
    if (catalogExpandAnim_ > 0.001f)
      Anim::activeFlag() = true;
  } else {
    Anim::VtxScope listScope(dl);
    drawCatalogScreen(dl, dt);
    // 0.5..1 of the animation = catalog zoom IN (1.1 -> 1.0) + fade.
    float t = Anim::easeOutCubic((catalogExpandAnim_ - 0.5f) * 2.0f);
    float alpha = t;
    float scale = 1.1f - 0.1f * t;
    listScope.apply(alpha, ImVec2(0, 0), scale, ImVec2(pivotX, pivotY));
    if (catalogExpandAnim_ < 0.999f)
      Anim::activeFlag() = true;
  }
}

void UiRoot::drawCatalogScreen(ImDrawList *dl, float dt) {
  auto &jobs = GhPlugins::Jobs::instance();
  const bool busy = jobs.running();
  const std::string busyText =
      busy ? jobs.name() + ": " + jobs.message() : std::string();

  const auto &apps = AppCatalog::entries();
  UiRoot *self = this;

  // Build the sub-list that reuses drawEntryList so geometry matches the
  // plugins list it replaces (same right-side slot, no second card).
  std::vector<Entry> v;
  for (const auto &a : apps) {
    std::string tid = a.titleId;
    std::string detail;
    if (busy && jobs.name() == a.name) {
      detail = busyText;
    } else {
      const std::string &inst = a.installedVer;
      const std::string &net = a.version;
      if (!inst.empty() && !net.empty())
        detail = "Installed " + inst + "   -   Latest " + net;
      else if (!inst.empty())
        detail = "Installed " + inst;
      else if (!net.empty())
        detail = "Latest " + net + " - not installed";
      else
        detail = "Download latest version";
    }
    v.push_back({a.name, detail,
                 [self, tid] {
                   std::string err;
                   if (!AppCatalog::startInstall(tid, err))
                     self->ctl_.notify(err, true);
                 },
                 false, !busy});
  }
  (void)self;

  catalogCursor_ = std::min(catalogCursor_, (int)v.size() - 1);
  if (catalogCursor_ < 0)
    catalogCursor_ = 0;
  drawEntryList(dl, dt, v, "APP CATALOG", catalogCursor_, catalogScroll_,
                catalogScrollTarget_, catalogHighlightY_,
                catalogHighlightInit_);
}

void UiRoot::drawPicker(ImDrawList *dl, float dt) {
  (void)dt;
  float e = Anim::easeOutCubic(overlayAnim_);
  Theme::FillRoundRect(dl, ImVec2(0, 0), ImVec2(SW, SH),
                       IM_COL32(0, 4, 14, (int)(160 * e)));
  Anim::VtxScope scope(dl);
  const float w = 760.0f, h = 420.0f;
  ImVec2 a((SW - w) * 0.5f, (SH - h) * 0.5f), b(a.x + w, a.y + h);
  Theme::FillRoundRect(dl, a, b, IM_COL32(16, 28, 54, 246), 26.0f);
  dl->AddRect(a, b, Theme::glassEdge(), 26.0f, 0, 1.5f);

  text(dl, a.x + 56.0f, a.y + 44.0f, 17.0f, Theme::textDim(), "FIXED POSITION");
  text(dl, a.x + 56.0f, a.y + 70.0f, 32.0f, Theme::textPrimary(),
       ellipsize(pickerTitle_, 32.0f, w - 112.0f));

  float cy = a.y + 190.0f;
  std::string num = std::to_string(pickerValue_);
  textCenter(dl, SW * 0.5f, cy - 44.0f, 88.0f, Theme::white(), num);
  float half = std::max(60.0f, measure(88.0f, num).x * 0.5f) + 50.0f;
  ImU32 lc = pickerValue_ > 1 ? Theme::white() : Theme::textDim();
  ImU32 rc = pickerValue_ < pickerMax_ ? Theme::white() : Theme::textDim();
  float ax = SW * 0.5f - half, bx = SW * 0.5f + half;
  dl->AddTriangleFilled(ImVec2(ax, cy), ImVec2(ax + 22.0f, cy - 20.0f),
                        ImVec2(ax + 22.0f, cy + 20.0f), lc);
  dl->AddTriangleFilled(ImVec2(bx, cy), ImVec2(bx - 22.0f, cy - 20.0f),
                        ImVec2(bx - 22.0f, cy + 20.0f), rc);
  textCenter(dl, SW * 0.5f, cy + 54.0f, 21.0f, Theme::textMuted(),
             "of " + std::to_string(pickerMax_) +
                 " on the home screen    L1 / R1: 10 places");

  drawHintRow(dl, b.x - 56.0f, b.y - 50.0f,
              {{Glyph::Cross, "Set"}, {Glyph::Circle, "Cancel"}});
  float s = 0.95f + 0.05f * e;
  scope.apply(e, ImVec2(0, 0), s, ImVec2(SW * 0.5f, SH * 0.5f));
}

void UiRoot::drawHints(ImDrawList *dl) {
  std::vector<Hint> hints;
  if (overlay_ == Overlay::Confirm || overlay_ == Overlay::Picker)
    return;
  if (overlay_ == Overlay::Options) {
    hints = {{Glyph::Cross, "Select"}, {Glyph::Circle, "Close"}};
  } else if (ctl_.reorderMode()) {
    hints = {{Glyph::L1, "Move back"},
             {Glyph::R1, "Move forward"},
             {Glyph::Cross, "Save"},
             {Glyph::Circle, "Cancel"}};
  } else if (shownTab_ == Tab::Library) {
    hints = {{Glyph::Triangle, "Refresh"},
             {Glyph::Circle, ctl_.markedCount() ? "Clear selection" : "Exit"}};
  } else if (shownTab_ == Tab::Downloads) {
    if (!NetworkService::instance().tasks().empty())
      hints = {{Glyph::Cross, "Pause / Resume"}, {Glyph::Square, "Stop"}};
    hints.push_back({Glyph::Triangle, NetworkService::instance().running()
                                          ? "Stop server"
                                          : "Start server"});
    hints.push_back({Glyph::Circle, "Exit"});
  } else if (shownTab_ == Tab::Ntfs) {
    auto &ntfs = NtfsManager::instance();
    if (ntfsPickerShown_) {
      hints = {{Glyph::Cross, "Mount"},
               {Glyph::Triangle, "Rescan"},
               {Glyph::Circle, "Close"}};
    } else if (!ntfs.mounted()) {
      hints = {{Glyph::Cross, "Pick drive"}, {Glyph::Circle, "Exit"}};
    } else {
      hints = {{Glyph::Cross, "Open / Install"},
               {Glyph::Square, "Change drive"},
               {Glyph::Triangle, "Refresh"},
               {Glyph::Circle, ntfs.currentDir() == "/" ? "Exit" : "Back"}};
    }
  } else if (shownTab_ == Tab::Plugins && catalogExpanded_) {
    hints = {{Glyph::Cross, "Install"},
             {Glyph::Triangle, "Refresh catalog"},
             {Glyph::Circle, "Back"}};
  } else {
    hints = {{Glyph::Cross, "Select"}, {Glyph::Circle, "Exit"}};
  }
  text(dl, M, 1032.0f - 21.0f * 0.49f, 21.0f, Theme::textDim(),
       "github.com/laroping");
  drawHintRow(dl, SW - M, 1032.0f, hints);
}

void UiRoot::drawOptions(ImDrawList *dl, float dt) {
  float e = Anim::easeOutCubic(overlayAnim_);
  const float w = 600.0f;
  Theme::FillRoundRect(dl, ImVec2(0, 0), ImVec2(SW, SH),
                       IM_COL32(0, 4, 14, (int)(130 * e)));
  float x0 = SW - w * e;
  Theme::FillRoundRect(
      dl, ImVec2(x0, 0), ImVec2(SW, SH),
      IM_COL32(10, 18, 38, (int)(242 * std::min(1.0f, e * 1.5f))));
  dl->AddLine(ImVec2(x0, 0), ImVec2(x0, SH),
              IM_COL32(160, 200, 255, (int)(45 * e)), 1.5f);

  float x = x0 + 56.0f;
  text(dl, x, 96.0f, 17.0f, Theme::textDim(), "OPTIONS");
  text(dl, x, 124.0f, 34.0f, Theme::textPrimary(),
       ellipsize(optionsTitle_, 34.0f, w - 112.0f));
  dl->AddLine(ImVec2(x, 190.0f), ImVec2(x0 + w - 56.0f, 190.0f),
              IM_COL32(255, 255, 255, 26), 1.0f);

  // The list scrolls with the cursor so long lists (hidden tiles) stay usable.
  const float top = 220.0f, rowH = 96.0f, viewBot = SH - 90.0f,
              viewH = viewBot - top;
  const int n = (int)options_.size();
  float rowTop = optionsCursor_ * rowH;
  if (rowTop < optionsScrollTarget_)
    optionsScrollTarget_ = rowTop;
  if (rowTop + rowH > optionsScrollTarget_ + viewH)
    optionsScrollTarget_ = rowTop + rowH - viewH;
  optionsScrollTarget_ = std::max(
      0.0f, std::min(optionsScrollTarget_, std::max(0.0f, n * rowH - viewH)));
  float scroll = optionsScroll_.step(optionsScrollTarget_, dt, 5.0f, 1.0f);

  dl->PushClipRect(ImVec2(x0, top - 8.0f), ImVec2(SW, viewBot + 8.0f), true);
  float hy = optionsHighlightY_.step(rowTop, dt, 6.0f, 0.85f) + top - scroll;
  Theme::RowHighlight(dl, ImVec2(x - 24.0f, hy),
                      ImVec2(x0 + w - 44.0f, hy + rowH - 12.0f), 1.0f);
  int first = std::max(0, (int)(scroll / rowH) - 1),
      last = std::min(n - 1, (int)((scroll + viewH) / rowH) + 1);
  for (int i = first; i <= last; ++i) {
    float y = top + i * rowH - scroll;
    bool f = i == optionsCursor_;
    const Entry &o = options_[i];
    ImU32 c = !o.enabled
                  ? Theme::textDim()
                  : (o.danger && !f ? Theme::danger() : Theme::textPrimary());
    float boxW = o.check >= 0 ? 44.0f : 0.0f;
    if (o.check >= 0) {
      ImVec2 ba(x, y + 22.0f), bb(x + 28.0f, y + 50.0f);
      if (o.check)
        Theme::FillRoundRect(dl, ba, bb, Theme::accentBright(), 6.0f);
      dl->AddRect(ba, bb, o.check ? Theme::accentBright() : Theme::textMuted(),
                  6.0f, 0, 2.0f);
      if (o.check) {
        dl->AddLine(ImVec2(ba.x + 6.0f, ba.y + 14.0f),
                    ImVec2(ba.x + 12.0f, ba.y + 20.0f), Theme::white(), 3.0f);
        dl->AddLine(ImVec2(ba.x + 12.0f, ba.y + 20.0f),
                    ImVec2(ba.x + 22.0f, ba.y + 8.0f), Theme::white(), 3.0f);
      }
    }
    text(dl, x + boxW, y + 14.0f, 29.0f, c,
         ellipsize(o.label, 29.0f, w - 140.0f - boxW));
    text(dl, x + boxW, y + 52.0f, 20.0f,
         o.enabled ? Theme::textMuted() : Theme::textDim(),
         ellipsize(o.detail, 20.0f, w - 140.0f - boxW));
  }
  dl->PopClipRect();

  if (n * rowH > viewH) { // scroll indicator
    float tx = SW - 22.0f, h = viewH * viewH / (n * rowH);
    float ty = top + (viewH - h) * Anim::clamp01(scroll / (n * rowH - viewH));
    Theme::FillRoundRect(dl, ImVec2(tx, top), ImVec2(tx + 4, viewBot),
                         IM_COL32(255, 255, 255, 18), 2.0f);
    Theme::FillRoundRect(dl, ImVec2(tx, ty), ImVec2(tx + 4, ty + h),
                         IM_COL32(255, 255, 255, 120), 2.0f);
  }
}

void UiRoot::drawConfirm(ImDrawList *dl, float dt) {
  (void)dt;
  float e = Anim::easeOutCubic(overlayAnim_);
  Theme::FillRoundRect(dl, ImVec2(0, 0), ImVec2(SW, SH),
                       IM_COL32(0, 4, 14, (int)(160 * e)));

  Anim::VtxScope scope(dl);
  std::string title = confirmText_, body;
  size_t nl = confirmText_.find('\n');
  if (nl != std::string::npos) {
    title = confirmText_.substr(0, nl);
    body = confirmText_.substr(nl + 1);
  }
  const bool manualInfo = title == "Manual update installation";
  const float w = manualInfo ? 1040.0f : 880.0f;
  std::vector<std::string> bodyLines;
  if (manualInfo) {
    size_t start = 0;
    while (start <= body.size()) {
      size_t end = body.find('\n', start);
      const auto paragraph = body.substr(start, end == std::string::npos ?
                                                std::string::npos : end - start);
      if (paragraph.empty()) bodyLines.push_back("");
      else {
        auto lines = wrap(paragraph, 24.0f, w - 112.0f, 16);
        bodyLines.insert(bodyLines.end(), lines.begin(), lines.end());
      }
      if (end == std::string::npos) break;
      start = end + 1;
    }
  } else {
    bodyLines = wrap(body, 24.0f, w - 112.0f, 3);
  }
  const float h = manualInfo ? 240.0f + 33.0f * bodyLines.size() : 330.0f;
  ImVec2 a((SW - w) * 0.5f, (SH - h) * 0.5f), b(a.x + w, a.y + h);
  Theme::FillRoundRect(dl, a, b, IM_COL32(16, 28, 54, 246), 26.0f);
  dl->AddRect(a, b, Theme::glassEdge(), 26.0f, 0, 1.5f);

  float y = a.y + 52.0f;
  for (auto &l : wrap(title, 36.0f, w - 112.0f, 2)) {
    text(dl, a.x + 56.0f, y, 36.0f, Theme::textPrimary(), l);
    y += 46.0f;
  }
  y += 6.0f;
  for (auto &l : bodyLines) {
    text(dl, a.x + 56.0f, y, 24.0f, Theme::textMuted(), l);
    y += 33.0f;
  }

  const float bh = 62.0f, by = b.y - 52.0f - bh;
  // The player confirms with the gamepad glyph, so neither button starts
  // visually selected (no "primary" white fill); both are drawn as outline
  // chips with the glyph that triggers them.
  auto button = [&](float right, const char *label, Glyph g) {
    float tw = Theme::Measure(25.0f, label).x, bw = tw + 40.0f + 32.0f + 12.0f;
    ImVec2 p0(right - bw, by), p1(right, by + bh);
    dl->AddRect(p0, p1, IM_COL32(255, 255, 255, 110), bh * 0.5f, 0, 2.0f);
    Theme::ButtonGlyph(dl, ImVec2(p0.x + 20.0f, by + (bh - 32.0f) * 0.5f),
                       32.0f, g);
    Theme::Text(dl, p0.x + 20.0f + 32.0f + 12.0f,
                by + (bh - 25.0f) * 0.5f + 0.5f, 25.0f, Theme::textPrimary(),
                label);
    return bw;
  };
  float right = b.x - 56.0f;
  if (!manualInfo)
    right -= button(right, "Cancel", Glyph::Circle) + 16.0f;
  button(right, manualInfo ? "Close" : "OK", Glyph::Cross);

  float s = 0.95f + 0.05f * e;
  scope.apply(e, ImVec2(0, 0), s, ImVec2(SW * 0.5f, SH * 0.5f));
}

// Console-style toast: one short headline ("New order saved"); errors get a
// second, muted line with the reason. Long explanations are dropped.
namespace {
void splitToast(const std::string &full, bool error, std::string &head,
                std::string &sub) {
  head = full;
  sub.clear();
  // Cut at the first sentence end or " - " explanation.
  size_t cut = std::string::npos;
  for (const char *sep : {". ", " - "}) {
    size_t p = head.find(sep);
    if (p != std::string::npos && p < cut)
      cut = p;
  }
  std::string rest;
  if (cut != std::string::npos) {
    rest = head.substr(cut + 2);
    head = head.substr(0, cut);
  }
  while (!head.empty() && (head.back() == '.' || head.back() == ' '))
    head.pop_back();
  if (error) {
    // "Saving failed: reason" -> "Saving failed" + "reason"
    size_t colon = head.find(": ");
    if (colon != std::string::npos && colon > 0) {
      sub = head.substr(colon + 2);
      head = head.substr(0, colon);
    } else
      sub = rest;
    while (!sub.empty() && (sub.back() == '.' || sub.back() == ' '))
      sub.pop_back();
  }
}
} // namespace

void UiRoot::drawToast(ImDrawList *dl, float dt) {
  if (ctl_.statusSerial() != toastSerial_) {
    toastSerial_ = ctl_.statusSerial();
    if (!ctl_.statusText().empty()) {
      toastText_ = ctl_.statusText();
      toastError_ = ctl_.statusIsError();
      toastTimer_ = toastError_ ? 4.5f : 2.6f;
    }
  }
  if (toastTimer_ > 0.0f) {
    toastTimer_ -= dt;
    Anim::activeFlag() = true;
  }
  Anim::approach(toastAnim_, toastTimer_ > 0.0f ? 1.0f : 0.0f, 11.0f, dt);
  if (toastAnim_ <= 0.01f)
    return;

  std::string head, sub;
  splitToast(toastText_, toastError_, head, sub);
  const float maxW = 560.0f, textX = 70.0f, padR = 28.0f;
  head = ellipsize(head, 23.0f, maxW - textX - padR);
  sub = ellipsize(sub, 18.0f, maxW - textX - padR);
  float tw = std::max(measure(23.0f, head).x,
                      sub.empty() ? 0.0f : measure(18.0f, sub).x);
  const float w = std::min(maxW, std::max(300.0f, textX + tw + padR));
  const float h = sub.empty() ? 64.0f : 84.0f;

  // Below the top bar, right aligned, so it never covers the clock.
  float e = Anim::easeOutCubic(toastAnim_);
  ImVec2 a(SW - M - w, kTopBarY + kTopBarH + 18.0f), b(a.x + w, a.y + h);
  Anim::VtxScope scope(dl);
  Theme::FillRoundRect(dl, ImVec2(a.x + 4, a.y + 8), ImVec2(b.x - 4, b.y + 10),
                       IM_COL32(0, 4, 14, 70), 18.0f);
  Theme::FillRoundRect(dl, a, b, IM_COL32(18, 32, 60, 250), 18.0f);
  dl->AddRect(a, b, Theme::glassEdge(), 18.0f, 0, 1.5f);
  ImU32 ic = toastError_ ? Theme::danger() : Theme::success();
  ImVec2 c(a.x + 36.0f, a.y + h * 0.5f);
  dl->AddCircleFilled(c, 17.0f, Theme::alpha(ic, 0.2f), 24);
  Theme::DrawIcon(dl, ImVec2(c.x - 10.0f, c.y - 10.0f), 20.0f,
                  toastError_ ? Icon::Alert : Icon::Check, ic);
  if (sub.empty()) {
    text(dl, a.x + textX, a.y + (h - measure(23.0f, "Ag").y) * 0.5f, 23.0f,
         Theme::textPrimary(), head);
  } else {
    text(dl, a.x + textX, a.y + 14.0f, 23.0f, Theme::textPrimary(), head);
    text(dl, a.x + textX, a.y + 46.0f, 18.0f, Theme::textMuted(), sub);
  }
  scope.apply(e, ImVec2((1.0f - e) * 50.0f, 0.0f));
}

void UiRoot::drawNtfs(ImDrawList *dl, float dt) {
  auto &ntfs = NtfsManager::instance();
  const float pad = 36.0f;

  if (ntfsPickerShown_) {
    ImVec2 pa(M, kContentTop), pb(SW - M, kContentBot);
    Theme::Glass(dl, pa, pb, 20.0f, 1.0f, false);
    float px = pa.x + pad;
    text(dl, px, pa.y + 24.0f, 32.0f, Theme::textPrimary(), "Select a drive");
    text(dl, px, pa.y + 68.0f, 20.0f, Theme::textMuted(),
         "Pick a USB drive or partition. Press Triangle to rescan.");

    int n = (int)ntfsPickerList_.size();
    float listTop = pa.y + 120.0f;
    const float rowH = 68.0f;

    if (n == 0) {
      text(dl, px, listTop + 20.0f, 22.0f, Theme::textMuted(),
           "No drives found. Plug in a USB drive and press Triangle.");
      return;
    }

    int cur = std::min(ntfsPickerCursor_, n - 1);
    int visible = std::max(1, (int)((pb.y - listTop - 20.0f) / rowH));
    int first = std::max(0, std::min(cur - visible / 2, n - visible));
    first = std::max(0, first);

    // Animated row highlight bar for the focused drive - spring glide
    // between rows instead of a hard jump.
    float targetY = listTop + (cur - first) * rowH;
    if (!ntfsPickerHighlightInit_) {
      ntfsPickerHighlightY_ = Anim::Spring(targetY);
      ntfsPickerHighlightInit_ = true;
    }
    float phy = ntfsPickerHighlightY_.step(targetY, dt, 6.0f, 0.85f);
    Theme::RowHighlight(dl, ImVec2(pa.x + 12.0f, phy),
                        ImVec2(pb.x - 12.0f, phy + rowH - 8.0f), 1.0f);

    for (int i = first; i < std::min(n, first + visible); ++i) {
      const auto &d = ntfsPickerList_[i];
      float ry = listTop + (i - first) * rowH;
      bool focused = (i == cur);

      Icon ic = d.shellMount ? Icon::Disk : Icon::Disk;
      ImU32 col = focused ? Theme::textPrimary() : Theme::textMuted();
      Theme::DrawIcon(dl, ImVec2(px, ry + 18.0f), 28.0f, ic,
                      d.shellMount ? Theme::accentBright() : Theme::success());

      text(dl, px + 48.0f, ry + 10.0f, 24.0f, col,
           ellipsize(d.label, 24.0f, pb.x - px - 80.0f));
      const char *sub =
          d.shellMount ? "Already mounted by PS4 (exFAT/FAT) - read-only access"
                       : "Raw NTFS via ntfs-3g - mount as needed";
      text(dl, px + 48.0f, ry + 36.0f, 18.0f, Theme::textDim(), sub);
    }
    return;
  }

  ImVec2 ha(M, kContentTop), hb(SW - M, kContentTop + 110.0f);
  Theme::Glass(dl, ha, hb);
  float hx = ha.x + pad;
  float dy = bandTextY(ha.y, 25.0f);

  if (!ntfs.mounted()) {
    Theme::DrawIcon(dl, ImVec2(hx, dy - 4.0f), 28.0f, Icon::Disk,
                    Theme::danger());
    text(dl, hx + 42.0f, dy, 25.0f, Theme::textPrimary(),
         "No USB drive detected");
    text(dl, hx, ha.y + 68.0f, 20.0f, Theme::textMuted(),
         "Press Triangle or Cross to pick a drive.");
  } else {
    Theme::DrawIcon(dl, ImVec2(hx, dy - 4.0f), 28.0f, Icon::Disk,
                    Theme::success());
    // Big label - "File Explorer - /dev/da1s1" so the device is visible
    // in the title bar and the file area below is clearly its contents.
    std::string header = "File Explorer   -   " + ntfs.devicePath();
    text(dl, hx + 42.0f, dy, 25.0f, Theme::textPrimary(), header);
    text(dl, hx, ha.y + 68.0f, 20.0f, Theme::textMuted(),
         "Current folder: " + ntfs.currentDir());
  }

  ImVec2 la(M, kContentTop + 130.0f), lb(SW - M, kContentBot);
  Theme::Glass(dl, la, lb, 20.0f, 1.0f, false);

  if (!ntfs.mounted())
    return;

  const auto &es = ntfs.entries();
  const int n = (int)es.size();
  const float rowH = 52.0f;
  const float top = la.y + pad;

  if (n == 0) {
    text(dl, la.x + pad, top, 22.0f, Theme::textMuted(), "(empty folder)");
    return;
  }
  int visible = std::max(1, (int)((lb.y - top - 20.0f) / rowH));
  int cur = std::min(ntfsCursor_, n - 1);
  int first = std::max(0, std::min(cur - visible / 2, n - visible));
  first = std::max(0, first);

  float hy = ntfsHighlightY_.step(top + (cur - first) * rowH, dt, 6.0f, 0.85f);
  Theme::RowHighlight(dl, ImVec2(la.x + 12.0f, hy),
                      ImVec2(lb.x - 12.0f, hy + rowH - 6.0f), 1.0f);

  // Baseline shared by name, size and INSTALL pill so they sit on one line.
  const float iconS = 26.0f;
  const float nameS = 24.0f;
  const float sizeS = 20.0f;

  for (int i = first; i < std::min(n, first + visible); ++i) {
    const NtfsEntry &e = es[i];
    float ry = top + (i - first) * rowH;
    float rowMidY = ry + rowH * 0.5f;
    float ix = la.x + 26.0f;

    Icon ic = e.isDir ? Icon::Folder : Icon::Download;
    ImU32 iconCol = e.isDir ? Theme::accentBright() : Theme::textPrimary();
    std::string lower = e.name;
    for (auto &c : lower)
      c = (char)std::tolower((unsigned char)c);
    bool isPkg = !e.isDir && lower.size() >= 4 &&
                 lower.substr(lower.size() - 4) == ".pkg";
    if (isPkg)
      iconCol = Theme::success();

    Theme::DrawIcon(dl, ImVec2(ix, rowMidY - iconS * 0.5f), iconS, ic, iconCol);

    ImU32 nameCol = (i == cur) ? Theme::textPrimary() : Theme::textMuted();
    float nameY = rowMidY - measure(nameS, "Ag").y * 0.5f;
    text(dl, ix + 42.0f, nameY, nameS, nameCol,
         ellipsize(e.name, nameS, lb.x - (ix + 42.0f) - 220.0f));

    float sizeY = rowMidY - measure(sizeS, "Ag").y * 0.5f;
    if (!e.isDir) {
      textRight(dl, lb.x - 26.0f, sizeY, sizeS, Theme::textMuted(),
                formatBytes(e.size));
    } else {
      textRight(dl, lb.x - 26.0f, sizeY, sizeS, Theme::textDim(), "folder");
    }
    bool installable = isPkg && !ntfs.isShellMount();
    if (isPkg && i == cur) {
      // Theme::Pill renders at height = textSize + 12 (= 30 for size=18).
      // RowHighlight runs from hy to hy + rowH - 6.0f (center is rowMidY
      // - 3.0f). Centre the pill vertically inside the highlight box.
      const float pillTextSize = 18.0f;
      const float pillH = pillTextSize + 12.0f;
      float bx = lb.x - 26.0f - measure(sizeS, formatBytes(e.size)).x - 110.0f;
      float pillY = rowMidY - 3.0f - pillH * 0.5f;
      if (installable) {
        Theme::Pill(dl, bx, pillY, pillTextSize, "INSTALL",
                    Theme::alpha(Theme::success(), 0.3f), Theme::textPrimary());
      } else {
        // exFAT/FAT mount - installs blocked. Show why on the row.
        Theme::Pill(dl, bx, pillY, pillTextSize, "exFAT",
                    Theme::alpha(Theme::danger(), 0.3f), Theme::textMuted());
      }
    }
  }

  (void)ntfsScroll_;
  (void)ntfsScrollTarget_;
  (void)ntfsHighlightInit_;

  // Install progress overlay (anchored to the bottom of the file list).
  auto st = NtfsInstall::instance().status();
  // Hide finished / failed states after a short grace so the overlay
  // doesn't linger. The grace timer runs off real time via the static tick.
  static float finishedSince = 0.0f;
  static NtfsInstall::Phase lastPhase = NtfsInstall::Phase::Idle;
  if (st.phase != lastPhase) {
    lastPhase = st.phase;
    finishedSince = (st.phase == NtfsInstall::Phase::Done ||
                     st.phase == NtfsInstall::Phase::Failed)
                        ? time_
                        : 0.0f;
  }
  bool hide = (st.phase == NtfsInstall::Phase::Done ||
               st.phase == NtfsInstall::Phase::Failed) &&
              finishedSince > 0.0f && (time_ - finishedSince) > 3.0f;
  if (st.phase != NtfsInstall::Phase::Idle && !hide) {
    Anim::activeFlag() =
        true; // keep the frame loop spinning so the bar updates

    const float oH = 110.0f;
    ImVec2 oa(la.x + 16.0f, lb.y - oH - 16.0f);
    ImVec2 ob(lb.x - 16.0f, lb.y - 16.0f);
    Theme::FillRoundRect(dl, oa, ob, IM_COL32(16, 28, 54, 235), 18.0f);
    dl->AddRect(oa, ob, Theme::glassEdge(), 18.0f, 0, 1.5f);

    const char *title = "";
    ImU32 accent = Theme::accentBright();
    switch (st.phase) {
    case NtfsInstall::Phase::Copying:
      title = "Copying from USB";
      accent = Theme::accentBright();
      break;
    case NtfsInstall::Phase::Installing:
      title = "Installing";
      accent = Theme::accentBright();
      break;
    case NtfsInstall::Phase::Done:
      title = "Install started";
      accent = Theme::success();
      break;
    case NtfsInstall::Phase::Failed:
      title = "Install failed";
      accent = Theme::danger();
      break;
    default:
      break;
    }

    text(dl, oa.x + 24.0f, oa.y + 14.0f, 22.0f, Theme::textPrimary(), title);
    text(dl, oa.x + 24.0f, oa.y + 44.0f, 19.0f, Theme::textMuted(),
         ellipsize(st.fileName.empty() ? st.message : st.fileName, 19.0f,
                   ob.x - oa.x - 48.0f));

    float pct = 0.0f;
    if (st.phase == NtfsInstall::Phase::Copying && st.totalBytes > 0)
      pct = (float)((double)st.bytesCopied / (double)st.totalBytes);
    else if (st.phase == NtfsInstall::Phase::Installing)
      pct = 1.0f;
    else if (st.phase == NtfsInstall::Phase::Done ||
             st.phase == NtfsInstall::Phase::Failed)
      pct = 1.0f;
    pct = std::max(0.0f, std::min(1.0f, pct));

    ImVec2 ba(oa.x + 24.0f, ob.y - 30.0f);
    ImVec2 bb(ob.x - 24.0f, ob.y - 22.0f);
    Theme::FillRoundRect(dl, ba, bb, IM_COL32(255, 255, 255, 28), 4.0f);
    if (pct > 0.0f) {
      Theme::FillRoundRect(dl, ba, ImVec2(ba.x + (bb.x - ba.x) * pct, bb.y),
                           accent, 4.0f);
    }

    char rb[64];
    if (st.phase == NtfsInstall::Phase::Copying && st.totalBytes > 0) {
      std::snprintf(rb, sizeof(rb), "%d%%   %s / %s", (int)(pct * 100.0f),
                    formatBytes(st.bytesCopied).c_str(),
                    formatBytes(st.totalBytes).c_str());
    } else if (st.phase == NtfsInstall::Phase::Installing) {
      std::snprintf(rb, sizeof(rb), "Handing off to BGFT...");
    } else {
      std::snprintf(rb, sizeof(rb), "%s", st.message.c_str());
    }
    textRight(dl, ob.x - 24.0f, oa.y + 14.0f, 18.0f, Theme::textMuted(), rb);
  }
}
