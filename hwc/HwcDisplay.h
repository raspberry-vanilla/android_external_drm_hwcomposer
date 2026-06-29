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

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <ui/ColorSpace.h>

#include "compositor/CompositionPlanner.h"
#include "compositor/DisplayInfo.h"
#include "compositor/ICompositorDisplay.h"
#include "compositor/LayerData.h"
#include "compositor/PresentedCompositionCache.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/drm_mode.h"
#include "hwc/HwcDisplayConfigs.h"
#include "hwc/HwcLayer.h"
#include "utils/BacklightController.h"
#include "utils/EdidWrapper.h"
#include "utils/fd.h"

namespace aidl::android::hardware::graphics::common {
enum class Hdr;
}  // namespace aidl::android::hardware::graphics::common

namespace android::ui {
using aidl::android::hardware::graphics::common::Hdr;
}  // namespace android::ui

namespace android::drm_hwcomposer {

class ChangedLayer;
template <typename T>
class CommitStatusOr;
class DisplayConfigurationResultReporter;
class DisplayHotplugConnectModeDetectedAtomReporter;
class DrmHwc;
class FlatteningController;
class HdcpController;
class VSyncWorker;

struct AtomicCommitArgs;
struct AtomicCommitResult;
struct CommitStatus;
struct CompositionAttributes;
struct CompositionStats;
struct DrmDisplayPipeline;

using DisplayHandle = int64_t;
using EdidWrapperUnique = std::unique_ptr<EdidWrapper>;
using ColorGamut = ::android::ColorSpace;

class FrontendDisplayBase {
 public:
  virtual ~FrontendDisplayBase() = default;
};

inline constexpr uint32_t kPrimaryDisplay = 0;
inline constexpr float kBrightnessUnset = -1;

// NOLINTNEXTLINE
class HwcDisplay : public ICompositorDisplay {
 public:
  enum class Error {
    kNone,
    kBadParameter,
    kUnsupported,
  };

  enum ConfigError {
    kNone,
    kBadConfig,
    kSeamlessNotAllowed,
    kSeamlessNotPossible,
    kConfigFailed
  };

  enum DisplayType { kInternal, kExternal, kVirtual };

  enum class PowerMode {
    kOff,
    kDoze,
    kDozeSuspend,
    kSuspend,
    kOn,
  };

  HwcDisplay(DisplayHandle handle, bool is_virtual, DrmHwc *hwc);
  HwcDisplay(const HwcDisplay &) = delete;
  ~HwcDisplay() override;

  auto GetColorTransformMatrix() const
      -> std::shared_ptr<const HalColorTransforMatrix> override {
    return color_matrix_;
  }

  void SetColorTransformMatrix(
      const HalColorTransforMatrix &color_transform_matrix);

  bool CursorPlaneNeedsColorPipeline(
      const HwcLayer &cursor_layer) const override;

  /* SetPipeline should be carefully used only by DrmHwcTwo hotplug handlers */
  void SetPipeline(std::shared_ptr<DrmDisplayPipeline> pipeline);

  CommitStatus TestComposition(
      CompositionPlanner::ValidatedComposition &composition) const override;

  auto GetLastPresentedComposition() const
      -> const PresentedCompositionCache & override {
    return last_presented_composition_;
  }

  std::vector<const HwcLayer *> GetOrderLayersByZPos() const override;

  std::string Dump();

  auto GetDisplayName() const -> std::string;

  auto GetDisplayConfigs() const -> std::vector<HwcDisplayConfig>;

  // Get the config representing the mode that has been committed to KMS.
  auto GetCurrentConfig() const -> const HwcDisplayConfig *;

  // Get the config that was last requested through SetActiveConfig and similar
  // functions. This may differ from the GetCurrentConfig if the config change
  // is queued up to take effect in the future.
  auto GetLastRequestedConfig() const -> const HwcDisplayConfig *;

  // Get the config that will be active during the next commit. If a config
  // change has been staged, it will be returned iff the scheduled time has
  // arrived. Otherwise the current config will be returned.
  const HwcDisplayConfig *GetNextConfig() const;

  // Set a config synchronously. If the requested config fails to be committed,
  // this will return with an error. Otherwise, the config will have been
  // committed to the kernel on successful return.
  ConfigError SetConfig(ConfigId config);

  // Queues a configuration change to take effect in the future. All queued
  // configurations are seamless.
  auto QueueConfig(ConfigId config, int64_t desired_time,
                   QueuedConfigTiming *out_timing) -> ConfigError;

  // Get the HwcDisplayConfig, or nullptr if none.
  auto GetConfig(ConfigId config_id) const -> const HwcDisplayConfig *;

  auto GetDisplayBoundsMm() -> std::pair<int32_t, int32_t>;

