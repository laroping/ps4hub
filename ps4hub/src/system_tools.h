#pragma once
#include <pthread.h>

#include <atomic>
#include <set>
#include <string>
#include <vector>

#include "port_owner.h"

namespace UpdateBlocker {
enum class State { Off, On, Partial };
State state();
bool enable(std::string &err);
bool disable(std::string &err);
} // namespace UpdateBlocker

struct ServiceInfo {
  std::string name;
  int port = 0;
  bool listening = false;
  bool ours = false;
  bool stoppable = false;
  PortOwnerInfo owner;
};

namespace Services {
std::vector<ServiceInfo> scan(); // quick: local connect() probes + owner lookup
bool stop(const ServiceInfo &s, std::string &err);
} // namespace Services

class FpkgRebuilder {
public:
  enum class State { Idle, Running, Done };
  static FpkgRebuilder &instance();

  // `known` = every title id already in app.db.
  void start(const std::set<std::string> &known);
  State state() const { return state_.load(); }
  unsigned serial() const { return serial_.load(); }
  std::string summary();
  int restoredCount();

  static std::vector<std::string>
  findMissing(const std::set<std::string> &known);

private:
  FpkgRebuilder() = default;
  static void *entry(void *self);

  pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
  std::atomic<State> state_{State::Idle};
  std::atomic<unsigned> serial_{0};
  std::set<std::string> known_;
  std::string summary_;
  int restored_ = 0;
};
