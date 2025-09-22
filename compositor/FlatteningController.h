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

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <thread>

#include <android-base/thread_annotations.h>

namespace android::drm_hwcomposer {

// NOLINTNEXTLINE(misc-unused-using-decls): False positive
using std::chrono_literals::operator""s;

struct FlatConCallbacks {
  std::function<void()> trigger;
};

class FlatteningController {
 public:
  FlatteningController(FlatConCallbacks callbacks,
                       std::chrono::milliseconds timeout);
  ~FlatteningController();

  // Disable flattening and stop checking for an idle scene.
  void DisableFlattening();

  // Registers a new frame by updating the flattening state as needed and
  // resetting the idle timer.
  void NewFrame();

  // Returns true if the FlatteningController detects that the scene is idle
  // and should be flattened by the compositor.
  bool ShouldFlatten() const;

 private:
  // Stop the helper thread
  void StopThread();

  void ThreadFn();

  std::thread thread_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;

  enum class State {
    // Thread is not active, should not flatten.
    kDisabled,
    // Thread is active. Waiting for timeout.
    kActive,
    // Callback has been triggered but NewFrame has not yet been called.
    kTriggeredCallback,
    // Callback was triggered and NewFrame was called once.
    kFlattened,
    // Thread will exit without any further processing or state update.
    kExitThread,
  };

  /* Disable the controller by default as it can cause refresh event to be
   * issued at creation time, even when it is not required. This can fail VTS
   * tests at teardown that check for this behaviour. See:
   * https://cs.android.com/android/platform/superproject/main/+/cedca652b903e4f4e584e457b5a7038e0825fb94:hardware/interfaces/graphics/composer/aidl/vts/VtsComposerClient.cpp;drc=a2a6deaf5036e081f48379b6573db4465538b5ac;l=604
   */
  State state_ GUARDED_BY(mutex_) = State::kDisabled;

  // Only accessed from helper thread.
  const FlatConCallbacks cbks_;
  decltype(std::chrono::system_clock::now()) sleep_until_{};
  const std::chrono::milliseconds timeout_;
};

}  // namespace android::drm_hwcomposer
