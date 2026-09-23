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

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace android::drm_hwcomposer {

class AtomicCommitSink;
class BufferInfoGetter;
class DrmConnector;
class DrmDevice;
struct DrmDisplayPipeline;

class Backend {
 public:
  using RefreshCallback = std::function<void()>;
  using HotplugHandler = std::function<void(uint32_t /*connector_id*/,
                                            bool /*connected*/)>;

  explicit Backend(DrmDevice &drm);
  virtual ~Backend() = default;

  // Create a DrmDisplayPipeline for the given DrmConnector, including
  // creating the CompositionPlanner for this DrmDisplayPipeline.
  virtual std::unique_ptr<DrmDisplayPipeline> CreatePipeline(
      DrmConnector &connector) = 0;

  // Set (or clear if callback is null) a refresh trigger callback for a
  // connector.
  // NOLINTBEGIN(performance-unnecessary-value-param)
  virtual void SetRefreshCallbackForConnector(uint32_t /*connector_id*/,
                                              RefreshCallback /*callback*/) {
  }

  // Set the hotplug event handler for the backend. Note that a backend might
  // call the passed function before SetHotplugHandler returns.
  virtual void SetHotplugHandler(HotplugHandler /*handler*/) {
  }
  // NOLINTEND(performance-unnecessary-value-param)

  // Returns true if this backend provides its own hotplug events via
  // SetHotplugHandler, or false if UEventListener and standard polling
  // should be used.
  virtual bool UseBackendHotplug() const {
    return false;
  }

  // Get the BufferInfoGetter for the Backend.
  virtual std::unique_ptr<BufferInfoGetter> CreateBufferInfoGetter() = 0;

  virtual bool SupportsDoze() const {
    return false;
  }

  virtual bool SupportsDozeSuspend() const {
    return false;
  }

  virtual bool SupportsSuspend() const {
    return false;
  }
  // Get the AtomicCommitSink for the Backend.
  virtual std::unique_ptr<AtomicCommitSink> CreateAtomicCommitSink() = 0;

  virtual std::optional<std::string> Dump() {
    return std::nullopt;
  }

 protected:
  DrmDevice &GetDrmDevice() const {
    return *drm_;
  }

 private:
  // The DrmDevice owns this Backend. Guaranteed not to be nullptr because it is
  // passed to the constructor by reference.
  DrmDevice *drm_;
};

}  // namespace android::drm_hwcomposer
