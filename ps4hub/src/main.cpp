#include <SDL2/SDL.h>
#include <pthread.h>

#include <atomic>
#include <exception>
#include <string>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "app_catalog.h"
#include "app_controller.h"
#include "config.h"
#include "crash_handler.h"
#include "gamepad.h"
#include "gh_plugins.h"
#include "icon_cache.h"
#include "imgui.h"
#include "imgui_soft_renderer.h"
#include "network_service.h"
#include "ntfs_manager.h"
#include "platform.h"
#include "title_inspector.h"
#include "ui_theme.h"
#include "ui_views.h"

namespace {

int shiftOf(uint32_t mask) {
  int s = 0;
  while (mask && !(mask & 1u)) {
    mask >>= 1;
    ++s;
  }
  return s;
}

// Exit to shell on fatal SDL/init failure instead of hanging on a black screen.
[[noreturn]] void bailOut(const char *msg) {
  std::printf("[PS4Hub] fatal: %s (%s)\n", msg, SDL_GetError());
  Platform::shutdown();
  Platform::exitToShell();
}

// Background poller for slow status data (statfs, local IP, BGFT progress).
namespace Poller {
struct Snapshot {
  Platform::Storage storage;
  std::string ip;
  Platform::SystemStats stats;
};
pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t g_thread;
std::atomic<bool> g_run{false};
Snapshot g_snap;

void *loop(void *) {
  while (g_run) {
    try {
      Snapshot s;
      s.storage = Platform::queryStorage(DISK_MOUNT_POINT);
      s.ip = Platform::localIp();
      s.stats = Platform::querySystemStats();
      NetworkService::instance().refreshProgress();
      pthread_mutex_lock(&g_mutex);
      g_snap = s;
      pthread_mutex_unlock(&g_mutex);
    } catch (...) {
      std::printf("[PS4Hub] Poller: exception caught, continuing\n");
    }
    for (int i = 0; i < 10 && g_run; ++i)
      SDL_Delay(100);
  }
  return nullptr;
}
void start() {
  g_run = true;
  if (pthread_create(&g_thread, nullptr, loop, nullptr) != 0)
    g_run = false;
}
void stop() {
  if (!g_run)
    return;
  g_run = false;
  pthread_join(g_thread, nullptr);
}
Snapshot snapshot() {
  pthread_mutex_lock(&g_mutex);
  Snapshot s = g_snap;
  pthread_mutex_unlock(&g_mutex);
  return s;
}
} // namespace Poller

bool fileReadable(const char *path) {
  FILE *f = std::fopen(path, "rb");
  if (!f)
    return false;
  std::fclose(f);
  return true;
}

} // namespace

