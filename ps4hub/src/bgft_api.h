#pragma once
// BGFT (background file transfer) ABI as the firmware expects it.
//
// The OpenOrbis <orbis/_types/bgft.h> declares packageSize and the progress
// lengths as 32-bit; the firmware uses 64-bit fields there, so those headers
// are not used for calls. Layouts below match flatz's original Remote Package
// Installer and RPI NextGen.
#include <cstdint>

namespace Bgft {

constexpr int32_t kInvalidTask = -1;
constexpr int32_t kErrAlreadyInstalled  = (int32_t)0x80990088;
constexpr int32_t kErrAlreadyDownloading = (int32_t)0x80990086;
constexpr int32_t kOptDisableCdnQueryParam = 0x10000;

struct DownloadParam {
  int32_t userId;
  int32_t entitlementType;
  const char *id;         // content id
  const char *contentUrl; // manifest json url
  const char *contentExUrl;
  const char *contentName; // shown in Notifications > Downloads
  const char *iconPath;
  const char *skuId;
  int32_t option;
  const char *playgoScenarioId;
  const char *releaseDate;
  const char *packageType; // PS4GD / PS4AC / PS4AL / PS4DP
  const char *packageSubType;
  uint64_t packageSize;
};

struct TaskProgress {
  uint32_t bits;
  int32_t errorResult;
  uint64_t length;
  uint64_t transferred;
  uint64_t lengthTotal;
  uint64_t transferredTotal;
  uint32_t numIndex;
  uint32_t numTotal;
  uint32_t restSec;
  uint32_t restSecTotal;
  int32_t preparingPercent;
  int32_t localCopyPercent;
};

// Resolved at runtime from libSceBgft; optional entries may be null.
struct Api {
  int32_t (*registerTask)(DownloadParam *, int32_t *taskId);
  int32_t (*debugRegisterPkg)(DownloadParam *,
                              int32_t *taskId); // optional (patches)
  int32_t (*startTask)(int32_t);
  int32_t (*stopTask)(int32_t);
  int32_t (*pauseTask)(int32_t);
  int32_t (*resumeTask)(int32_t);
  int32_t (*unregisterTask)(int32_t);
  int32_t (*getProgress)(int32_t, TaskProgress *);
  int32_t (*findTaskByContentId)(const char *, int32_t subType,
                                 int32_t *taskId);
};

} // namespace Bgft