  // To be called after SetDisplayProperties. Returns an empty vector if the
  // requested layers have been validated, otherwise the vector describes
  // the requested composition type changes.
  using ChangedLayer = std::pair<ILayerId, CompositionType>;

  struct ValidateResult {
    // Layers whose composition type was changed my the HWC.
    std::vector<ChangedLayer> changed_layers;
    // Request the client to write transparent pixels where these layers would
    // be.
    std::vector<ILayerId> punch_out_layers;
  };
  auto ValidateStagedComposition() -> ValidateResult;

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
  auto GetPort() const -> uint8_t;

  auto HasBacklight() -> bool {
    return backlight_controller_ != nullptr;
  }

  auto SetBrightness(float brightness) -> bool;

  auto SetContentType(ContentType content_type) {
    content_type_ = content_type;
  }

  // Physical displays are either internal or external.
  auto GetDisplayType() const -> DisplayType;

  // Enable or disable vsync callbacks.
  void SetVsyncCallbacksEnabled(bool enabled);

  bool GetDisplayEnabled() const;

  auto GetFrontendPrivateData() -> std::shared_ptr<FrontendDisplayBase> {
    return frontend_private_data_;
  }

  auto SetFrontendPrivateData(std::shared_ptr<FrontendDisplayBase> data) {
    frontend_private_data_ = std::move(data);
  }

  auto CreateLayer(ILayerId new_layer_id) -> bool;
  auto DestroyLayer(ILayerId layer_id) -> bool;

  auto GetColorModes() -> std::vector<ColorMode>;
  void SetColorMode(ColorMode color_mode);

  void GetHdrCapabilities(std::vector<ui::Hdr> *types, float *max_luminance,
                          float *max_average_luminance, float *min_luminance);

  auto IsHdcpPropertyPresent() -> bool;
  auto StartHdcp() -> bool;
  auto StopHdcp() -> bool;

  bool IsWritebackSupported();
  bool SetWritebackEnabled(bool enabled);
  SharedFd GetWritebackBufferFence();

  HwcLayer *get_layer(ILayerId layer) {
    auto it = layers_.find(layer);
    if (it == layers_.end())
      return nullptr;
    return &it->second;
  }

  auto layers() -> std::map<ILayerId, HwcLayer> & {
    return layers_;
  }

  auto layers() const -> const std::map<ILayerId, HwcLayer> & {
    return layers_;
  }

  const auto &GetPipe() const {
    return *pipeline_;
  }

  auto &GetPipe() {
    return *pipeline_;
  }

  size_t GetNumAvailablePlanes() const override;
  std::shared_ptr<BindingOwner<DrmPlane>> GetCursorPlane() const override;

  bool CtmByGpu() const override;

  bool ForcedScalingWithGpu() const override;

  bool UseColorPipeline() const override;

  const std::map<CompositionAttributes, CompositionStats> &comp_stats() const {
    return comp_stats_;
  }

  /* Headless mode required to keep SurfaceFlinger alive when all display are
   * disconnected, Without headless mode Android will continuously crash.
   * Only single internal (primary) display is required to be in HEADLESS mode
   * to prevent the crash. See:
   * https://source.android.com/devices/graphics/hotplug#handling-common-scenarios
   */
  bool IsInHeadlessMode() const {
    return !pipeline_;
  }

  void Deinit();

  const FlatteningController *GetFlatCon() const override {
    return flatcon_.get();
  }

  const HdcpController *GetHdcpController() const {
    return hdcpcon_.get();
  }

  auto GetClientLayer() -> HwcLayer & {
    return client_layer_;
  }

  const HwcLayer &GetClientLayer() const override {
    return client_layer_;
  }

  auto &GetWritebackLayer() {
    return writeback_layer_;
  }

  void SetVirtualDisplayResolution(uint16_t width, uint16_t height) {
    virtual_disp_width_ = width;
    virtual_disp_height_ = height;
  }

  auto getDisplayPhysicalOrientation() const -> std::optional<PanelOrientation>;

  bool NeedsClientLayerUpdate() const;

  std::pair<uint32_t, uint32_t> GetSize() const override;

  // Enable or disable the display.
  HwcDisplay::Error SetPowerMode(PowerMode mode);

 private:
  bool IsDozeSupported() const;
  bool IsDozeSuspendSupported() const;
  bool IsSuspendSupported() const;

  // Before CreateFrameUpdateCommit() can be called, it must be ensured that
  // the composition's internal states are up to date and ready to create an
  // AtomicCommitArgs.
  void PrepareCompositionForCommit(
      CompositionPlanner::ValidatedComposition &composition) const;

