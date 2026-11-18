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

#include <atomic>
#include <condition_variable>
#include <functional>
#include <thread>

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace android::drm_hwcomposer {

enum class CtmHandling;
class DrmConnector;
class DrmDevice;
struct DrmDisplayPipeline;
class UEvent;
class UEventListener;
struct UEventHotplugEvent;

class PipelineToFrontendBindingInterface {
 public:
  virtual ~PipelineToFrontendBindingInterface() = default;
  virtual bool BindDisplay(std::shared_ptr<DrmDisplayPipeline>) = 0;
  virtual bool UnbindDisplay(std::shared_ptr<DrmDisplayPipeline>) = 0;
  virtual bool ReplaceDisplay(
      std::shared_ptr<DrmDisplayPipeline> old_pipeline,
      std::shared_ptr<DrmDisplayPipeline> new_pipeline) = 0;
  virtual void FinalizeDisplayBinding() = 0;
  // Destroys all frontend display objects and detaches their DRM pipelines
  // before the underlying DrmDevice instances are cleared during teardown.
  virtual void DeinitDisplays() = 0;
  virtual void NotifyDisplayLinkStatus(
      std::shared_ptr<DrmDisplayPipeline> pipeline) = 0;
  virtual void NotifyIncompatibleCable() = 0;
  virtual void NotifyDisplayConnected(
      std::shared_ptr<DrmDisplayPipeline> pipeline) = 0;
  virtual void NotifyHdcpTermination(
      std::shared_ptr<DrmDisplayPipeline> pipeline) = 0;
  virtual void FlushHotplugEvents() = 0;
  [[nodiscard]] virtual bool IsEarlyBootAnimationActive() const {
    return false;
  }
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

  [[nodiscard]] bool IsInitialized() const {
    // Atomic for unlocked callers (e.g. ComposerClient::registerCallback(),
    // ~DrmHwcThree()); writes occur under (init_mutex_, hotplug_mutex_,
    // main_lock_).
    return initialized_;
  }

  bool ForcedScalingWithGpu() const {
    return scale_with_gpu_;
  }

  auto &GetCtmHandling() const {
    return ctm_handling_;
  }

  bool UseColorPipeline() const {
    return color_pipeline_enabled_;
  }

  bool PersistentHdrEnabled() const {
    return persistent_hdr_enabled_;
  }

  bool ForceCtmHandlesOffset() const {
    return force_ctm_handles_offset_;
  }

  bool ExternalHdrEnabled() const {
    return external_hdr_enabled_;
  }

  int ForceColorMode() const {
    return force_color_mode_;
  }

  auto &GetMainLock() {
    return main_lock_;
  }

  auto GetVirtualDisplayPipeline() -> std::shared_ptr<DrmDisplayPipeline>;
  // Requires GetMainLock() to be held by the caller.
  auto GetWritebackConnectorsCount() -> uint32_t;
  auto GetInternalDisplayNames() -> const std::set<std::string>&;
  std::optional<std::string> DumpBackends();
  void FlushDeferredHotplug();

  static auto GetTimeMonotonicNs() -> int64_t;

 private:
  friend struct DrmHwcTest;
  friend struct HwcDisplayTest;

  void ReloadProperties();

  auto CreateDrmDevice(const std::string &path, uint32_t index)
      -> std::unique_ptr<DrmDevice>;

  auto GetOrderedConnectors() -> std::vector<DrmConnector *>;
  void StartUEventListener();
  void StartUEventListener(std::unique_ptr<UEvent> uevent);
  bool HandleUEventHotplug(const UEventHotplugEvent &event);
  bool UpdateFrontendDisplays(
      bool update_modes = true,
      const std::set<DrmConnector *> *scoped_status_connectors = nullptr,
      const std::set<DrmConnector *> *content_protection_updated_connectors =
          nullptr);
  void ProcessHotplugForConnector(DrmConnector *conn, bool update_modes = true,
                                  bool refresh_status_props = true,
                                  bool check_status_props = true,
                                  bool cp_was_updated = false);
  void DetachConnector(DrmConnector *conn, bool update_modes = true);
  void DetachStalePipelines(
      const std::vector<std::unique_ptr<DrmConnector>> &stale_connectors);
  void DetachAllFrontendDisplays();
  void MaybeScheduleDelayedEdidRecovery();
  void CancelDelayedEdidRecovery();
  void ScheduleDeferredHotplugSettle(const UEventHotplugEvent &event);
  void CancelDeferredHotplugSettle();

  std::vector<std::unique_ptr<DrmDevice>> drms_;
  std::set<std::string> displays_;

  // Android properties:
  bool scale_with_gpu_{};
  CtmHandling ctm_handling_{};
  bool color_pipeline_enabled_{};
  bool force_ctm_handles_offset_{};
  int force_color_mode_{};
  bool persistent_hdr_enabled_{};
  bool external_hdr_enabled_{};

  std::shared_ptr<UEventListener> uevent_listener_;

  // Serializes Init() and DeInit() across threads.
  // Lock hierarchy: acquire init_mutex_ -> hotplug_mutex_ -> main_lock_.
  std::mutex init_mutex_;

  // Serializes off-lock DRM connector probing during hotplugs.
  // Lock hierarchy: acquire hotplug_mutex_ before main_lock_.
  std::mutex hotplug_mutex_;
  std::mutex main_lock_;

  using TileGroupKey = std::pair<const DrmDevice *, int>;
  static auto GetConnectorTileGroupKey(DrmConnector *conn)
      -> std::optional<TileGroupKey>;
  auto FindAttachedSiblingTileConnector(DrmConnector *conn) const
      -> DrmConnector *;

  std::map<DrmConnector *, std::shared_ptr<DrmDisplayPipeline>>
      attached_pipelines_;
  std::map<DrmConnector *, TileGroupKey> attached_tile_groups_;
  struct DeferredHotplugEvent {
    std::set<uint32_t> connector_ids;
    bool refresh_all = false;
  };
  std::optional<DeferredHotplugEvent> deferred_bootanim_hotplug_;

  // Test-only override for DrmDevice creation; null in production.
  std::function<std::unique_ptr<DrmDevice>(const std::string &, uint32_t)>
      device_factory_hook_;

  PipelineToFrontendBindingInterface *const frontend_interface_;

  // Delayed-EDID recovery thread state
  std::thread edid_recovery_thread_;
  std::mutex edid_recovery_mutex_;
  std::condition_variable edid_recovery_cv_;
  bool edid_recovery_pending_{};

  // Deferred-hotplug settle reprobe thread state
  std::mutex deferred_settle_lifecycle_mutex_;
  std::thread deferred_settle_thread_;
  std::mutex deferred_settle_mutex_;
  std::condition_variable deferred_settle_cv_;
  bool deferred_settle_cancelled_{};
  bool deferred_settle_pending_{};

  std::atomic<bool> initialized_{false};
  bool deinitializing_{};
};

}  // namespace android::drm_hwcomposer
