#pragma once
#include <pthread.h>

#include <atomic>
#include <deque>
#include <map>
#include <string>

#include "imgui_soft_renderer.h"

enum class IconSize { Tile = 0, Hero = 1, Small = 2 };

class IconCache {
public:
  static constexpr int kTilePx = 168;
  static constexpr int kHeroPx = 232;
  static constexpr int kSmallPx = 72;

  static IconCache &instance();

  void start();
  void stop();

  // Non-blocking: returns the texture when ready, otherwise queues it
  // (most recent requests first) and returns 0.
  ImTextureID get(const std::string &titleId, IconSize size);
  ImTextureID getFile(const std::string &path, IconSize size);

  bool busy();

private:
  struct Entry {
    enum State { Queued, Ready, Missing } state = Queued;
    SoftRender::Texture tex;
    uint32_t lastUse = 0;
  };
  using Key = std::pair<std::string, int>;

  IconCache() = default;
  static void *threadEntry(void *self);
  void worker();
  bool load(const std::string &titleId, int px, SoftRender::Texture &out);
  void evictHeroes();

  pthread_t thread_{};
  pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
  pthread_cond_t cond_ = PTHREAD_COND_INITIALIZER;
  std::atomic<bool> running_{false};
  bool started_ = false;
  std::map<Key, Entry> entries_;
  std::deque<Key> queue_;
  uint32_t clock_ = 0;
};
