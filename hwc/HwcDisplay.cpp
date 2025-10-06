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

#define LOG_TAG "drmhwc"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "HwcDisplay.h"

#include <cinttypes>

#include <ui/ColorSpace.h>
#include <utils/Trace.h>

#include "backend/Backend.h"
#include "backend/BackendManager.h"
#include "compositor/DisplayInfo.h"
#include "drm/DrmConnector.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmHwc.h"
#include "utils/properties.h"

using ColorGamut = ::android::ColorSpace;

namespace android::drm_hwcomposer {

namespace {

constexpr auto kFlatteningTimeout = 1s;
constexpr int kCtmRows = 3;
constexpr int kCtmCols = 3;

bool float_equals(float a, float b) {
  const float epsilon = 0.001F;
  return std::abs(a - b) < epsilon;
}

uint64_t To3132FixPt(float in) {
  constexpr uint64_t kSignMask = (1ULL << 63);
  constexpr uint64_t kValueMask = ~(1ULL << 63);
  constexpr auto kValueScale = static_cast<float>(1ULL << 32);
  if (in < 0)
    return (static_cast<uint64_t>(-in * kValueScale) & kValueMask) | kSignMask;
  return static_cast<uint64_t>(in * kValueScale) & kValueMask;
}

bool TransformHasOffsetValue(const float *matrix) {
  for (int i = 12; i < 14; i++) {
    if (!float_equals(matrix[i], 0.F)) {
      ALOGW("DRM API does not support CTM with offsets.");
      return true;
    }
  }
  return false;
}

auto ToColorTransform(const std::array<float, 16> &color_transform_matrix) {
  /* HAL provides a 4x4 float type matrix:
   * | 0  1  2  3|
   * | 4  5  6  7|
   * | 8  9 10 11|
   * |12 13 14 15|
   *
   * R_out = R*0 + G*4 + B*8 + 12
   * G_out = R*1 + G*5 + B*9 + 13
   * B_out = R*2 + G*6 + B*10 + 14
   *
   * DRM expects a 3x3 s31.32 fixed point matrix:
   * out   matrix    in
   * |R|   |0 1 2|   |R|
   * |G| = |3 4 5| x |G|
   * |B|   |6 7 8|   |B|
   *
   * R_out = R*0 + G*1 + B*2
   * G_out = R*3 + G*4 + B*5
   * B_out = R*6 + G*7 + B*8
   */
  auto color_matrix = std::make_shared<drm_color_ctm>();
  for (int i = 0; i < kCtmCols; i++) {
    for (int j = 0; j < kCtmRows; j++) {
      constexpr int kInCtmRows = 4;
      color_matrix->matrix[(i * kCtmRows) + j] = To3132FixPt(
          color_transform_matrix[(j * kInCtmRows) + i]);
    }
  }
  return color_matrix;
}

}  // namespace

auto HwcDisplay::GetDisplayName() const -> std::string {
  std::ostringstream stream;
  if (IsInHeadlessMode()) {
    stream << "null-display";
  } else {
    stream << "display-" << GetPipe().connector->Get()->GetId();
  }
  return stream.str();
}

auto HwcDisplay::GetDisplayConfigs() const -> std::vector<HwcDisplayConfig> {
  std::vector<HwcDisplayConfig> filtered_configs;
  for (const auto &[_, config] : configs_.hwc_configs) {
    if (config.disabled) {
      continue;
    }

    filtered_configs.emplace_back(config);
  }

  return filtered_configs;
}

HwcDisplay::HwcDisplay(DisplayHandle handle, bool is_virtual, DrmHwc *hwc)
    : hwc_(hwc), handle_(handle), is_virtual_(is_virtual), client_layer_(this) {
  // Create writeback layer for both virtual displays and potential readback
  // operations
  writeback_layer_ = std::make_unique<HwcLayer>(this);

  identity_color_matrix_ = ToColorTransform(kIdentityMatrix);
}

void HwcDisplay::SetColorTransformMatrix(
    const std::array<float, 16> &color_transform_matrix) {
  color_transform_is_identity_ = std::equal(color_transform_matrix.begin(),
                                            color_transform_matrix.end(),
                                            kIdentityMatrix.begin(),
                                            float_equals);
  ctm_has_offset_ = false;

  if (IsInHeadlessMode())
    return;

  if (color_transform_is_identity_) {
    SetColorMatrixToIdentity();
    return;
  }

  ctm_has_offset_ = TransformHasOffsetValue(color_transform_matrix.data());
  if (!ctm_has_offset_) {
    color_matrix_ = ToColorTransform(color_transform_matrix);
  }
}

void HwcDisplay::SetColorMatrixToIdentity() {
  ctm_has_offset_ = false;
  color_matrix_ = identity_color_matrix_;
  color_transform_is_identity_ = true;
}

HwcDisplay::~HwcDisplay() {
  Deinit();
};

auto HwcDisplay::GetConfig(ConfigId config_id) const
    -> const HwcDisplayConfig * {
  auto config_iter = configs_.hwc_configs.find(config_id);
  if (config_iter == configs_.hwc_configs.end()) {
    return nullptr;
  }

  if (config_iter->second.disabled) {
    return nullptr;
  }

  return &config_iter->second;
}

auto HwcDisplay::GetCurrentConfig() const -> const HwcDisplayConfig * {
  return GetConfig(configs_.active_config_id);
}

auto HwcDisplay::GetLastRequestedConfig() const -> const HwcDisplayConfig * {
  return GetConfig(staged_mode_config_id_.value_or(configs_.active_config_id));
}

const HwcDisplayConfig *HwcDisplay::GetNextConfig() const {
  if (staged_mode_config_id_ &&
      staged_mode_change_time_ <= vsync_worker_->GetNextVsyncTimestamp(
                                      ResourceManager::GetTimeMonotonicNs())) {
    return GetLastRequestedConfig();
  }
  return GetCurrentConfig();
}

void HwcDisplay::SetOutputType(OutputType hdr_output_type) {
  if (Properties::DisableHdr()) {
    hdr_metadata_.reset();
    min_bpc_ = 6;
    colorspace_ = Colorspace::kDefault;
    return;
  }
  switch (hdr_output_type) {
    case OutputType::kHdr10: {
      SetHdrOutputMetadata(ui::Hdr::HDR10);
      min_bpc_ = 8;
      colorspace_ = Colorspace::kBt2020Rgb;
      break;
    }
    case OutputType::kSystem: {
      std::vector<ui::Hdr> hdr_types;
      GetEdid()->GetSupportedHdrTypes(hdr_types);
      if (!hdr_types.empty()) {
        SetHdrOutputMetadata(hdr_types.front());
        min_bpc_ = 8;
        colorspace_ = Colorspace::kBt2020Rgb;
        break;
      }
      [[fallthrough]];
    }
    case OutputType::kInvalid:
      [[fallthrough]];
    case OutputType::kSdr:
      [[fallthrough]];
    default:
      hdr_metadata_.reset();
      min_bpc_ = 6;
      colorspace_ = Colorspace::kDefault;
  }
}

HwcDisplay::ConfigError HwcDisplay::SetConfig(ConfigId config) {
  ATRACE_CALL();

  const HwcDisplayConfig *new_config = GetConfig(config);
  if (new_config == nullptr) {
    ALOGE("Could not find active mode for %u", config);
    return ConfigError::kBadConfig;
  }
  if (IsInHeadlessMode()) {
    configs_.active_config_id = config;
    return ConfigError::kNone;
  }

  ALOGV("Create modeset commit.");
  // Allow HDR only on external displays
  if (GetPipe().connector->Get()->IsExternal())
    SetOutputType(new_config->output_type);

  // Create atomic commit args for a blocking modeset. There's no need to do a
  // separate test commit, since the commit does a test anyways.
  std::optional<LayerData> modeset_layer_data = GetModesetLayerData(new_config);
  AtomicCommitArgs commit_args = CreateModesetCommit(new_config,
                                                     modeset_layer_data);
  commit_args.blocking = true;
  if (!GetPipe().atomic_state_manager->ExecuteAtomicCommit(commit_args)) {
    ALOGE("Blocking config failed.");
    return HwcDisplay::ConfigError::kConfigFailed;
  }

  ALOGV("Blocking config succeeded.");
  configs_.active_config_id = config;
  staged_mode_config_id_.reset();
  vsync_worker_->SetVsyncPeriodNs(new_config->mode.GetVSyncPeriodNs());
  // set new vsync period
  return ConfigError::kNone;
}

auto HwcDisplay::QueueConfig(ConfigId config, int64_t desired_time,
                             QueuedConfigTiming *out_timing) -> ConfigError {
  const HwcDisplayConfig *new_config = GetConfig(config);
  if (!new_config) {
    ALOGE("Could not find active mode for %u", config);
    return ConfigError::kBadConfig;
  }

  const HwcDisplayConfig *current_config = GetCurrentConfig();
  if (!current_config || current_config->group_id != new_config->group_id) {
    return ConfigError::kSeamlessNotAllowed;
  }

  // Estimate the timestamp of the next vsync after the desired time.
  int64_t next_vsync = vsync_worker_->GetNextVsyncTimestamp(desired_time);

  // Request a refresh from the client one vsync period before the estimated
  // timestamp.
  out_timing->refresh_time_ns = next_vsync -
                                current_config->mode.GetVSyncPeriodNs();
  out_timing->new_vsync_time_ns = next_vsync;

  // Queue the config change timing to be consistent with the requested
  // refresh time.
  staged_mode_change_time_ = out_timing->refresh_time_ns;
  staged_mode_config_id_ = config;

  // Allow HDR only on external displays
  if (current_config && !IsInHeadlessMode() &&
      GetPipe().connector->Get()->IsExternal()) {
    SetOutputType(current_config->output_type);
  }

  // Enable vsync events until the mode has been applied.
  vsync_worker_->SetVsyncTimestampTracking(true);

  return ConfigError::kNone;
}

auto HwcDisplay::ValidateStagedComposition() -> std::vector<ChangedLayer> {
  if (validated_composition_.has_value()) {
    ALOGE("%s: Previously validated composition was not presented", __func__);
    validated_composition_.reset();
  }

  if (IsInHeadlessMode()) {
    return {};
  }

  if (layers_.empty()) {
    ALOGI("No layers to validate.");
    return {};
  }

  /* In current drm_hwc design in case previous frame layer was not validated as
   * a CLIENT, it is used by display controller (Front buffer). We have to store
   * this state to provide the CLIENT with the release fences for such buffers.
   */
  for (auto &l : layers_) {
    l.second.SetPriorBufferScanOutFlag(l.second.GetValidatedType() !=
                                       CompositionType::kClient);

    /* Populate layer data for layers that might be mapped to a drm plane. */
    if (l.second.GetSfType() == CompositionType::kDevice ||
        l.second.GetSfType() == CompositionType::kCursor) {
      l.second.PopulateLayerData();
    }
  }

  // Notify the flattening controller of a new frame.
  if (layers_.size() <= 1) {
    flatcon_->DisableFlattening();
  } else {
    flatcon_->NewFrame();
  }

  validated_composition_.emplace(backend_->ValidateDisplay(this));

  // Iterate through the layers to find which layers actually changed.
  std::vector<ChangedLayer> changed_layers;
  for (auto &[id, layer] : layers_) {
    // Set the validated type
    auto it = validated_composition_->composition_types.find(&layer);
    ALOGE_IF(it == validated_composition_->composition_types.end(),
             "Backend did not composite layer %" PRId64 "", id);
    if (it != validated_composition_->composition_types.end()) {
      layer.SetValidatedType(it->second);
    }
    if (layer.IsTypeChanged()) {
      changed_layers.emplace_back(id, layer.GetValidatedType());
    }
  }
  return changed_layers;
}

auto HwcDisplay::GetDisplayBoundsMm() -> std::pair<int32_t, int32_t> {
  if (IsInHeadlessMode()) {
    return {configs_.mm_width, -1};
  }

  const auto bounds = GetEdid()->GetBoundsMm();
  if (bounds.first > 0 || bounds.second > 0) {
    return bounds;
  }

  ALOGE("Failed to get display bounds for d=%d\n", int(handle_));
  // mm_width and mm_height are unreliable. so only provide mm_width to avoid
  // wrong dpi computations or other use of the values.
  return {configs_.mm_width, -1};
}

auto HwcDisplay::AcceptValidatedComposition() -> void {
  for (auto &[_, layer] : layers_) {
    layer.AcceptTypeChange();
  }
}

auto HwcDisplay::PresentStagedComposition(
    std::optional<int64_t> desired_present_time, SharedFd &out_present_fence,
    std::vector<ReleaseFence> &out_release_fences) -> bool {
  ATRACE_CALL();

  if (IsInHeadlessMode()) {
    return true;
  }

  if (layers_.empty()) {
    ALOGI("No layers to present.");
    return true;
  }

  ++total_stats_.total_frames;

  // With multiple displays configured at different refresh rates,
  // desired_present_time can be up to almost 2 vsync periods away for the
  // slower display. WaitLastFrame() should be called before
  // WaitForPresenttime(), otherwise  can lead to a situation where hwc sleeps
  // for up to 1.25 vsync period and blocks viable presents in SurfaceFlinger.
  GetPipe().atomic_state_manager->WaitLastFrame();

  uint32_t vperiod_ns = GetCurrentVsyncPeriodNs();
  if (desired_present_time && vperiod_ns != 0) {
    // DRM atomic uAPI does not support specifying that a commit should be
    // applied to some future vsync. Until such uAPI is available, sleep in
    // userspace until the next expected vsync time is consistent with the
    // desired present time.
    WaitForPresentTime(desired_present_time.value(), vperiod_ns);
  }

  // Check if validation was skipped, and populate the composition types as
  // needed. Otherwise, use the already-validated composition types.
  if (!validated_composition_.has_value()) {
    validated_composition_ = Backend::ValidatedComposition{};
    for (auto &l : layers_) {
      validated_composition_->composition_types
          .emplace(&l.second, l.second.GetValidatedType());
    }
  }

  if (!CommitStagedComposition(out_present_fence)) {
    ++total_stats_.failed_kms_present;
    return false;
  }

  // Reset the hdr output metadata blobs so we don't apply it repeatedly.
  hdr_metadata_.reset();

  ++frame_no_;

  if (!out_present_fence) {
    return true;
  }

  vsync_worker_->AddLastPresentFence(out_present_fence);

  for (auto &l : layers_) {
    if (l.second.GetPriorBufferScanOutFlag()) {
      out_release_fences.emplace_back(l.first, out_present_fence);
    }
  }

  return true;
}

auto HwcDisplay::GetRawEdid() -> std::vector<uint8_t> {
  if (IsInHeadlessMode()) {
    return {};
  }

  auto *connector = GetPipe().connector->Get();
  auto blob = connector->GetEdidBlob();
  if (!blob || blob->length == 0) {
    return {};
  }
  const uint8_t *edid_data = static_cast<uint8_t *>(blob->data);
  return {edid_data, edid_data + blob->length};
}

auto HwcDisplay::GetPort() const -> uint8_t {
  if (IsInHeadlessMode()) {
    return 0;
  }

/*
  auto *connector = GetPipe().connector->Get();

  constexpr uint8_t kDrmDeviceBitShift = 5U;
  constexpr uint8_t kDrmDeviceBitMask = 0xE0;
  constexpr uint8_t kConnectorBitMask = 0x1F;
  const auto kDrmIdx = static_cast<uint8_t>(
      connector->GetDev().GetIndexInDevArray());
  const auto kConnectorIdx = static_cast<uint8_t>(
      connector->GetIndexInResArray());
  return (((kDrmIdx << kDrmDeviceBitShift) & kDrmDeviceBitMask) |
          (kConnectorIdx & kConnectorBitMask));
*/
  return handle_; /* TDOD(nobody): What should be here? */
}

auto HwcDisplay::GetDisplayType() -> DisplayType {
  if (is_virtual_) {
    return kVirtual;
  }

  if (IsInHeadlessMode()) {
    return kInternal;
  }

  /* Primary display should be always internal,
   * otherwise SF will be unhappy and will crash
   */
  if (handle_ == kPrimaryDisplay) {
    return kInternal;
  }

  auto displays = hwc_->GetResMan().GetInternalDisplayNames();
  if (!displays.empty()) {
    std::string name = GetPipe().connector->Get()->GetName();
    const bool is_internal = (displays.find(name) != displays.end());
    return is_internal ? kInternal : kExternal;
  }

  if (GetPipe().connector->Get()->IsInternal())
    return kInternal;

  ALOGW_IF(!GetPipe().connector->Get()->IsExternal(),
           "Connector type is neither internal nor external.");
  return kExternal;
}

void HwcDisplay::SetVsyncCallbacksEnabled(bool enabled) {
  // Enabling vsync callbacks for a virtual display succeeds with no effect.
  if (!vsync_worker_) {
    ALOGE_IF(!is_virtual_, "Invalid VSyncWorker. Did HwcDisplay::Init fail?");
    return;
  }

  vsync_event_en_ = enabled;
  std::optional<VSyncWorker::VsyncTimestampCallback> callback = std::nullopt;
  if (vsync_event_en_) {
    DrmHwc *hwc = hwc_;
    DisplayHandle id = handle_;
    // Callback will be called from the vsync thread.
    callback = [hwc, id](int64_t timestamp, uint32_t period_ns) {
      hwc->SendVsyncEventToClient(id, timestamp, period_ns);
    };
  }
  vsync_worker_->SetTimestampCallback(std::move(callback));
}

bool HwcDisplay::SetDisplayEnabled(bool enabled) {
  if (IsInHeadlessMode()) {
    return true;
  }
  if (enabled) {
    /*
     * Setting the display to active before we have a composition
     * can break some drivers, so skip setting a_args.active to
     * true, as the next composition frame will implicitly activate
     * the display
     */
    return GetPipe().atomic_state_manager->ActivateDisplayUsingDPMS() == 0;
  };

  // Disable the display.
  AtomicCommitArgs a_args{};
  a_args.active = false;

  const bool commit_success = GetPipe()
                                  .atomic_state_manager->ExecuteAtomicCommit(
                                      a_args);
  ALOGE_IF(!commit_success, "Failed to apply the dpms composition.");
  return commit_success;
}

void HwcDisplay::SetPipeline(std::shared_ptr<DrmDisplayPipeline> pipeline) {
  Deinit();

  pipeline_ = std::move(pipeline);

  if (pipeline_ != nullptr || handle_ == kPrimaryDisplay) {
    bool success = Init();
    ALOGE_IF(!success, "Failed to init HwcDisplay after setting pipeline.");
    hwc_->ScheduleHotplugEvent(handle_, DrmHwc::kConnected);
  } else {
    hwc_->ScheduleHotplugEvent(handle_, DrmHwc::kDisconnected);
  }
}

void HwcDisplay::Deinit() {
  if (pipeline_ != nullptr) {
    AtomicCommitArgs a_args{};
    a_args.composition = std::make_shared<DrmKmsPlan>();
    GetPipe().atomic_state_manager->ExecuteAtomicCommit(a_args);
    a_args.composition = {};
    a_args.active = false;
    a_args.teardown = true;
    GetPipe().atomic_state_manager->ExecuteAtomicCommit(a_args);

    validated_composition_.reset();
    backend_.reset();
    flatcon_.reset();
  }

  if (vsync_worker_) {
    vsync_worker_->StopThread();
    vsync_worker_ = {};
  }

  client_layer_.ClearSlots();
}

bool HwcDisplay::Init() {
  if (!is_virtual_) {
    vsync_worker_ = VSyncWorker::CreateInstance(pipeline_);
    if (!vsync_worker_) {
      ALOGE("Failed to create event worker for d=%d\n", int(handle_));
      return false;
    }
  }

  if (!IsInHeadlessMode()) {
    auto ret = BackendManager::GetInstance().SetBackendForDisplay(this);
    if (ret) {
      ALOGE("Failed to set backend for d=%d %d\n", int(handle_), ret);
      return false;
    }
    auto flatcbk = (struct FlatConCallbacks){
        .trigger = [this]() { hwc_->SendRefreshEventToClient(handle_); }};
    flatcon_ = std::make_unique<FlatteningController>(flatcbk,
                                                      kFlatteningTimeout);

#if HAS_LIBDISPLAY_INFO
    auto edid = LibdisplayEdidWrapper::Create(
        pipeline_->connector->Get()->GetEdidBlob());
    if (edid) {
      edid_wrapper_ = std::move(edid);
    }
    ALOGW_IF(!edid, "Failed to create a LibdisplayInfo parser.");
#endif
  }

  HwcLayer::LayerProperties lp;
  lp.blend_mode = BufferBlendMode::kPreMult;
  client_layer_.SetLayerProperties(lp);

  SetColorMatrixToIdentity();

  if (is_virtual_) {
    configs_.GenFakeMode(virtual_disp_width_, virtual_disp_height_);
    pipeline_->writeback_connector = pipeline_->connector;
  } else if (IsInHeadlessMode()) {
    configs_.GenFakeMode(0, 0);
  } else if (!configs_.Init(*pipeline_->connector->Get())) {
    return false;
  }

  if (SetConfig(configs_.preferred_config_id) !=
      HwcDisplay::ConfigError::kNone) {
    return false;
  }

  if (!IsInHeadlessMode() && GetPipe().connector->Get()->IsInternal()) {
    SetConfigGroupsForActiveConfig();
  }
  return true;
}

std::optional<PanelOrientation> HwcDisplay::getDisplayPhysicalOrientation()
    const {
  if (IsInHeadlessMode()) {
    // The pipeline can be nullptr in headless mode, so return the default
    // "normal" mode.
    return PanelOrientation::kModePanelOrientationNormal;
  }

  const DrmDisplayPipeline &pipeline = GetPipe();
  if (pipeline.connector == nullptr || pipeline.connector->Get() == nullptr) {
    ALOGW(
        "No display pipeline present to query the panel orientation property.");
    return {};
  }

  return pipeline.connector->Get()->GetPanelOrientation();
}

auto HwcDisplay::CreateLayer(ILayerId new_layer_id) -> bool {
  if (layers_.count(new_layer_id) > 0)
    return false;

  layers_.emplace(new_layer_id, HwcLayer(this));

  return true;
}

auto HwcDisplay::DestroyLayer(ILayerId layer_id) -> bool {
  auto count = layers_.erase(layer_id);
  return count != 0;
}

auto HwcDisplay::GetColorModes() -> std::vector<ColorMode> {
  // disable non-native color modes until tone-mapping is supported
  return {ColorMode::kNative};
}

void HwcDisplay::SetColorMode(ColorMode mode) {
  /* Maps to the Colorspace DRM connector property:
   * https://elixir.bootlin.com/linux/v6.11/source/include/drm/drm_connector.h#L538
   */
  switch (mode) {
    case ColorMode::kNative:
      colorspace_ = Colorspace::kDefault;
      break;
    case ColorMode::kBt601_625:
    case ColorMode::kBt601_625Unadjusted:
    case ColorMode::kBt601_525:
    case ColorMode::kBt601_525Unadjusted:
      // The DP spec does not say whether this is the 525 or the 625 line version.
      colorspace_ = Colorspace::kBt601Ycc;
      break;
    case ColorMode::kBt709:
    case ColorMode::kSrgb:
      colorspace_ = Colorspace::kBt709Ycc;
      break;
    case ColorMode::kDciP3:
    case ColorMode::kDisplayP3:
      colorspace_ = Colorspace::kDciP3RgbD65;
      break;
    case ColorMode::kDisplayBt2020:
    case ColorMode::kAdobeRgb:
    case ColorMode::kBt2020:
    case ColorMode::kBt2100Pq:
    case ColorMode::kBt2100Hlg:
      // HDR color modes should be requested during modeset
      ALOGW("HDR color modes are not supported with this API.");
      return;
  }
}

void HwcDisplay::GetHdrCapabilities(std::vector<ui::Hdr> *types,
                                    float *max_luminance,
                                    float *max_average_luminance,
                                    float *min_luminance) {
  if (IsInHeadlessMode())
    return;

  // Return HDR caps only when we have the ability to set HDR
  DrmDisplayPipeline &pipeline = GetPipe();
  if (pipeline.connector == nullptr || pipeline.connector->Get() == nullptr ||
      !pipeline.connector->Get()->GetHdrOutputMetadataProperty()) {
    return;
  }

  GetEdid()->GetHdrCapabilities(*types, max_luminance, max_average_luminance,
                                min_luminance);
}

AtomicCommitArgs HwcDisplay::CreateModesetCommit(
    const HwcDisplayConfig *config,
    const std::optional<LayerData> &modeset_layer) {
  AtomicCommitArgs args{};

  args.color_matrix = color_matrix_;
  args.content_type = content_type_;
  args.colorspace = colorspace_;
  args.hdr_metadata = hdr_metadata_;
  args.min_bpc = min_bpc_;

  std::vector<LayerData> composition_layers;
  if (modeset_layer) {
    composition_layers.emplace_back(modeset_layer.value());
  }

  if (composition_layers.empty()) {
    ALOGW("Attempting to create a modeset commit without a layer.");
  }

  args.display_mode = config->mode;
  args.active = true;
  args.composition = DrmKmsPlan::CreateDrmKmsPlan(GetPipe(),
                                                  std::move(
                                                      composition_layers));
  ALOGW_IF(!args.composition, "No composition for blocking modeset");

  return args;
}

void HwcDisplay::WaitForPresentTime(int64_t present_time,
                                    uint32_t vsync_period_ns) {
  const int64_t current_time = ResourceManager::GetTimeMonotonicNs();
  int64_t next_vsync_time = vsync_worker_->GetNextVsyncTimestamp(current_time);

  int64_t vsync_after_present_time = vsync_worker_->GetNextVsyncTimestamp(
      present_time);
  int64_t vsync_before_present_time = vsync_after_present_time -
                                      vsync_period_ns;

  // Check if |present_time| is closer to the expected vsync before or after.
  int64_t desired_vsync = (vsync_after_present_time - present_time) <
                                  (present_time - vsync_before_present_time)
                              ? vsync_after_present_time
                              : vsync_before_present_time;

  // Don't sleep if desired_vsync is before or nearly equal to vsync_period of
  // the next expected vsync.
  const int64_t quarter_vsync_period = vsync_period_ns / 4;
  if ((desired_vsync - next_vsync_time) < quarter_vsync_period) {
    return;
  }

  // Sleep until 75% vsync_period before the desired_vsync.
  int64_t sleep_until = desired_vsync - (quarter_vsync_period * 3);

  ATRACE_NAME("WaitForPresentTime");

  // NOLINTBEGIN
  std::stringstream oss;
  oss << "current_time: " << current_time
      << " next_vsync_time: " << next_vsync_time << " (rel "
      << ((next_vsync_time - current_time) / 1000000.00) << "ms)"
      << " desired_vsync: " << desired_vsync << " (rel "
      << ((desired_vsync - current_time) / 1000000.00) << "ms)"
      << " vsync_period_ns: " << vsync_period_ns
      << " sleep_until: " << sleep_until << " (rel "
      << ((sleep_until - current_time) / 1000000.00) << "ms)";
  ATRACE_INSTANT(oss.str().c_str());
  // NOLINTEND

  struct timespec sleep_until_ts{};
  constexpr int64_t kOneSecondNs = 1LL * 1000 * 1000 * 1000;
  sleep_until_ts.tv_sec = int(sleep_until / kOneSecondNs);
  sleep_until_ts.tv_nsec = int(sleep_until -
                               (sleep_until_ts.tv_sec * kOneSecondNs));
  clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &sleep_until_ts, nullptr);
}

