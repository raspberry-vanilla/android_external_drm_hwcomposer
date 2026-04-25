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

#pragma once

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace android::drm_hwcomposer {

enum class CtmHandling;
class DrmConnector;
class DrmDevice;
struct DrmDisplayPipeline;
class UEventListener;

class PipelineToFrontendBindingInterface {
 public:
  virtual ~PipelineToFrontendBindingInterface() = default;
  virtual bool BindDisplay(std::shared_ptr<DrmDisplayPipeline>) = 0;
  virtual bool UnbindDisplay(std::shared_ptr<DrmDisplayPipeline>) = 0;
  virtual void FinalizeDisplayBinding() = 0;
  virtual void NotifyDisplayLinkStatus(
      std::shared_ptr<DrmDisplayPipeline> pipeline) = 0;
  virtual void NotifyHdcpTermination(
      std::shared_ptr<DrmDisplayPipeline> pipeline) = 0;
};

class ResourceManager {
 public:
  explicit ResourceManager(
      PipelineToFrontendBindingInterface *p2f_bind_interface);
  ResourceManager(const ResourceManager &) = delete;
  ResourceManager &operator=(const ResourceManager &) = delete;
  ResourceManager(const ResourceManager &&) = delete;
  ResourceManager &&operator=(const ResourceManager &&) = delete;
  ~ResourceManager();

  void Init();

  void DeInit();

  bool ForcedScalingWithGpu() const {
    return scale_with_gpu_;
  }

  auto &GetCtmHandling() const {
    return ctm_handling_;
  }

  bool UseColorPipeline() const {
    return color_pipeline_enabled_;
  }

  int ForceColorMode() const {
    return force_color_mode_;
  }

  auto &GetMainLock() {
    return main_lock_;
  }

  auto GetVirtualDisplayPipeline() -> std::shared_ptr<DrmDisplayPipeline>;
  auto GetWritebackConnectorsCount() -> uint32_t;
  auto GetInternalDisplayNames() -> const std::set<std::string>&;

  static auto GetTimeMonotonicNs() -> int64_t;

 private:
  auto GetOrderedConnectors() -> std::vector<DrmConnector *>;
  void UpdateFrontendDisplays();
  void DetachStalePipelines(
      const std::vector<std::unique_ptr<DrmConnector>> &stale_connectors);
  void DetachAllFrontendDisplays();

  std::vector<std::unique_ptr<DrmDevice>> drms_;
  std::set<std::string> displays_;

  // Android properties:
  bool scale_with_gpu_{};
  CtmHandling ctm_handling_{};
  bool color_pipeline_enabled_{};
  int force_color_mode_{};

  std::shared_ptr<UEventListener> uevent_listener_;

  std::recursive_mutex main_lock_;

  std::map<DrmConnector *, std::shared_ptr<DrmDisplayPipeline>>
      attached_pipelines_;

  PipelineToFrontendBindingInterface *const frontend_interface_;

  bool initialized_{};
};

}  // namespace android::drm_hwcomposer
