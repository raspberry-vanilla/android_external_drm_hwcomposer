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
#include <memory>
#include <string>

#include "backend/Backend.h"

namespace android::drm_hwcomposer {

class AtomicCommitSink;
class BufferInfoGetter;
class DrmConnector;
class DrmDevice;
struct DrmDisplayPipeline;

// BackendManager is a singleton that manages the registration of Backend
// creators and instantiating them for DrmDevices.
class BackendManager {
 public:
  using BackendCreator = std::function<std::unique_ptr<Backend>(DrmDevice &)>;
  using BackendInit = std::function<bool()>;

  struct BackendRegistration {
    BackendCreator creator;
    BackendInit init;
  };

  static BackendManager &GetInstance();

  void Register(const std::string &name, BackendRegistration registration);
  void InitializeBackends();
  std::unique_ptr<Backend> CreateBackendForDevice(DrmDevice &drm);

  // Template helper for static registration of backends
  template <typename T>
  class RegisterBackend {
   public:
    explicit RegisterBackend(const std::string &name) {
      BackendManager::GetInstance().Register(name,
                                             {.creator = [](DrmDevice &drm) {
                                               return std::make_unique<T>(drm);
                                             }});
    }
  };

 private:
  BackendManager() = default;

  bool initialized_ = false;
  std::map<std::string, BackendRegistration> backends_;
};

}  // namespace android::drm_hwcomposer
