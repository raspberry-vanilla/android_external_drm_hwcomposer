/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <hardware/hwcomposer2.h>

#include <atomic>
#include <optional>
#include <sstream>

#include <ui/GraphicTypes.h>

#include "HwcDisplayConfigs.h"
#include "compositor/DisplayInfo.h"
#include "compositor/FlatteningController.h"
#include "compositor/LayerData.h"
#include "drm/DrmAtomicStateManager.h"
#include "drm/ResourceManager.h"
#include "drm/VSyncWorker.h"
#include "hwc2_device/HwcLayer.h"
#include "stats/CompositionStats.h"

namespace android {

class Backend;
class DrmHwc;

class FrontendDisplayBase {
 public:
  virtual ~FrontendDisplayBase() = default;
};

inline constexpr uint32_t kPrimaryDisplay = 0;

// NOLINTNEXTLINE
class HwcDisplay {
 public:
  enum ConfigError {
    kNone,
    kBadConfig,
    kSeamlessNotAllowed,
    kSeamlessNotPossible
  };

  enum DisplayType { kInternal, kExternal, kVirtual };

  HwcDisplay(hwc2_display_t handle, bool is_virtual, DrmHwc *hwc);
  HwcDisplay(const HwcDisplay &) = delete;
  ~HwcDisplay();

  void SetColorTransformMatrix(
      const std::array<float, 16> &color_transform_matrix);

  /* SetPipeline should be carefully used only by DrmHwcTwo hotplug handlers */
  void SetPipeline(std::shared_ptr<DrmDisplayPipeline> pipeline);

  HWC2::Error CreateComposition(AtomicCommitArgs &a_args);
  std::vector<HwcLayer *> GetOrderLayersByZPos();

  std::string Dump();

  auto GetDisplayName() -> std::string;

  const HwcDisplayConfigs &GetDisplayConfigs() const {
    return configs_;
  }

  // Get the config representing the mode that has been committed to KMS.
  auto GetCurrentConfig() const -> const HwcDisplayConfig *;

  // Get the config that was last requested through SetActiveConfig and similar
  // functions. This may differ from the GetCurrentConfig if the config change
  // is queued up to take effect in the future.
  auto GetLastRequestedConfig() const -> const HwcDisplayConfig *;

  // Set a config synchronously. If the requested config fails to be committed,
  // this will return with an error. Otherwise, the config will have been
  // committed to the kernel on successful return.
  ConfigError SetConfig(hwc2_config_t config);

  // Queue a configuration change to take effect in the future.
  auto QueueConfig(hwc2_config_t config, int64_t desired_time, bool seamless,
                   QueuedConfigTiming *out_timing) -> ConfigError;

  // Get the HwcDisplayConfig, or nullptor if none.
  auto GetConfig(hwc2_config_t config_id) const -> const HwcDisplayConfig *;

  auto GetDisplayBoundsMm() -> std::pair<int32_t, int32_t>;

  // To be called after SetDisplayProperties. Returns an empty vector if the
  // requested layers have been validated, otherwise the vector describes
  // the requested composition type changes.
  using ChangedLayer = std::pair<ILayerId, HwcLayer::CompositionType>;
  auto ValidateStagedComposition() -> std::vector<ChangedLayer>;

  // Mark previously validated properties as ready to present.
  auto AcceptValidatedComposition() -> void;

  using ReleaseFence = std::pair<ILayerId, SharedFd>;
  // Present previously staged properties, and return fences to indicate when
  // the new content has been presented, and when the previous buffers have
  // been released. If |desired_present_time| is set, ensure that the
  // composition is presented at the closest vsync to that requested time.
  // Otherwise, present immediately.
  auto PresentStagedComposition(std::optional<int64_t> desired_present_time,
                                SharedFd &out_present_fence,
                                std::vector<ReleaseFence> &out_release_fences)
      -> bool;

  // Get the edid bytes for this display. Return an empty vector on error.
  auto GetRawEdid() -> std::vector<uint8_t>;

  // Get the port id that this display is plugged into.
  auto GetPort() -> uint8_t;

  auto SetContentType(ContentType content_type) {
    content_type_ = content_type;
  }

  // Physical displays are either internal or external.
  auto GetDisplayType() -> DisplayType;

  // Enable or disable vsync callbacks.
  void SetVsyncCallbacksEnabled(bool enabled);

  // Enable or disable the display.
  bool SetDisplayEnabled(bool enabled);

  auto GetFrontendPrivateData() -> std::shared_ptr<FrontendDisplayBase> {
    return frontend_private_data_;
  }

  auto SetFrontendPrivateData(std::shared_ptr<FrontendDisplayBase> data) {
    frontend_private_data_ = std::move(data);
  }

