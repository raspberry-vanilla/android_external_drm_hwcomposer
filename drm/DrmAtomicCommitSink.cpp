/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "drm/DrmAtomicCommitSink.h"

#include <unistd.h>

#include <cutils/trace.h>
#include <drm/drm_mode.h>
#include <utils/Trace.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

#include "compositor/LayerToPlaneJoiningPlan.h"
#include "drm/AtomicStateManager.h"
#include "drm/DrmAtomicStateManager.h"
#include "drm/DrmDevice.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmUnique.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {
namespace {

// NOLINTBEGIN(readability-function-cognitive-complexity)
bool CommitFrame(
    const std::vector<std::pair<AtomicStateManager *, AtomicCommitArgs>> &args,
    bool test_only,
    std::vector<std::pair<AtomicStateManager *, AtomicCommitResult>> &results) {
  if (args.empty()) {
    return false;
  }

  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
  auto *drm = (static_cast<DrmAtomicStateManager *>(args.front().first))
                  ->GetDevice();
  DrmModeAtomicReqUnique pset = MakeDrmModeAtomicReqUnique();
  int err = 0;
  bool seamless = args.front().second.seamless;
  bool nonblock = true;
  std::map<AtomicStateManager *, std::unique_ptr<AtomicRequest>> requests;
  for (auto [atomic_state_manager, arg] : args) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    if (static_cast<DrmAtomicStateManager *>(atomic_state_manager)
            ->GetDevice() != drm) {
      ALOGE("Commit for different device");
      return false;
    }
    if (seamless != arg.seamless) {
      ALOGE("Commit for different seamless level");
      return false;
    }
    auto request = atomic_state_manager->GetAtomicModeReqForArgs(arg);
    if (!arg.HasInputs()) {
      continue;
    }
    if (!request) {
      ALOGE("Failed to create request.");
      return false;
    }

    err = drmModeAtomicMerge(
        pset.get(),
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
        (static_cast<DrmAtomicStateManager::DrmAtomicRequest *>(request.get()))
            ->property_set.get());
    nonblock &= !arg.blocking && !arg.power_mode;
    if (err != 0) {
      ALOGE("Failed to append request");
      return false;
    }
    requests[atomic_state_manager] = std::move(request);
  }

  if (requests.empty()) {
    ALOGD("Commiting no input, success.");
    return true;
  }

  if (!test_only) {
    for (const auto &[atomic_state_manager, _] : args) {
      atomic_state_manager->WaitLastFrame();
    }
  }

  const int error_buf_max_size = 64;
  char err_buf[error_buf_max_size];
  uint32_t flags = seamless ? 0U : DRM_MODE_ATOMIC_ALLOW_MODESET;
  flags |= nonblock && !test_only ? DRM_MODE_ATOMIC_NONBLOCK : 0U;
  flags |= test_only ? DRM_MODE_ATOMIC_TEST_ONLY : 0U;
  {
    ATRACE_NAME((nonblock ? "Commit_nonblock" : "Commit_block"));
    err = drmModeAtomicCommit(*drm->GetFd(), pset.get(), flags, drm);
  }

  if (test_only) {
    return err == 0;
  }

