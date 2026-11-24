#include "icon_cache.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>

#include "catalog_db.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include <stb/stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image_resize.h>
#pragma clang diagnostic pop

namespace {

struct Lock {
  pthread_mutex_t *m;
  explicit Lock(pthread_mutex_t *mm) : m(mm) { pthread_mutex_lock(m); }
  ~Lock() { pthread_mutex_unlock(m); }
};

constexpr size_t kMaxTiles = 512;
constexpr size_t kMaxHeroes = 6;

// Bakes a rounded-rectangle mask (anti-aliased) into the alpha channel.
void roundCorners(uint8_t *px, int size, float radius) {
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      float cx = x + 0.5f, cy = y + 0.5f;
      float dx = 0.0f, dy = 0.0f;
      if (cx < radius)
        dx = radius - cx;
      else if (cx > size - radius)
        dx = cx - (size - radius);
      if (cy < radius)
        dy = radius - cy;
      else if (cy > size - radius)
        dy = cy - (size - radius);
      if (dx == 0.0f || dy == 0.0f)
        continue;
      float dist = std::sqrt(dx * dx + dy * dy) - radius; // >0 outside
      float cover = std::fmin(1.0f, std::fmax(0.0f, 0.5f - dist));
      uint8_t &a = px[(y * size + x) * 4 + 3];
      a = (uint8_t)(a * cover);
    }
  }
}

} // namespace

IconCache &IconCache::instance() {
  static IconCache c;
  return c;
}

void IconCache::start() {
  if (started_)
    return;
  running_ = true;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 1024 * 1024); // PNG inflate + resize
  started_ =
      pthread_create(&thread_, &attr, &IconCache::threadEntry, this) == 0;
  pthread_attr_destroy(&attr);
  if (!started_)
    running_ = false;
}

void IconCache::stop() {
  if (!started_)
    return;
  {
    Lock l(&mutex_);
    running_ = false;
    pthread_cond_signal(&cond_);
  }
  pthread_join(thread_, nullptr);
  started_ = false;
}

bool IconCache::busy() {
  Lock l(&mutex_);
  return !queue_.empty();
}

ImTextureID IconCache::get(const std::string &titleId, IconSize size) {
  if (titleId.empty() ||
      (titleId[0] != '/' && !CatalogDb::isPlainTitleId(titleId)))
    return 0;
  Key key(titleId, (int)size);
  Lock l(&mutex_);
  ++clock_;
  auto it = entries_.find(key);
  if (it != entries_.end()) {
    it->second.lastUse = clock_;
    if (it->second.state == Entry::Ready)
      return SoftRender::TextureId(&it->second.tex);
    if (it->second.state == Entry::Queued) {
      // Re-prioritise: whatever is on screen now goes first.
      for (auto q = queue_.begin(); q != queue_.end(); ++q)
        if (*q == key) {
          queue_.erase(q);
          break;
        }
      queue_.push_front(key);
    }
    return 0;
  }
  if (size == IconSize::Tile && entries_.size() >= kMaxTiles)
    return 0;
  if (size == IconSize::Hero)
    evictHeroes();
  Entry &e = entries_[key];
  e.lastUse = clock_;
  queue_.push_front(key);
  pthread_cond_signal(&cond_);
  return 0;
}

void IconCache::evictHeroes() {
  // Called with the mutex held. Keeps the most recently used hero images.
  size_t heroes = 0;
  for (auto &kv : entries_)
    if (kv.first.second == (int)IconSize::Hero)
      ++heroes;
  while (heroes >= kMaxHeroes) {
    auto victim = entries_.end();
    for (auto it = entries_.begin(); it != entries_.end(); ++it)
      if (it->first.second == (int)IconSize::Hero &&
          it->second.state != Entry::Queued &&
          (victim == entries_.end() ||
           it->second.lastUse < victim->second.lastUse))
        victim = it;
    if (victim == entries_.end())
      break;
    std::free(victim->second.tex.pixels);
    entries_.erase(victim);
    --heroes;
  }
}

void *IconCache::threadEntry(void *self) {
  try {
    static_cast<IconCache *>(self)->worker();
  } catch (const std::exception &e) {
    std::printf("[PS4Hub] IconCache worker exception: %s\n", e.what());
  } catch (...) {
    std::printf("[PS4Hub] IconCache worker exception (unknown)\n");
  }
  return nullptr;
}

void IconCache::worker() {
  for (;;) {
    Key key;
    {
      Lock l(&mutex_);
      while (running_ && queue_.empty())
        pthread_cond_wait(&cond_, &mutex_);
      if (!running_)
        return;
      key = queue_.front();
      queue_.pop_front();
    }
    SoftRender::Texture tex;
    int px = key.second == (int)IconSize::Hero    ? kHeroPx
             : key.second == (int)IconSize::Small ? kSmallPx
                                                  : kTilePx;
    bool ok = load(key.first, px, tex);
    Lock l(&mutex_);
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      std::free(tex.pixels);
      continue;
    }
    it->second.tex = tex;
    it->second.state = ok ? Entry::Ready : Entry::Missing;
  }
}

ImTextureID IconCache::getFile(const std::string &path, IconSize size) {
  if (path.empty() || path[0] != '/' || path.find("..") != std::string::npos)
    return 0;
  return get(path, size); // keys starting with '/' are file paths
}

bool IconCache::load(const std::string &id, int px, SoftRender::Texture &out) {
  const bool isFile = !id.empty() && id[0] == '/';
  const std::string candidates[] = {
      isFile ? id : "/user/appmeta/" + id + "/icon0.png",
      isFile ? std::string() : "/user/appmeta/external/" + id + "/icon0.png",
      isFile ? std::string() : "/system_ex/app/" + id + "/sce_sys/icon0.png",
      isFile ? std::string() : "/system/vsh/app/" + id + "/sce_sys/icon0.png",
  };
  for (const std::string &path : candidates) {
    if (path.empty())
      continue;
    int w = 0, h = 0, n = 0;
    stbi_uc *src = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!src)
      continue;
    uint8_t *dst = static_cast<uint8_t *>(std::malloc((size_t)px * px * 4));
    bool ok = dst && stbir_resize_uint8(src, w, h, 0, dst, px, px, 0, 4);
    stbi_image_free(src);
    if (!ok) {
      std::free(dst);
      return false;
    }
    roundCorners(dst, px, px * 0.11f);
    out.pixels = dst;
    out.width = px;
    out.height = px;
    return true;
  }
  return false;
}
