#pragma once
#include <mutex>
#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <string>

class NtfsInstall {
public:
  enum class Phase { Idle, Copying, Installing, Done, Failed };

  static NtfsInstall &instance();

  bool start(const std::string &ntfsSourcePath, const std::string &fileName,
             int userId);

  struct Status {
    Phase phase = Phase::Idle;
    std::string fileName;
    uint64_t bytesCopied = 0;
    uint64_t totalBytes = 0;
    std::string message; // last toast / result
  };

  Status status() const;
  bool busy() const;

private:
  NtfsInstall() = default;

  static void *entry(void *self);
  void run(std::string src, std::string name, int userId);

  mutable std::mutex mutex_;
  std::atomic<bool> running_{false};
  pthread_t thread_{};

  Status st_;
};