  // Retry non-blocking commits that fail with EBUSY. The kernel returns EBUSY
  // when the previous commit's cleanup_done (checked by stall_checks in
  // drm_atomic_helper_setup_commit) has not yet completed. The out-fence
  // signals at vblank after hw_done, but cleanup_done is signaled slightly
  // later in the kernel's commit worker. This gap is typically < 1ms but
  // becomes more frequent at high resolutions (e.g. 8K) where commit
  // processing takes longer.
  if (err != 0 && errno == EBUSY && nonblock) {
    // Time out after approximately 30s (9375 * 3.2ms) of retries.
    static constexpr int kMaxBusyRetries = 9375;
    static constexpr int kInitialRetryUs = 200;
    int retry = 0;
    for (retry = 0; retry < kMaxBusyRetries; retry++) {
      if (err == 0 || errno != EBUSY) {
        // Only log of retries that were unusually long to prevent noisy
        // logging.
        ALOGI_IF(retry > 4, "Kernel recovered from EBUSY after %d attempts.",
                 retry + 1);
        break;
      }
      {
        ATRACE_NAME("EbusyRetryWait");
        // 200, 400, 800, 1600, 3200 us
        // Max retry duration is 3200 us after the 4th retry.
        usleep(kInitialRetryUs << std::min(retry, 4));
      }
      {
        ATRACE_NAME("EbusyRetryCommit");
        err = drmModeAtomicCommit(*drm->GetFd(), pset.get(), flags, drm);
      }
    }

    if (err != 0 && errno == EBUSY && retry >= kMaxBusyRetries) {
      LOG_ALWAYS_FATAL(
          "Could not recover from kernel EBUSY after %d attempts. Shutting "
          "down.", retry);
    }
  }

  if (err != 0 && seamless) {
    ALOGE(
        "Seamless commit failed, retrying a full modeset (visual artifacts may "
        "be observed). Error: %s",
        // NOLINTNEXTLINE(misc-include-cleaner)
        strerror_r(errno, err_buf, error_buf_max_size));
    err = drmModeAtomicCommit(*drm->GetFd(), pset.get(),
                              flags | DRM_MODE_ATOMIC_ALLOW_MODESET, drm);
  }
  if (err != 0) {
    ALOGE("Failed to commit pset ret=%d errno=%d strerror=%s\n", err, errno,
          // NOLINTNEXTLINE(misc-include-cleaner)
          strerror_r(errno, err_buf, error_buf_max_size));

    return false;
  }
  for (const auto &[atomic_state_manager, arg] : args) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
    auto res = static_cast<DrmAtomicStateManager *>(atomic_state_manager)
                   ->FinishPendingRequest(arg,
                                          std::move(
                                              requests[atomic_state_manager]));
    results.emplace_back(atomic_state_manager, res);
  }
  return true;
}
// NOLINTEND(readability-function-cognitive-complexity)

void CleanUpFailedRequest(
    const std::vector<std::pair<AtomicStateManager *, AtomicCommitArgs>>
        &args) {
  // Disable the hw used by the last active composition. This allows us to
  // signal the release fences from that composition to avoid hanging.
  std::vector<std::pair<AtomicStateManager *, AtomicCommitArgs>> cl_args;
  for (const auto &[atomic_state_manager, _] : args) {
    AtomicCommitArgs cl_arg{};
    cl_arg.composition = std::make_shared<LayerToPlaneJoiningPlan>();
    cl_args.emplace_back(atomic_state_manager, cl_arg);
  }
  std::vector<std::pair<AtomicStateManager *, AtomicCommitResult>>
      unused_result;
  if (!CommitFrame(cl_args, false, unused_result)) {
    ALOGE("Failed to clean-up active composition");
  }
}

}  // namespace

bool DrmAtomicCommitSink::TestAtomicCommit(
    const std::vector<std::pair<AtomicStateManager *, AtomicCommitArgs>> &args)
    const {
  std::vector<std::pair<AtomicStateManager *, AtomicCommitResult>>
      unused_result;
  return CommitFrame(args, /*test_only =*/true, unused_result);
}

std::vector<std::pair<AtomicStateManager *, AtomicCommitResult>>
DrmAtomicCommitSink::ExecuteAtomicCommit(
    const std::vector<std::pair<AtomicStateManager *, AtomicCommitArgs>>
        &args) {
  std::vector<std::pair<AtomicStateManager *, AtomicCommitResult>> results;
  if (!CommitFrame(args, /*test_only =*/false, results)) {
    CleanUpFailedRequest(args);
  } else if (results.empty()) {
    for (const auto &[atomic_state_manager, _] : args) {
      results.emplace_back(atomic_state_manager, AtomicCommitResult{});
    }
  }
  return results;
}
}  // namespace android::drm_hwcomposer
