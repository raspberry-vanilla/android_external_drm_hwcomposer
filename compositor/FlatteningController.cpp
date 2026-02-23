/*
 * Copyright (C) 2023 The Android Open Source Project
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

/*
 * Usually, display controllers do not use intermediate buffer for composition
 * results. Instead, they scan-out directly from the input buffers, composing
 * the planes on the fly every VSYNC.
 *
 * Flattening is a technique that reduces memory bandwidth and power consumption
 * by converting non-updating multi-plane composition into a single-plane.
 * Additionally, flattening also makes more shared planes available for use by
 * other CRTCs.
 *
 * If the client is not updating layers for 1 second, FlatCon triggers a
 * callback to refresh the screen. The compositor should mark all layers to be
 * composed by the client into a single framebuffer using GPU.
 */

#include "FlatteningController.h"

#include <chrono>
#include <mutex>
#include <thread>

#include <android-base/thread_annotations.h>

#include "compositor/FlatteningEventAtomReporter.h"
#include "hwc/HwcDisplay.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

FlatteningController::FlatteningController(DisplayHandle handle,
                                           FlatConCallbacks callbacks,
                                           std::chrono::milliseconds timeout)
    : handle_(handle),
      reporter_(FlatteningEventAtomReporter::Create()),
      cbks_(std::move(callbacks)),
      timeout_(timeout) {
  thread_ = std::thread(&FlatteningController::ThreadFn, this);
}

FlatteningController::~FlatteningController() {
  StopThread();
  thread_.join();
}

void FlatteningController::DisableFlattening() {
  auto lock = std::lock_guard<std::mutex>(mutex_);
  SetState(State::kDisabled);
}

void FlatteningController::NewFrame() {
  auto lock = std::lock_guard<std::mutex>(mutex_);

  if (state_ == State::kTriggeredCallback) {
    SetState(State::kFlattened);
    return;
  }

  sleep_until_ = std::chrono::system_clock::now() + timeout_;
  bool was_active = (state_ == State::kActive);
  SetState(State::kActive);

  if (!was_active) {
    cv_.notify_all();
  }
}

bool FlatteningController::ShouldFlatten() const {
  auto lock = std::lock_guard<std::mutex>(mutex_);
  return state_ == State::kTriggeredCallback || state_ == State::kFlattened;
}

void FlatteningController::StopThread() {
  auto lock = std::lock_guard<std::mutex>(mutex_);
  SetState(State::kExitThread);
  cv_.notify_all();
}

void FlatteningController::ThreadFn() {
  for (;;) {
    std::unique_lock<std::mutex> lock(mutex_);
    base::ScopedLockAssertion lock_assertion(mutex_);
    if (state_ == State::kExitThread) {
      break;
    }

    if (sleep_until_ <= std::chrono::system_clock::now() &&
        (state_ == State::kActive)) {
      SetState(State::kTriggeredCallback);
      ALOGV("Timeout. Sending an event to compositor");
      cbks_.trigger();
    }

    if (state_ != State::kActive) {
      ALOGV("Wait");
      cv_.wait(lock);
    } else {
      ALOGV("Wait_until");
      cv_.wait_until(lock, sleep_until_);
    }
  }
}

void FlatteningController::SetState(State state) {
  if (state_ != state) {
    state_ = state;

    if (reporter_) {
      switch (state_) {
        case State::kDisabled:
        case State::kActive:
        case State::kFlattened:
          reporter_->PushAtom(handle_, state_);
          break;
        case State::kTriggeredCallback:
        case State::kExitThread:
          // Internal states, no need to report.
          break;
      }
    }
  }
}

}  // namespace android::drm_hwcomposer