  auto CreateLayer(ILayerId new_layer_id) -> bool;
  auto DestroyLayer(ILayerId layer_id) -> bool;

  // HWC2 Hooks - these should not be used outside of the hwc2 device.
  HWC2::Error GetColorModes(uint32_t *num_modes, int32_t *modes);
#if __ANDROID_API__ > 27
  HWC2::Error GetRenderIntents(int32_t mode, uint32_t *outNumIntents,
                               int32_t *outIntents);
  HWC2::Error SetColorModeWithIntent(int32_t mode, int32_t intent);
#endif
  HWC2::Error GetHdrCapabilities(uint32_t *num_types, int32_t *types,
                                 float *max_luminance,
                                 float *max_average_luminance,
                                 float *min_luminance);
  HWC2::Error SetColorMode(int32_t mode);
  HWC2::Error SetColorTransform(const float *matrix, int32_t hint);

  bool IsWritebackSupported();
  bool SetWritebackEnabled(bool enabled);
  SharedFd GetWritebackBufferFence();

  HwcLayer *get_layer(ILayerId layer) {
    auto it = layers_.find(layer);
    if (it == layers_.end())
      return nullptr;
    return &it->second;
  }

  const Backend *backend() const;
  void set_backend(std::unique_ptr<Backend> backend);

  auto GetHwc() {
    return hwc_;
  }

  auto layers() -> std::map<ILayerId, HwcLayer> & {
    return layers_;
  }

  auto &GetPipe() {
    return *pipeline_;
  }

  bool CtmByGpu();

  CompositionStats &total_stats() {
    return total_stats_;
  }

  /* Headless mode required to keep SurfaceFlinger alive when all display are
   * disconnected, Without headless mode Android will continuously crash.
   * Only single internal (primary) display is required to be in HEADLESS mode
   * to prevent the crash. See:
   * https://source.android.com/devices/graphics/hotplug#handling-common-scenarios
   */
  bool IsInHeadlessMode() {
    return !pipeline_;
  }

  void Deinit();

  auto GetFlatCon() {
    return flatcon_;
  }

  auto GetClientLayer() -> HwcLayer & {
    return client_layer_;
  }

  auto &GetWritebackLayer() {
    return writeback_layer_;
  }

  void SetVirtualDisplayResolution(uint16_t width, uint16_t height) {
    virtual_disp_width_ = width;
    virtual_disp_height_ = height;
  }

  auto getDisplayPhysicalOrientation() -> std::optional<PanelOrientation>;

  bool NeedsClientLayerUpdate() const;

 private:
  AtomicCommitArgs CreateModesetCommit(
      const HwcDisplayConfig *config,
      const std::optional<LayerData> &modeset_layer);

  // Sleep the current thread until |present_time| is closest to the next
  // expected vsync time.
  void WaitForPresentTime(int64_t present_time, uint32_t vsync_period_ns);

  uint32_t GetCurrentVsyncPeriodNs() const;

  HwcDisplayConfigs configs_;

  DrmHwc *const hwc_;

  int64_t staged_mode_change_time_{};
  std::optional<uint32_t> staged_mode_config_id_{};

  std::shared_ptr<DrmDisplayPipeline> pipeline_;

  std::unique_ptr<Backend> backend_;
  std::shared_ptr<FlatteningController> flatcon_;

  std::unique_ptr<VSyncWorker> vsync_worker_;
  bool vsync_event_en_{};

  const hwc2_display_t handle_;
  bool is_virtual_;

  std::map<ILayerId, HwcLayer> layers_;
  HwcLayer client_layer_;
  std::unique_ptr<HwcLayer> writeback_layer_;
  uint16_t virtual_disp_width_{};
  uint16_t virtual_disp_height_{};
  int32_t color_mode_{};
  std::shared_ptr<drm_color_ctm> color_matrix_;
  std::shared_ptr<drm_color_ctm> identity_color_matrix_;
  android_color_transform_t color_transform_hint_{};
  bool ctm_has_offset_ = false;
  ContentType content_type_ = ContentType::kNoData;
  Colorspace colorspace_{};
  std::shared_ptr<hdr_output_metadata> hdr_metadata_;

  std::shared_ptr<DrmKmsPlan> current_plan_;

  SharedFd writeback_complete_fence_;

  uint32_t frame_no_ = 0;
  CompositionStats total_stats_;

  void SetColorMatrixToIdentity();

  bool Init();

  HWC2::Error SetHdrOutputMetadata(ui::Hdr hdrType);
  auto GetEdid() -> EdidWrapperUnique & {
    return GetPipe().connector->Get()->GetParsedEdid();
  }

  std::shared_ptr<FrontendDisplayBase> frontend_private_data_;
};

}  // namespace android
