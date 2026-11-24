#pragma once
// NetworkService - remote package install server, wire-compatible with
// flatz's Remote Package Installer (and RPI NextGen's legacy API), so the
// usual PC/phone sender tools work unchanged.
//
// Legacy API (POST, JSON):
//   /api/install            {"type":"direct","packages":[url,...]} |
//   {"type":"ref_pkg_url","url":...} /api/get_task_progress  {"task_id":N}
//   /api/start_task | stop_task | pause_task | resume_task | unregister_task
//   {"task_id":N} /api/find_task          {"content_id":...,"sub_type":N}
//   /api/is_exists          {"title_id":...}
//   /api/uninstall_game | uninstall_patch {"title_id":...}
//   /api/uninstall_ac | uninstall_theme   {"content_id":...}
// Extras: POST /api/v2/install {"url"|"urls"|"ref_pkg_url"}, GET / and
// /api/v2/system, GET /static/<file> (manifests for BGFT), OPTIONS (CORS).
//
// Each connection is served on its own thread: BGFT fetches the manifest
// from this server while a registration is still in progress.
// Only LAN (RFC 1918 / loopback) clients are accepted.
#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct InstallTask {
  int taskId = -1;
  std::string title;
  std::string contentId;
  std::string iconPath;
  uint64_t length = 0;
  uint64_t transferred = 0;
  uint32_t restSec = 0;
  int32_t error = 0;
  bool paused = false;
  bool stopped = false;
  bool complete = false;
  bool announced = false;
};

class NetworkService {
public:
  static NetworkService &instance();

  bool start(int port, int userId);
  void stop();
  bool running() const { return running_.load(); }
  int port() const { return port_; }

  std::vector<std::string> log();
  std::vector<InstallTask> tasks();
  std::string lastError();

  // Polls BGFT progress for known tasks (UI thread, ~1x/sec).
  void refreshProgress();

  bool togglePause(int taskId);
  bool stopTask(int taskId);

  void handleClient(int fd, const std::string &peer);

  // Visible to other modules so that one-off diagnostics can surface in
  // the on-screen ACTIVITY panel without plumbing a dedicated log sink.
  void debugLog(const std::string &line) { addLog(line); }

private:
  NetworkService() = default;
  static void *acceptEntry(void *self);
  void serve();

  std::string route(const std::string &method, const std::string &path,
                    const std::string &body, int &code);
  std::string apiInstall(const std::string &body, bool v2, int &code);
  std::string apiProgress(const std::string &body, int &code);
  std::string apiTaskControl(const std::string &path, const std::string &body,
                             int &code);
  std::string apiFindTask(const std::string &body, int &code);
  std::string apiIsExists(const std::string &body, int &code);
  std::string apiUninstall(const std::string &path, const std::string &body,
                           int &code);
  std::string apiSystem();
  bool serveStatic(int fd, const std::string &path,
                   const std::string &headLower, bool headOnly = false);
  bool serveNtfsStream(int fd, const std::string &query,
                       const std::string &headLower, bool headOnly = false);
  bool serveAutoloader(int fd, const std::string &path);

  void addLog(const std::string &line);
  void setError(const std::string &e);
  void rememberTask(const InstallTask &t);

  pthread_t thread_{};
  pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
  std::atomic<bool> running_{false};
  std::atomic<int> activeClients_{0};
  bool threadStarted_ = false;
  int port_ = 0;
  int userId_ = -1;
  int listenFd_ = -1;

  std::deque<std::string> log_;
  std::vector<InstallTask> tasks_;
  std::string lastError_;
};
