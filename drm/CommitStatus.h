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

#include <optional>

#include "utils/log.h"

namespace android::drm_hwcomposer {

// The result of executing a commit, indicating either success or failure with
// the corresponding error code.
struct CommitStatus {
  bool success = true;
  // Indicates internal error iff success is false and error code is 0.
  // Otherwise indicates commit error.
  int error_code = 0;

  static CommitStatus Success() {
    return {.success = true, .error_code = 0};
  }

  static CommitStatus InternalFailure() {
    return {.success = false, .error_code = 0};
  }

  static CommitStatus Failure(int error_code) {
    ALOGW_IF(error_code == 0,
             "Ambiguous failure with error_code=0. Use CommitStatus::Success "
             "or CommitStatus::InternalFailure");
    return {.success = false, .error_code = error_code};
  }
};

// An object which either holds a value of type T in the event of a successful
// commit, or a CommitStatus indicating failure in the event of an unsuccessful
// commit.
template <typename T>
class CommitStatusOr {
 public:
  // Constructs a new CommitStatusOr<T> with an unsuccessful status. Requires
  // that the provided status does not represent success, or it will be replaced
  // with an internal failure status.
  explicit CommitStatusOr<T>(const CommitStatus& status) : status_(status) {
    if (status.success) {
      ALOGE("CommitStatusOr must not be created with a success status");
      status_ = CommitStatus::InternalFailure();
    }
  }

  explicit CommitStatusOr<T>(const T& value)
      : value_(value), status_(CommitStatus::Success()) {
  }

  bool IsSuccess() const {
    return value_.has_value();
  }

  const std::optional<T>& GetValue() const {
    return value_;
  }

  const CommitStatus& GetStatus() const {
    return status_;
  }

 private:
  std::optional<T> value_ = std::nullopt;
  CommitStatus status_{};
};

}  // namespace android::drm_hwcomposer