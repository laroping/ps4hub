#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

enum class DefaultTiles { Unknown, Missing, Visible, Hidden, Mixed };

struct CatalogEntry {
  std::string titleId; // "CUSA12345", "NPXS20001", "folder_xxx", "?folder_xxx"
  std::string titleName;
  std::string lastLaunch; // "YYYY-MM-DD hh:mm:ss.mmm"
  int uiPos = 0;
  bool visible = true;
  bool isFolder = false;
  int folderFlag = 0; // folder type column, when the schema has one
  std::string
      category; // "gd" app, "ac" add-on, "gp" patch... (empty if no column)
};

class CatalogDb {
public:
  explicit CatalogDb(std::string dbPath);

  // Resolves the browse table (tbl_appbrowse or tbl_appbrowse_<userId>) and
  // its time / position columns. Must succeed before any other call.
  bool open(int userId);
  bool isReady() const { return !table_.empty(); }
  const std::string &tableName() const { return table_; }
  const std::string &lastError() const { return error_; }

  bool backup(const std::string &backupPath);
  bool restore(const std::string &backupPath);

  std::vector<CatalogEntry> fetchEntries();
  // Diagnostics: every row whose titleId is not a plain title id, all
  // columns, tab separated (helps identify folder rows on new firmwares).
  bool dumpNonTitleRows(const std::string &path);

  // 3. Reorder, in one transaction: `pinned` gets 2099-12-31 23:59:00.000
  //    counting down one minute per item and ui_pos 1, 2, 3...; `sendToEnd`
  //    gets 2000-01-01 00:00:00.000 counting up and ui_pos 990, 991...
  bool commitOrder(const std::vector<std::string> &pinned,
                   const std::vector<std::string> &sendToEnd);

  // 4. Default tiles NPXS29999 / NPXS29998: real state read from the table,
  //    hide = SET visible = 0, canRemove = 1; show = SET visible = 1, canRemove
  //    = 0.
  DefaultTiles defaultTilesState();
  bool setDefaultTilesVisible(bool visible);

  bool purgeApplication(const std::string &titleId);

  // Sets only the launch timestamp of the given titles (one transaction);
  // used to slot a moved tile between its new neighbours.
  bool setLaunchTimes(
      const std::vector<std::pair<std::string, std::string>> &idAndTime);

  struct RowState {
    std::string id, time;
    int uiPos;
    bool visible;
  };
  bool restoreRows(const std::vector<RowState> &rows);

  // Full sequence commit (one transaction): every row gets its position
  // and launch time, e.g. ui_pos 1, 2, 3... with times counting down.
  struct SeqRow {
    std::string id, time;
    int uiPos;
  };
  bool commitSequence(const std::vector<SeqRow> &rows);

  // Best known real "last played" time per title, from any date column
  // that still holds a genuine value (PS4Hub's 2099 / 2000 marks skipped).
  std::map<std::string, std::string> realTimes();
  static bool isMarkerTime(const std::string &t);

  std::vector<std::string> allTitleIds();

  std::vector<RowState> readLayout(const std::string &dbFile);

  bool setVisible(const std::vector<std::string> &ids, bool visible);

  bool deleteRows(const std::vector<std::string> &ids);

  const std::vector<std::string> &tables() const { return tables_; }

  // Strict "ABCD12345" check; used for anything that touches the filesystem.
  static bool isPlainTitleId(const std::string &id);

private:
  struct Handle;
  bool exec(Handle &h, const char *sql);
  bool hasColumn(Handle &h, const std::string &column);

  std::string dbPath_;
  std::string table_;
  std::string timeCol_;
  std::string posCol_;
  std::string visibleCol_;
  std::string nameCol_;
  std::string categoryCol_;
  std::string folderCol_;
  std::string time2Col_; // second time column written alongside timeCol_
  bool loadColumns(Handle &h);
  std::string timeAssign() const;
  bool mirroring_ = false;
  template <typename F> bool mirrored(F f);
  std::string error_;
  std::vector<std::string> tables_;
};