  // Create AtomicCommitArgs to commit at the next vsync. Returns nullopt if
  // such AtomicCommitArgs cannot be created due to lack of drm resources or
  // invalid HwcDisplay or HwcLayer state.
  // The caller must do a test commit on the returned args to ensure that the
  // hardware can perform the commit.
  // PrepareCompositionForCommit() must be called before this function to
  // ensure that the composition's internal states are up to date.
  std::optional<AtomicCommitArgs> CreateFrameUpdateCommit(
      const CompositionPlanner::ValidatedComposition &composition) const;

  // Creates a LayerToPlaneJoiningPlan for the given composition type map.
  std::unique_ptr<LayerToPlaneJoiningPlan> CreateLayerToPlaneJoiningPlan(
      const CompositionPlanner::CompositionTypeMap &composition_types) const;

  CommitStatus CommitStagedComposition(SharedFd &out_present_fence);

  // Update HwcDisplay state tracking to reflect what was committed in |a_args|.
  // This should be called after a successful commit.
  void ApplyCommitChanges(const AtomicCommitArgs &a_args,
                          const AtomicCommitResult &result);

  AtomicCommitArgs CreateModesetCommit(
      const HwcDisplayConfig *config,
      const std::optional<LayerData> &modeset_layer);

  CommitStatusOr<AtomicCommitResult> ExecuteAtomicCommit(
      AtomicCommitArgs &a_args) const;

  // Sleep the current thread until |present_time| is closest to the next
  // expected vsync time.
  void WaitForPresentTime(int64_t present_time, uint32_t vsync_period_ns);

  uint32_t GetCurrentVsyncPeriodNs() const;

  // Returns a client's layer if one was already provided and its size matches
  // the new config, otherwise allocates a new one.
  std::optional<LayerData> GetModesetLayerData(
      const HwcDisplayConfig *new_config);

  // Seamless-tests all configs against the active config for future seamless
  // transitions and update the config groups.
  void SetConfigGroupsForActiveConfig();

  void SetColorMatrixToIdentity();

  bool Init();

  void SetHdrHeadroom();
  void SetHdrOutputMetadata(const ColorGamut &color_gamut,
                            TransferFunction transfer_function);
  void SetOutputType(OutputType hdr_output_type);

  auto GetEdid() const -> const EdidWrapperUnique & {
    return edid_wrapper_;
  }

  void LogModesOnHotplug();
  void LogConfigResult(const AtomicCommitArgs &args, bool success,
                       int64_t duration_ns) const;

  HwcDisplayConfigs configs_;

  DrmHwc *const hwc_;

  EdidWrapperUnique edid_wrapper_ = std::make_unique<EdidWrapper>();

  int64_t staged_mode_change_time_{};
  std::optional<ConfigId> staged_mode_config_id_{};

  std::shared_ptr<DrmDisplayPipeline> pipeline_;

  std::unique_ptr<FlatteningController> flatcon_;
  std::unique_ptr<HdcpController> hdcpcon_;

  std::unique_ptr<VSyncWorker> vsync_worker_;
  bool vsync_event_en_{};

  const DisplayHandle handle_;
  bool is_virtual_;

  std::map<ILayerId, HwcLayer> layers_;
  HwcLayer client_layer_;
  std::unique_ptr<HwcLayer> writeback_layer_;
  uint16_t virtual_disp_width_{};
  uint16_t virtual_disp_height_{};
  std::shared_ptr<HalColorTransforMatrix> color_matrix_;
  std::shared_ptr<HalColorTransforMatrix> identity_color_matrix_;
  bool color_transform_is_identity_{};
  bool ctm_has_offset_ = false;
  ContentType content_type_ = ContentType::kNoData;
  Colorspace colorspace_{};
  TransferFunction transfer_func_{};
  int32_t min_bpc_{};
  std::shared_ptr<hdr_output_metadata> hdr_metadata_;
  float brightness_ = kBrightnessUnset;
  float hdr_headroom_{};

  // Most recent result of ValidateStagedComposition. Must be kept alive until
  // the composition is committed.
  std::optional<CompositionPlanner::ValidatedComposition>
      validated_composition_ = std::nullopt;

  SharedFd writeback_complete_fence_;

  uint32_t frame_no_ = 0;
  std::map<CompositionAttributes, CompositionStats> comp_stats_{};

  std::shared_ptr<FrontendDisplayBase> frontend_private_data_;

  std::unique_ptr<DisplayHotplugConnectModeDetectedAtomReporter>
      display_mode_reporter_;
  std::unique_ptr<DisplayConfigurationResultReporter> config_result_reporter_;

  std::unique_ptr<BacklightController> backlight_controller_;

  PresentedCompositionCache last_presented_composition_;
};

}  // namespace android::drm_hwcomposer
