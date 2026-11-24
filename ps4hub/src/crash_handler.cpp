#include "crash_handler.h"

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "platform.h"

namespace CrashHandler {
namespace {

std::atomic<bool> g_inHandler{false};

const char *signalName(int sig) {
  switch (sig) {
  case SIGSEGV:
    return "SIGSEGV";
  case SIGBUS:
    return "SIGBUS";
  case SIGFPE:
    return "SIGFPE";
  case SIGILL:
    return "SIGILL";
  case SIGABRT:
    return "SIGABRT";
  case SIGPIPE:
    return "SIGPIPE";
  case SIGTERM:
    return "SIGTERM";
  default:
    return "?";
  }
}

void handler(int sig) {
  // Re-entrancy guard: a second fault in the handler would loop forever.
  bool expected = false;
  if (!g_inHandler.compare_exchange_strong(expected, true)) {
    _exit(1);
  }

  // Write a short line to klog. async-signal-safe style: no std::string, no
  // malloc, just write() of a prebuilt buffer.
  char msg[128];
  int n = std::snprintf(msg, sizeof(msg),
                        "[PS4Hub] fatal signal %d (%s), leaving to shell\n",
                        sig, signalName(sig));
  if (n > 0)
    (void)write(2, msg, (size_t)n);

  // Best-effort graceful teardown. These may themselves fault; the
  // re-entrancy guard above bounces us into _exit if they do.
  Platform::shutdown();
  Platform::exitToShell();
}

void installOne(int sig) {
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = handler;
  sa.sa_flags = SA_RESTART;
  sigemptyset(&sa.sa_mask);
  sigaction(sig, &sa, nullptr);
}

} // namespace

void install() {
  // SIGPIPE from closed sockets must not kill the server thread.
  signal(SIGPIPE, SIG_IGN);

  installOne(SIGSEGV);
  installOne(SIGBUS);
  installOne(SIGFPE);
  installOne(SIGILL);
  installOne(SIGABRT);
  installOne(SIGTERM);
}

} // namespace CrashHandler