uint32_t HwcDisplay::GetCurrentVsyncPeriodNs() const {
  const HwcDisplayConfig *config = GetCurrentConfig();
  if (config == nullptr) {
    return 0;
  }
  return config->mode.GetVSyncPeriodNs();
}

bool HwcDisplay::TestComposition(
    Backend::ValidatedComposition &composition) const {
  ATRACE_CALL();

  if (IsInHeadlessMode()) {
    return true;
  }
  auto a_args = CreateFrameUpdateCommit(composition);
  if (!a_args) {
    return false;
  }
  a_args->test_only = true;
  if (GetPipe().atomic_state_manager->ExecuteAtomicCommit(*a_args)) {
    // Put the composition plan into the newly-validated composition. Its owner
    // is responsible for keeping it alive until commit.
    composition.composition_plan = a_args->composition;
    return true;
  }
  return false;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
std::optional<AtomicCommitArgs> HwcDisplay::CreateFrameUpdateCommit(
    const Backend::ValidatedComposition &composition) const {
  if (IsInHeadlessMode()) {
    ALOGE("%s: Display is in headless mode, should never reach here", __func__);
    return AtomicCommitArgs{};
  }

  AtomicCommitArgs a_args;
  a_args.color_matrix = color_matrix_;
  a_args.content_type = content_type_;
  a_args.colorspace = colorspace_;
  a_args.hdr_metadata = hdr_metadata_;
  a_args.min_bpc = min_bpc_;

  if (staged_mode_config_id_ &&
      staged_mode_change_time_ <= ResourceManager::GetTimeMonotonicNs()) {
    const auto *staged_config = GetConfig(staged_mode_config_id_.value());
    if (staged_config == nullptr) {
      return std::nullopt;
    }

    a_args.display_mode = staged_config->mode;
    a_args.seamless = true;
  }

  // order the layers by z-order
  size_t client_layer_count = 0;
  bool use_client_layer = false;
  uint32_t client_z_order = UINT32_MAX;
  std::map<uint32_t, const HwcLayer *> z_map;
  std::optional<LayerData> cursor_layer = std::nullopt;
  for (const auto &[_, layer] : layers_) {
    auto it = composition.composition_types.find(&layer);
    CompositionType type = it != composition.composition_types.end()
                               ? it->second
                               : CompositionType::kInvalid;
    switch (type) {
      case CompositionType::kDevice:
        z_map.emplace(layer.GetZOrder(), &layer);
        break;
      case CompositionType::kCursor:
        if (!cursor_layer.has_value()) {
          cursor_layer = layer.GetLayerData();
        } else {
          ALOGW("Detected multiple cursor layers");
          z_map.emplace(layer.GetZOrder(), &layer);
        }
        break;
      case CompositionType::kClient:
        // Place it at the z_order of the lowest client layer
        use_client_layer = true;
        client_layer_count++;
        client_z_order = std::min(client_z_order, layer.GetZOrder());
        break;
      case CompositionType::kSolidColor:
      case CompositionType::kInvalid:
        ALOGE("Invalid layer type: %d", static_cast<int>(type));
        continue;
    }
  }

  // CTM will be applied by the client, don't apply DRM CTM
  if (client_layer_count == layers_.size())
    a_args.color_matrix = identity_color_matrix_;
  else
    a_args.color_matrix = color_matrix_;

  if (use_client_layer) {
    z_map.emplace(client_z_order, &client_layer_);
    if (!client_layer_.IsLayerUsableAsDevice()) {
      ALOGE_IF(!a_args.test_only,
               "Client layer must be always usable by DRM/KMS");
      /* This may be normally triggered on validation of the first frame
       * containing CLIENT layer. At this moment client buffer is not yet
       * provided by the CLIENT.
       * This may be triggered once in HwcLayer lifecycle in case FB can't be
       * imported. For example when non-contiguous buffer is imported into
       * contiguous-only DRM/KMS driver.
       */
      return std::nullopt;
    }
  }

  ALOGW_IF(z_map.empty() && !cursor_layer.has_value(), "Empty composition");

  std::vector<LayerData> composition_layers;

  // now that they're ordered by z, add them to the composition
  for (const auto &[_, layer] : z_map) {
    if (!layer->IsLayerUsableAsDevice()) {
      return std::nullopt;
    }
    composition_layers.emplace_back(layer->GetLayerData());
  }

  // Use the provided validated composition plan if it exists, otherwise create
  // it now.
  a_args
      .composition = composition.composition_plan != nullptr
                         ? composition.composition_plan
                         : DrmKmsPlan::CreateDrmKmsPlan(GetPipe(),
                                                        std::move(
                                                            composition_layers),
                                                        cursor_layer);
  if (!a_args.composition) {
    ALOGE_IF(!a_args.test_only, "Failed to create DrmKmsPlan");
    return std::nullopt;
  }

  if (pipeline_->writeback_connector) {
    writeback_layer_->PopulateLayerData();
    if (!writeback_layer_->IsLayerUsableAsDevice()) {
      ALOGE("Writeback layer not usable by DRM/KMS - no valid buffer set");
      return std::nullopt;
    }
    a_args.writeback_fb = writeback_layer_->GetLayerData().fb;
    a_args.writeback_release_fence = writeback_layer_->GetLayerData()
                                         .acquire_fence;
  }
  return a_args;
}

bool HwcDisplay::CommitStagedComposition(SharedFd &out_present_fence) {
  ATRACE_CALL();

  if (IsInHeadlessMode()) {
    ALOGE("%s: Display is in headless mode, should never reach here", __func__);
    return true;
  }

  if (!validated_composition_.has_value()) {
    ALOGE("%s: No composition is staged. Cannot commit.", __func__);
    return false;
  }

  // Client layer needs to be populated after validation since the client may
  // not provide a new buffer until after validation.
  if (std::any_of(validated_composition_->composition_types.begin(),
                  validated_composition_->composition_types.end(),
                  [](const auto &pair) -> bool {
                    return pair.second == CompositionType::kClient;
                  })) {
    client_layer_.PopulateLayerData();
  }

  auto a_args = CreateFrameUpdateCommit(validated_composition_.value());
  // |validated_composition_| can safely be reset now. |a_args| holds its own
  // pointer to the plan which will remain in scope until the commit is finished
  // (successfully or not).
  validated_composition_.reset();

  if (!a_args) {
    ALOGE("Failed to create AtomicCommitArgs for frame composition.");
    return false;
  }

  if (!GetPipe().atomic_state_manager->ExecuteAtomicCommit(*a_args)) {
    ALOGE("Failed to commit the frame composition.");
    return false;
  }
  out_present_fence = a_args->out_fence;
  ApplyCommitChanges(*a_args);
  return true;
}

void HwcDisplay::ApplyCommitChanges(const AtomicCommitArgs &a_args) {
  ALOGE_IF(a_args.test_only, "Applying commit changes for test_only args.");
  writeback_complete_fence_ = a_args.out_writeback_complete_fence;
  if (a_args.display_mode) {
    // Get the vsync period before updating active_config_id.
    uint32_t prev_vperiod_ns = GetCurrentVsyncPeriodNs();
    vsync_worker_->SetVsyncTimestampTracking(false);
    uint32_t last_vsync_ts = vsync_worker_->GetLastVsyncTimestamp();
    if (last_vsync_ts != 0) {
      hwc_->SendVsyncPeriodTimingChangedEventToClient(handle_,
                                                      last_vsync_ts +
                                                          prev_vperiod_ns);
    }

    // If staged_mode_config_id_ is nullopt that indicates a logic error.
    ALOGE_IF(!staged_mode_config_id_,
             "a_args.display_mode is set but staged_mode_config_id_ is not.");
    // Update the active_config_id and update the vsync period for the
    // VsyncWorker.
    configs_.active_config_id = staged_mode_config_id_.value_or(
        configs_.active_config_id);
    staged_mode_config_id_.reset();
    vsync_worker_->SetVsyncPeriodNs(a_args.display_mode->GetVSyncPeriodNs());
  }
}

bool HwcDisplay::CtmByGpu() const {
  if (color_transform_is_identity_)
    return false;

  if (GetPipe().crtc->Get()->GetCtmProperty() && !ctm_has_offset_)
    return false;

  if (hwc_->GetResMan().GetCtmHandling() == CtmHandling::kDrmOrIgnore)
    return false;

  return true;
}

bool HwcDisplay::ForcedScalingWithGpu() const {
  return hwc_->GetResMan().ForcedScalingWithGpu();
}

bool HwcDisplay::IsWritebackSupported() {
  if (IsInHeadlessMode()) {
    return false;
  }

  return !is_virtual_ &&
         pipeline_->FindWritebackConnectorForPipeline() != nullptr;
}

bool HwcDisplay::SetWritebackEnabled(bool enabled) {
  if (IsInHeadlessMode()) {
    return false;
  }

  // Handle Disable
  if (!enabled) {
    pipeline_->writeback_connector = nullptr;
    return true;
  }

  // Handle Enable
  if (pipeline_->writeback_connector != nullptr) {
    return true;
  }

  auto *wb_connector = pipeline_->FindWritebackConnectorForPipeline();
  if (!wb_connector) {
    ALOGE("HwcDisplay: No writeback connector found");
    return false;
  }
  auto bound_connector = wb_connector->BindPipeline(pipeline_.get());
  if (!bound_connector) {
    ALOGE("HwcDisplay: Failed to bind writeback connector");
    return false;
  }
  pipeline_->writeback_connector = bound_connector;
  return true;
}

SharedFd HwcDisplay::GetWritebackBufferFence() {
  if (!writeback_complete_fence_) {
    ALOGE("HwcDisplay: No readback fence available for display");
    return nullptr;
  }

  return std::move(writeback_complete_fence_);
}

std::vector<const HwcLayer *> HwcDisplay::GetOrderLayersByZPos() const {
  std::vector<const HwcLayer *> ordered_layers;
  ordered_layers.reserve(layers_.size());

  for (const auto &[handle, layer] : layers_) {
    ordered_layers.emplace_back(&layer);
  }

  std::sort(std::begin(ordered_layers), std::end(ordered_layers),
            [](const HwcLayer *lhs, const HwcLayer *rhs) {
              // Cursor layers should always have highest zpos.
              if ((lhs->GetSfType() == CompositionType::kCursor) !=
                  (rhs->GetSfType() == CompositionType::kCursor)) {
                return rhs->GetSfType() == CompositionType::kCursor;
              }

              return lhs->GetZOrder() < rhs->GetZOrder();
            });

  return ordered_layers;
}

// Display primary values are coded as unsigned 16-bit values in units of
// 0.00002, where 0x0000 represents zero and 0xC350 represents 1.0000.
static uint64_t ToU16ColorValue(float in) {
  constexpr float kPrimariesFixedPoint = 50000.F;
  return static_cast<uint64_t>(kPrimariesFixedPoint * in);
}

void HwcDisplay::SetHdrOutputMetadata(ui::Hdr type) {
  hdr_metadata_ = std::make_shared<hdr_output_metadata>();
  hdr_metadata_->metadata_type = 0;
  auto *m = &hdr_metadata_->hdmi_metadata_type1;
  m->metadata_type = 0;

  switch (type) {
    case ui::Hdr::HDR10:
      m->eotf = 2;  // PQ
      break;
    case ui::Hdr::HLG:
      m->eotf = 3;  // HLG
      break;
    default:
      ALOGW("HDR type %d is not supported.", static_cast<int>(type));
      return;
  }

  // Most luminance values are coded as an unsigned 16-bit value in units of 1
  // cd/m2, where 0x0001 represents 1 cd/m2 and 0xFFFF represents 65535 cd/m2.
  std::vector<ui::Hdr> types;
  float hdr_luminance[3]{0.F, 0.F, 0.F};
  GetEdid()->GetHdrCapabilities(types, &hdr_luminance[0], &hdr_luminance[1],
                                &hdr_luminance[2]);
  m->max_display_mastering_luminance = m->max_cll = static_cast<uint64_t>(
      hdr_luminance[0]);
  m->max_fall = static_cast<uint64_t>(hdr_luminance[1]);
  // The min luminance value is coded as an unsigned 16-bit value in units of
  // 0.0001 cd/m2, where 0x0001 represents 0.0001 cd/m2 and 0xFFFF
  // represents 6.5535 cd/m2.
  m->min_display_mastering_luminance = static_cast<uint64_t>(hdr_luminance[2] *
                                                             10000.F);

  auto gamut = ColorGamut::BT2020();
  auto primaries = gamut.getPrimaries();
  m->display_primaries[0].x = ToU16ColorValue(primaries[0].x);
  m->display_primaries[0].y = ToU16ColorValue(primaries[0].y);
  m->display_primaries[1].x = ToU16ColorValue(primaries[1].x);
  m->display_primaries[1].y = ToU16ColorValue(primaries[1].y);
  m->display_primaries[2].x = ToU16ColorValue(primaries[2].x);
  m->display_primaries[2].y = ToU16ColorValue(primaries[2].y);

  auto whitePoint = gamut.getWhitePoint();
  m->white_point.x = ToU16ColorValue(whitePoint.x);
  m->white_point.y = ToU16ColorValue(whitePoint.y);
}

const Backend *HwcDisplay::backend() const {
  return backend_.get();
}

void HwcDisplay::set_backend(std::unique_ptr<Backend> backend) {
  backend_ = std::move(backend);
}

bool HwcDisplay::NeedsClientLayerUpdate() const {
  return std::any_of(layers_.begin(), layers_.end(), [](const auto &pair) {
    const auto &layer = pair.second;
    return layer.GetSfType() == CompositionType::kClient ||
           layer.GetValidatedType() == CompositionType::kClient;
  });
}

std::optional<LayerData> HwcDisplay::GetModesetLayerData(
    const HwcDisplayConfig *new_config) {
  const uint32_t new_width = new_config->mode.GetRawMode().hdisplay;
  const uint32_t new_height = new_config->mode.GetRawMode().vdisplay;

  const HwcDisplayConfig *active_config = GetCurrentConfig();
  if (client_layer_.IsLayerUsableAsDevice() && active_config &&
      active_config->mode.GetRawMode().hdisplay == new_width &&
      active_config->mode.GetRawMode().vdisplay == new_height) {
    ALOGV("Use existing client_layer for config.");
    return client_layer_.GetLayerData();
  }

  ALOGV("Allocate modeset buffer.");
  auto modeset_buffer = GetPipe().device->CreateBufferForModeset(new_width,
                                                                 new_height);
  if (!modeset_buffer)
    return std::nullopt;

  auto modeset_layer = std::make_unique<HwcLayer>(this);
  modeset_layer->SetLayerProperties({
      .slot_buffer = std::optional<HwcLayer::Buffer>({
          .slot_id = 0,
          .bi = modeset_buffer,
      }),
      .active_slot = std::optional<HwcLayer::Slot>({
          .slot_id = 0,
          .fence = {},
      }),
      .blend_mode = BufferBlendMode::kNone,
  });
  modeset_layer->PopulateLayerData();

  return modeset_layer->GetLayerData();
}

void HwcDisplay::SetConfigGroupsForActiveConfig() {
  const auto *active_config = GetCurrentConfig();
  if (!active_config) {
    ALOGW("Could not fetch active config for config group assignment.");
    return;
  }

  const std::optional<LayerData> modeset_layer_data = GetModesetLayerData(
      active_config);
  for (auto &[_, config] : configs_.hwc_configs) {
    AtomicCommitArgs commit_args = CreateModesetCommit(&config,
                                                       modeset_layer_data);
    commit_args.test_only = true;
    commit_args.seamless = true;
    if (pipeline_->atomic_state_manager->ExecuteAtomicCommit(commit_args)) {
      config.group_id = active_config->group_id;
    }
  }

  configs_.SanitizeGroups();
}

std::pair<uint32_t, uint32_t> HwcDisplay::GetSize() const {
  const auto *config = GetNextConfig();
  if (config == nullptr) {
    return std::make_pair(0, 0);
  }
  return std::make_pair(config->mode.GetRawMode().hdisplay,
                        config->mode.GetRawMode().vdisplay);
}

}  // namespace android::drm_hwcomposer