int main(int, char **) {
  setvbuf(stdout, nullptr, _IONBF, 0);

  Platform::initCore();
  CrashHandler::install(); // install AFTER initCore (which sets SIGSYS=IGN)

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) !=
      0) {
    std::printf("[PS4Hub] SDL_Init with game controller failed (%s), retrying "
                "without\n",
                SDL_GetError());
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0)
      bailOut("SDL_Init failed");
  }
  SDL_ShowCursor(SDL_DISABLE);

  SDL_Window *window =
      SDL_CreateWindow(APP_TITLE, SDL_WINDOWPOS_UNDEFINED,
                       SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT, 0);
  if (!window)
    bailOut("SDL_CreateWindow failed");

  SDL_Surface *surface = SDL_GetWindowSurface(window);
  if (!surface || !surface->format || surface->format->BytesPerPixel != 4 ||
      surface->w <= 0 || surface->h <= 0)
    bailOut("32-bit window surface required");

  SoftRender::Target target{};
  target.width = surface->w;
  target.height = surface->h;
  target.pitchPixels = surface->pitch / 4;
  target.rShift = shiftOf(surface->format->Rmask);
  target.gShift = shiftOf(surface->format->Gmask);
  target.bShift = shiftOf(surface->format->Bmask);
  target.alphaBits =
      surface->format->Amask ? surface->format->Amask : 0xFF000000u;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |=
      ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
  io.MouseDrawCursor = false;
  io.DisplaySize = ImVec2((float)surface->w, (float)surface->h);
  SoftRender::Init();
  Theme::Apply();
  // Game titles use ™ ® © and typographic punctuation; without these ranges
  // ImGui draws them as '?'. All are covered by Roboto.
  static const ImWchar kGlyphRanges[] = {
      0x0020, 0x00FF, // Basic Latin + Latin-1 (® © ° ...)
      0x0100, 0x024F, // Latin Extended-A/B (ą ę ł ő ...)
      0x0370, 0x03FF, // Greek
      0x0400, 0x04FF, // Cyrillic
      0x2010, 0x205E, // dashes, quotes, bullets, ellipsis
      0x20A0, 0x20BF, // currency (€ ...)
      0x2100, 0x2153, // letterlike (™ ℗ № ...)
      0x2190, 0x2199, // arrows
      0,
  };
  std::string fontPath = Platform::appPath("assets/fonts/Roboto-Medium.ttf");
  ImFont *font = fileReadable(fontPath.c_str())
                     ? io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 28.0f,
                                                    nullptr, kGlyphRanges)
                     : nullptr;
  if (!font && fileReadable(FONT_PATH)) {
    font =
        io.Fonts->AddFontFromFileTTF(FONT_PATH, 28.0f, nullptr, kGlyphRanges);
  }
  if (!font && Platform::status().jailbroken) {
    Platform::withSandbox([&] {
      if (fileReadable(FONT_PATH))
        font = io.Fonts->AddFontFromFileTTF(FONT_PATH, 28.0f, nullptr,
                                            kGlyphRanges);
    });
  }
  if (!font) {
    std::printf("[PS4Hub] Roboto font not loadable, using built-in font\n");
    io.Fonts->AddFontDefault();
  }

  Gamepad pad;
  pad.open();

  AppController controller; // stays empty ("Loading...") until stage 2
  UiRoot ui(controller);

  SoftRender::Backdrop backdrop;
  backdrop.init(target.width, target.height, Theme::kBgTop, Theme::kBgBottom);

  const double freq = (double)SDL_GetPerformanceFrequency();
  const double period = 1.0 / 60.0;
  uint64_t lastCounter = SDL_GetPerformanceCounter();
  double nextDeadline = lastCounter / freq + period;
  double flipAvg = 0.0; // EMA of SDL_UpdateWindowSurface time
  float clockSec = 0.0f;
  bool servicesStarted = false;
  uint64_t framesPresented = 0;
  bool running = true;

  while (running) {
    const uint32_t frameStart = SDL_GetTicks();

    try {
      SDL_Event ev;
      while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_QUIT)
          running = false;
        pad.handleEvent(ev);
      }
      pad.update(frameStart);
      if (ui.handleInput(pad))
        running = false;
      pad.endFrame();

      // Storage / IP / transfer progress come from a background poller so a
      // slow system call never costs a frame.
      if (servicesStarted) {
        Poller::Snapshot snap = Poller::snapshot();
        ui.setStorage(snap.storage);
        ui.setLocalIp(snap.ip);
        ui.setStats(snap.stats);
      }

      uint64_t now = SDL_GetPerformanceCounter();
      float dt = (float)((now - lastCounter) / freq);
      lastCounter = now;
      io.DeltaTime = dt > 0.0f ? (dt < 0.1f ? dt : 0.1f) : 1.0f / 60.0f;
      clockSec += io.DeltaTime;

      ImGui::NewFrame();
      ui.draw(io.DeltaTime);
      ImGui::Render();

      bool locked = !SDL_MUSTLOCK(surface) || SDL_LockSurface(surface) == 0;
      target.pixels =
          locked ? static_cast<uint32_t *>(surface->pixels) : nullptr;
      if (target.pixels) {
        backdrop.drawTo(target, clockSec);
        SoftRender::RenderDrawData(ImGui::GetDrawData(), target);
      }
      if (locked && SDL_MUSTLOCK(surface))
        SDL_UnlockSurface(surface);
      if (target.pixels) {
        uint64_t f0 = SDL_GetPerformanceCounter();
        SDL_UpdateWindowSurface(window); // flip to the video-out buffer
        double flip = (SDL_GetPerformanceCounter() - f0) / freq;
        flipAvg = flipAvg * 0.9 + flip * 0.1;
        ++framesPresented;
      }

      if (!servicesStarted && framesPresented >= 2) {
        servicesStarted = true;
        Platform::initServices();
        controller.reload();
        IconCache::instance().start();
        TitleInspector::instance().start();
        NetworkService::instance().start(SERVER_PORT, Platform::userId());
        Poller::start();

        AppCatalog::startAutoRefresh();
      }

      // Pacing: if the flip already waits for vblank it paces us; otherwise
      // sleep to a fixed 60 Hz deadline (absolute, so errors don't drift).
      double t = SDL_GetPerformanceCounter() / freq;
      if (flipAvg > 0.004) {
        nextDeadline = t + period;
      } else {
        if (t > nextDeadline + period)
          nextDeadline = t; // fell behind: resync
        double wait = nextDeadline - t;
        if (wait > 0.0015)
          SDL_Delay((uint32_t)((wait - 0.001) * 1000.0));
        while (SDL_GetPerformanceCounter() / freq <
               nextDeadline) { /* finish on a short spin */
        }
        nextDeadline += period;
      }
    } catch (const std::exception &e) {
      std::printf("[PS4Hub] frame exception: %s (continuing)\n", e.what());
    } catch (...) {
      std::printf("[PS4Hub] frame exception (unknown, continuing)\n");
    }
  }

  // Leave through the system (see Platform::exitToShell). The exit fade
  // already blacked the screen. We hand off to the shell AS FAST AS
  // POSSIBLE here:
  //   * do NOT pthread_join() worker threads - that can take seconds,
  //     during which the user sees an unresponsive black screen while
  //     the shell waits for us to finish
  //   * only drop the jailbreak credentials (hard requirement) and tell
  //     the kernel to unmount the NTFS volume cleanly
  //   * leave everything else to the kernel's process teardown
  Platform::shutdown(); // drops the GoldHEN jailbreak
  // Skip NtfsManager::unmount() on exit: the volume is mounted RDONLY so
  // there is nothing to flush, and ntfs_umount() can walk the whole MFT
  // which on a 60 GB drive adds visible delay on the already-black screen.
  // The kernel reclaims the FD when the process goes away.
  Platform::exitToShell(); // sceSystemServiceLoadExec("exit") -> system kills
                           // us
}
