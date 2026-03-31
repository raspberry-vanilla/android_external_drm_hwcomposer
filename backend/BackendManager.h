/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace android::drm_hwcomposer {

class AtomicCommitSink;
class BufferInfoGetter;
class DrmConnector;
struct DrmDisplayPipeline;

// BackendManager is a singleton that manages the registration of Backends and
// finding a Backend which can be used to create a DrmDisplayPipeline for a
// DrmConnector.
class BackendManager {
 public:
  // Backend is the top-level interface for any driver-specific or custom logic
  // for configuring a DrmDisplayPipeline for a DrmConnector, as well as
  // driver-specific or custom composition rules in a CompositionPlanner
  // implementation.
  class Backend {
   public:
    // Backend base class will register a backend called |name| on construction,
    // and deregister on destruction. The name must be unique across all
    // Backends.
    explicit Backend(const std::string &name);
    virtual ~Backend();

    // Backends with non-trivial initialization can override Init. If this
    // returns false, then the backend will be removed from the BackendManager.
    virtual bool Init() {
      return true;
    };

    // Create a DrmDisplayPipeline for the given DrmConnector, including
    // creating the CompositionPlanner for this DrmDisplayPipeline.
    virtual std::unique_ptr<DrmDisplayPipeline> CreatePipeline(
        DrmConnector &connector) = 0;

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
    // Get the atomiCommitSink for the Backend.
    virtual std::unique_ptr<AtomicCommitSink> CreateAtomicCommitSink() = 0;

   private:
    std::string name_;
  };

  static BackendManager &GetInstance();
  void RegisterBackend(const std::string &name, Backend *backend);
  void UnregisterBackend(const std::string &name);

  void InitializeBackends();

  std::unique_ptr<DrmDisplayPipeline> CreatePipelineForConnector(
      DrmConnector &connector);

  // Get the BufferInfoGetter for the Backend.
  std::unique_ptr<BufferInfoGetter> CreateBufferInfoGetter();

  bool IsDozeSupported(const std::string &driver_name);
  bool IsDozeSuspendSupported(const std::string &driver_name);
  bool IsSuspendSupported(const std::string &driver_name);
  std::unique_ptr<AtomicCommitSink> CreateAtomicCommitSink(
      const std::string &driver_name);

 private:
  Backend *GetBackendByName(std::string &name);

  BackendManager() = default;

  std::map<std::string, Backend *> available_backends_;
};

}  // namespace android::drm_hwcomposer
