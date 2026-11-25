#pragma once
#include "webkit_browser.h"
#include "yt_adblock.h"
// UiRoot - all screen drawing and pad-to-action mapping. Reads state from
// AppController / NetworkService / Platform and calls their actions; owns only
// presentation state (animations, scroll, menus, dialogs, toasts).
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "anim.h"
#include "app_controller.h"
#include "gamepad.h"
#include "ntfs_manager.h"
#include "platform.h"

struct ImDrawList;

class UiRoot {
public:
  explicit UiRoot(AppController &ctl);

  bool handleInput(const Gamepad &pad);

  bool draw(float dt);

  void setStorage(const Platform::Storage &s) { storage_ = s; }
  void setLocalIp(const std::string &ip) { localIp_ = ip; }
  void setStats(const Platform::SystemStats &s) { stats_ = s; }

private:
  enum class Overlay { None, Options, Confirm, Picker };
  struct Entry {
    std::string label;
    std::string detail;
    std::function<void()> action;
    bool danger = false;
    bool enabled = true;
    std::string confirm; // non-empty: ask before running ("Title\nBody")
    bool stay = false;
    int check = -1; // -1 no box, 0 empty box, 1 ticked box
    std::string category;
  };

  void openOptions();
  void openHiddenList();
  void openProfilesList();
  void openYoutubeList(bool keepPosition);
  void openPatchGames();
  void openNetworkList(bool keepPosition);
  void openNetworkActions(const std::string &file);
  void openGamePatches(std::string id, std::string name, bool keepPosition);
  void setList(const std::string &title, bool keepPosition);
  std::vector<Entry> pluginEntries();
  void drawPlugins(ImDrawList *dl, float dt);
  void drawCatalogScreen(ImDrawList *dl, float dt);
  void drawEntryList(ImDrawList *dl, float dt,
                     const std::vector<Entry> &entries, const char *heading,
                     int &cursor, Anim::Spring &scroll, float &scrollTarget,
                     Anim::Spring &highlight, bool &highlightInit);
  void openAddonsList(std::string id, std::string name);
  bool catalogExpanded_ = false;
  float catalogExpandAnim_ =
      0.0f; // 0 = plugins list visible, 1 = catalog screen visible
  int catalogCursor_ = 0;
  Anim::Spring catalogHighlightY_, catalogScroll_;
  bool catalogHighlightInit_ = false;
  float catalogScrollTarget_ = 0.0f;
  void openProfileActions(int id, std::string name);
  void showList(const std::string &title);
  void openConfirm(const std::string &text, std::function<void()> action);
  void closeOverlay();
  void runEntry(const Entry &e);
  std::vector<Entry> systemEntries() const;
  void gridMove(int dx, int dy);

  void drawTopBar(ImDrawList *dl, float dt);
  void drawPageTitle(ImDrawList *dl);
  void drawLibrary(ImDrawList *dl, float dt);
  void drawTile(ImDrawList *dl, const AppItem &it, ImVec2 p, float focus,
                float lift, float alpha);
  void drawDetail(ImDrawList *dl, float dt);
  void drawDownloads(ImDrawList *dl, float dt);
  void drawSystem(ImDrawList *dl, float dt);
  void drawNtfs(ImDrawList *dl, float dt);
  void drawHints(ImDrawList *dl);
  void drawOptions(ImDrawList *dl, float dt);
  void drawConfirm(ImDrawList *dl, float dt);
  void drawPicker(ImDrawList *dl, float dt);
  void openPositionPicker();
  void openServicesList();
  void refreshSystemCaches();
  void drawToast(ImDrawList *dl, float dt);

  AppController &ctl_;
  float time_ = 0.0f;

  Overlay overlay_ = Overlay::None;
  bool overlayClosing_ = false;
  float overlayAnim_ = 0.0f;
  std::vector<Entry> options_;
  std::string optionsTitle_;
  int optionsCursor_ = 0;
  Anim::Spring optionsHighlightY_;
  Anim::Spring optionsScroll_;
  float optionsScrollTarget_ = 0.0f;
  std::string confirmText_;
  std::function<void()> confirmAction_;

  Tab shownTab_ = Tab::Library;
  float pageAnim_ = 1.0f;
  int pageDir_ = 1;
  Anim::Spring tabPillX_, tabPillW_;
  bool tabPillInit_ = false;
  int systemCursor_ = 0;
  int pluginsCursor_ = 0;
  Anim::Spring pluginsHighlightY_, pluginsScroll_;
  bool pluginsHighlightInit_ = false;
  float pluginsScrollTarget_ = 0.0f;
  int jobSerial_ = 0;
  YtAdBlock::Settings yt_;
  // Plugins tab status, read from disk at most once a second (not per frame).
  struct PluginStatus {
    bool ytPlugin = false, ytIni = false, share = false, patch = false;
    std::string patchDb;
    size_t netProfiles = 0;
    bool autoloader = false;
    bool webkitInstalled = false;
    bool webkitBackup = false;
    std::string webkitTitleId;
  } plug_;
  float plugStamp_ = -10.0f;
  const PluginStatus &pluginStatus();
  void invalidatePluginStatus() { plugStamp_ = -10.0f; }
  Anim::Spring systemHighlightY_;
  bool systemHighlightInit_ = false;
  int downloadsCursor_ = 0;
  Anim::Spring downloadsHighlightY_;

  int ntfsCursor_ = 0;
  Anim::Spring ntfsHighlightY_, ntfsScroll_;
  bool ntfsHighlightInit_ = false;
  float ntfsScrollTarget_ = 0.0f;
  bool ntfsMountTried_ = false;

  // Drive picker shown when more than one device is available, or when
  // nothing is mounted and the user wants to pick one.
  bool ntfsPickerShown_ = false;
  int ntfsPickerCursor_ = 0;
  std::vector<NtfsDeviceInfo> ntfsPickerList_;
  Anim::Spring ntfsPickerHighlightY_;
  bool ntfsPickerHighlightInit_ = false;

  Anim::Spring scroll_;
  float scrollTarget_ = 0.0f;
  Anim::Bank<Anim::Spring2> tilePos_;
  Anim::Bank<Anim::Spring> tileFocus_;
  Anim::Bank<float> tileFade_;
  Anim::Bank<float> tileMark_;
  std::map<std::string, std::string> labelCache_;
  std::string detailId_;
  float detailAnim_ = 1.0f;
  float storageFill_ = 0.0f;
  Anim::Bank<float> taskProgress_;

  unsigned toastSerial_ = 0;
  std::string toastText_;
  bool toastError_ = false;
  float toastAnim_ = 0.0f;
  float toastTimer_ = 0.0f;

  float intro_ = 0.0f;
  bool quitRequested_ = false;
  float outro_ = 0.0f;

  Platform::Storage storage_;
  Platform::SystemStats stats_;

  int pickerValue_ = 1, pickerMax_ = 1;
  std::string pickerTitle_;

  Anim::Spring systemScroll_;
  float systemScrollTarget_ = 0.0f;
  int fpkgMissing_ = -1;
  int layoutProfiles_ = 0;
  int blockerState_ = -1; // UpdateBlocker::State as int, -1 unknown
  unsigned rebuildSerial_ = 0;
  unsigned updaterSerial_ = 0;
  std::string localIp_;
};
