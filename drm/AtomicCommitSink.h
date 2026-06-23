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

#include <utility>
#include <vector>

#include "drm/AtomicStateManager.h"

namespace android::drm_hwcomposer {

struct CommitStatus;

template <typename T>
class CommitStatusOr;

class AtomicCommitSink {
 public:
  virtual ~AtomicCommitSink() = default;
  virtual CommitStatus TestAtomicCommit(
      const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>& args)
      const = 0;
  virtual CommitStatusOr<
      std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>>
  ExecuteAtomicCommit(
      const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>&
          args) = 0;
};

}  // namespace android::drm_hwcomposer
