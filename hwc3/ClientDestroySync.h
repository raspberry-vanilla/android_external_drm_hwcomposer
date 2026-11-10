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

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace android::drm_hwcomposer {

struct ClientDestroySync {
  std::mutex mutex;
  std::condition_variable cv;
  bool client_alive = false;

  bool WaitForDestroyedLocked(std::unique_lock<std::mutex>& lock,
                              const std::function<bool()>& is_client_expired,
                              std::chrono::milliseconds grace_timeout,
                              std::chrono::milliseconds destroy_timeout) {
    if (!is_client_expired()) {
      // Wait briefly in case an asynchronous Binder BC_DECREFS from the
      // previous client's TearDown() is still in flight; if the client remains
      // held (e.g. VTS CreateClientSingleton), fail with kNoResources.
      cv.wait_for(lock, grace_timeout, is_client_expired);
      if (!is_client_expired()) {
        return false;
      }
    }
    // In libc++, std::weak_ptr::expired() becomes true before ~ComposerClient()
    // runs. Wait until ~ComposerClient() finishes tearing down KMS and closing
    // the DRM device before initializing the next DrmHwcThree instance.
    return cv.wait_for(lock, destroy_timeout,
                       [this]() { return !client_alive; });
  }

  static void NotifyClientDestroyed(
      const std::weak_ptr<ClientDestroySync>& client_destroy_sync) {
    if (auto sync = client_destroy_sync.lock()) {
      {
        std::scoped_lock lock(sync->mutex);
        sync->client_alive = false;
      }
      sync->cv.notify_all();
    }
  }
};

}  // namespace android::drm_hwcomposer
