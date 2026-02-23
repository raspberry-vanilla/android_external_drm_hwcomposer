/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "ResourceManager.h"

#include <android-base/strings.h>
#include <sys/stat.h>

#include <ctime>
#include <sstream>

#include "backend/BackendManager.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "drm/DrmConnector.h"
#include "drm/DrmDevice.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/UEventListener.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android::drm_hwcomposer {

ResourceManager::ResourceManager(
    PipelineToFrontendBindingInterface *p2f_bind_interface)
    : frontend_interface_(p2f_bind_interface) {
  uevent_listener_ = UEventListener::CreateInstance();
}

ResourceManager::~ResourceManager() {
  uevent_listener_->StopThread();
}

void ResourceManager::Init() {
  if (initialized_) {
    ALOGE("Already initialized");
    return;
  }

  color_pipeline_enabled_ = Properties::UseColorPipeline();

  // Could be a valid path or it can have at the end of it the wildcard %
  // which means that it will try open all devices until an error is met.
  std::string path_pattern = Properties::GetDevicePath();
  if (path_pattern.empty()) {
    path_pattern = "/dev/dri/card%";
  }
  if (path_pattern.back() != '%') {
    auto dev = DrmDevice::CreateInstance(path_pattern, this, 0);
    if (dev) {
      drms_.emplace_back(std::move(dev));
    }
  } else {
    path_pattern.resize(path_pattern.size() - 1);
    for (int idx = 0;; ++idx) {
      std::ostringstream path;
      path << path_pattern << idx;

      struct stat buf {};
      if (stat(path.str().c_str(), &buf) != 0)
        break;

      auto dev = DrmDevice::CreateInstance(path.str(), this, idx);
      if (dev) {
        drms_.emplace_back(std::move(dev));
      }
    }
  }

  // Ensure that Backends have been initialized before the BackendManager is
  // used.
  BackendManager::GetInstance().InitializeBackends();

  auto display_str = Properties::InternalDisplayNames();
  auto display_names = base::Tokenize(display_str, ",");
  displays_.insert(display_names.begin(), display_names.end());

  scale_with_gpu_ = Properties::ScaleWithGpu();
  ctm_handling_ = Properties::GetCtmHandling();

  if (BufferInfoGetter::GetInstance() == nullptr) {
    auto buffer_info_getter = BackendManager::GetInstance()
                                  .CreateBufferInfoGetter();
    if (!buffer_info_getter) {
      ALOGE("Failed to create BufferInfoGetter");
      return;
    }
    BufferInfoGetter::Init(std::move(buffer_info_getter));
  }

  for (auto &drm : drms_) {
    drm->ResetConnectorsAndCrtcs();
  }

  uevent_listener_->RegisterHotplugHandler([this] {
    const std::unique_lock lock(GetMainLock());
    for (auto &drm : drms_) {
      drm->RefreshConnectors();
    }
    UpdateFrontendDisplays();
  });

  UpdateFrontendDisplays();

  initialized_ = true;
}

void ResourceManager::DeInit() {
  if (!initialized_) {
    ALOGE("Not initialized");
    return;
  }

  uevent_listener_->RegisterHotplugHandler({});

  DetachAllFrontendDisplays();
  drms_.clear();

  initialized_ = false;
}

const std::set<std::string>& ResourceManager::GetInternalDisplayNames() {
  return displays_;
}

auto ResourceManager::GetTimeMonotonicNs() -> int64_t {
  struct timespec ts {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  constexpr int64_t kNsInSec = 1000000000LL;
  return (int64_t(ts.tv_sec) * kNsInSec) + int64_t(ts.tv_nsec);
}

void ResourceManager::UpdateFrontendDisplays() {
  auto ordered_connectors = GetOrderedConnectors();

  for (auto *conn : ordered_connectors) {
    conn->UpdateModes();
    auto connected = conn->IsConnected();
    auto attached = attached_pipelines_.count(conn) != 0;

    if (connected != attached) {
      ALOGI("%s connector %s", connected ? "Attaching" : "Detaching",
            conn->GetName().c_str());

      if (connected) {
        std::shared_ptr<DrmDisplayPipeline>
            pipeline = BackendManager::GetInstance().CreatePipelineForConnector(
                *conn);
        ALOGE_IF(pipeline == nullptr,
                 "Failed to create pipeline for connector %s",
                 conn->GetName().c_str());
        if (pipeline) {
          frontend_interface_->BindDisplay(pipeline);
          attached_pipelines_[conn] = std::move(pipeline);
        }
      } else {
        auto &pipeline = attached_pipelines_[conn];
        frontend_interface_->UnbindDisplay(pipeline);
        attached_pipelines_.erase(conn);
      }
    }
    if (connected) {
      if (!conn->IsLinkStatusGood())
        frontend_interface_->NotifyDisplayLinkStatus(attached_pipelines_[conn]);

      // If content protection is not enabled anymore, inform frontend so it
      // can terminate HDCP handling for this display.
      if (conn->UpdateContentProtection() &&
          !conn->IsContentProtectionEnabled()) {
        frontend_interface_->NotifyHdcpTermination(attached_pipelines_[conn]);
      }
    }
  }
  frontend_interface_->FinalizeDisplayBinding();
}

void ResourceManager::DetachAllFrontendDisplays() {
  for (auto &p : attached_pipelines_) {
    frontend_interface_->UnbindDisplay(p.second);
  }
  attached_pipelines_.clear();
  frontend_interface_->FinalizeDisplayBinding();
}

auto ResourceManager::GetOrderedConnectors() -> std::vector<DrmConnector *> {
  /* Put internal displays first then external to
   * ensure Internal will take Primary slot
   */

  std::vector<DrmConnector *> ordered_connectors;

  for (auto &drm : drms_) {
    for (const auto &conn : drm->GetConnectors()) {
      if (conn->IsInternal()) {
        ordered_connectors.emplace_back(conn.get());
      }
    }
  }

  for (auto &drm : drms_) {
    for (const auto &conn : drm->GetConnectors()) {
      if (conn->IsExternal()) {
        ordered_connectors.emplace_back(conn.get());
      }
    }
  }

  return ordered_connectors;
}

auto ResourceManager::GetVirtualDisplayPipeline()
    -> std::shared_ptr<DrmDisplayPipeline> {
  for (auto &drm : drms_) {
    for (const auto &conn : drm->GetWritebackConnectors()) {
      auto pipeline = DrmDisplayPipeline::CreatePipeline(*conn);
      if (!pipeline) {
        ALOGE("Failed to create pipeline for writeback connector %s",
              conn->GetName().c_str());
      }
      if (pipeline) {
        return pipeline;
      }
    }
  }
  return {};
}

auto ResourceManager::GetWritebackConnectorsCount() -> uint32_t {
  uint32_t count = 0;
  for (auto &drm : drms_) {
    count += drm->GetWritebackConnectors().size();
  }
  return count;
}

}  // namespace android::drm_hwcomposer
