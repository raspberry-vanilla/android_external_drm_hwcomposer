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

#pragma once

#include <xf86drmMode.h>

#include <cstdint>
#include <utility>
#include <vector>

#include "drm/AtomicCommitSink.h"
#include "drm/AtomicStateManager.h"
#include "drm/CommitStatus.h"

namespace android::drm_hwcomposer {
class DrmAtomicCommitSink : public AtomicCommitSink {
 public:
  CommitStatus TestAtomicCommit(
      const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>& args)
      const override;
  CommitStatusOr<
      std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>>
  ExecuteAtomicCommit(
      const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>& args)
      const override;

 protected:
  virtual int AtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags,
                           void* user_data) const;
  // Virtual seam allowing unit tests to intercept EBUSY backoff sleeps
  // without waiting 30 seconds of wall-clock time.
  virtual void RetrySleep(uint32_t usec) const;

 private:
  CommitStatus CommitFrame(
      const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>& args,
      bool test_only,
      std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>& results)
      const;
  void CleanUpFailedRequest(
      const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>& args)
      const;
};
}  // namespace android::drm_hwcomposer
