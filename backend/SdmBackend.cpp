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

#define LOG_TAG "drmhwc"  // NOLINT(cppcoreguidelines-macro-usage)

#include "SdmBackend.h"

#include <cinttypes>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <cutils/native_handle.h>
#include <ui/GraphicTypes.h>
#include <xf86drmMode.h>

#include <core/sdm_types.h>
#include <private/generic_payload.h>
#include <sdm_compositor_cb_intf.h>
#include <sdm_compositor_sideband_cb_intf.h>
#include <sdm_display_intf_caps.h>
#include <sdm_display_intf_drawcycle.h>
#include <sdm_display_intf_layer_builder.h>
#include <sdm_display_intf_lifecycle.h>
#include <sdm_display_intf_settings.h>
#include <sdm_display_intf_sideband.h>
#include <sdm_interface_factory.h>
// Need to #include <sdm_display_intf_layer_builder.h> (or
// <sdm_interface_factory.h>) first to get the base classes' definitions.
#include <hwc_buffer_allocator.h>
#include <hwc_socket_handler.h>

#include "backend/Backend.h"
#include "backend/BackendDisplayCapabilities.h"
#include "backend/BackendManager.h"
#include "backend/sdm/SdmAtomicStateManager.h"
#include "backend/sdm/SdmCompositionPlanner.h"
#include "backend/sdm/SdmDebugCallback.h"
#include "backend/sdm/SdmHotplugHandler.h"
#include "backend/sdm/SdmToConnectorMapper.h"
#include "backend/sdm/SnapAllocHandle.h"
#include "backend/sdm/sdm_error.h"
#include "bufferinfo/BufferInfoMapperMetadata.h"
#include "bufferinfo/GrallocBufferHandle.h"
#include "compositor/DisplayInfo.h"
#include "compositor/LayerToPlaneJoiningPlan.h"
#include "drm/AtomicStateManager.h"
#include "drm/CommitStatus.h"
#include "drm/DrmAtomicStateManager.h"
#include "drm/DrmConnector.h"
#include "drm/DrmCrtc.h"
#include "drm/DrmDevice.h"
#include "drm/DrmEncoder.h"
#include "drm/DrmFbImporter.h"
#include "drm/DrmPlane.h"
#include "drm/ResourceManager.h"
#include "hwc/HwcDisplay.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

using sdm_error::ErrorToString;

// Define static members
std::shared_ptr<sdm::SDMDisplayLifeCycleIntf> SdmBackend::life_cycle_intf;
std::shared_ptr<sdm::SDMDisplayLayerBuilderIntf> SdmBackend::layer_builder_intf;
std::shared_ptr<sdm::SDMDisplayCapsIntf> SdmBackend::display_caps_intf;
std::shared_ptr<sdm::SDMDisplayDrawCycleIntf> SdmBackend::draw_cycle_intf;
std::shared_ptr<sdm::SDMDisplaySettingsIntf> SdmBackend::settings_intf;
std::shared_ptr<sdm::SDMDisplaySideBandIntf> SdmBackend::sideband_intf;

std::unique_ptr<sdm::HWCBufferAllocator> SdmBackend::buffer_allocator;
std::unique_ptr<sdm::HWCSocketHandler> SdmBackend::socket_handler;
std::unique_ptr<sdm::DebugCallbackIntf> SdmBackend::debug_callback;
std::unique_ptr<sdm::SDMCompositorCbIntf> SdmBackend::callback_interface;
std::unique_ptr<sdm::SDMSideBandCompositorCbIntf>
    SdmBackend::sideband_callbacks;

std::unique_ptr<SdmToConnectorMapper> SdmBackend::connector_mapper;
SdmHotplugHandler SdmBackend::hotplug_handler;
bool SdmBackend::initialized = false;

void SdmBackend::SetRefreshCallbackForConnector(uint32_t connector_id,
                                                RefreshCallback callback) {
  if (connector_mapper) {
    connector_mapper->SetRefreshCallback(connector_id, std::move(callback));
  }
}

void SdmBackend::SetHotplugHandler(HotplugHandler handler) {
  bool enable = (handler != nullptr);
  hotplug_handler.SetHotplugHandler(std::move(handler));
  if (life_cycle_intf) {
    life_cycle_intf->RegisterCompositorCallback(callback_interface.get(),
                                                enable);
  }
}

namespace {

// Overrides BufferInfoMapperMetadata::Import so that the buffer_handle_t
// allocated by snapalloc gets imported for use in SDM. The PrimeFdsSharedBase
// handle on the BufferInfo struct can be cast to a SnapAllocHandle in order to
// access the underlying snapalloc::SnapHandle
class SnapAllocBufferInfoGetter : public BufferInfoMapperMetadata {
 public:
  // Import the buffer_handle_t into this process. The imported buffer_handle_t
  // will be released when the GrallocBufferHandle is destructed.
  std::shared_ptr<GrallocBufferHandle> Import(buffer_handle_t handle) override {
    auto snap_handle = SnapAllocHandle::Create(handle);
    if (snap_handle == nullptr) {
      ALOGE("SnapAllocBufferInfoGetter:: Failed to import buffer handle");
      return nullptr;
    }
    return snap_handle;
  }
};

class SdmDisplayCapabilities : public BackendDisplayCapabilities {
 public:
  SdmDisplayCapabilities(uint64_t sdm_display_id,
                         const SdmAtomicStateManager* state_manager,
                         sdm::SDMDisplayCapsIntf* caps_intf)
      : state_manager_(state_manager),
        caps_intf_(caps_intf),
        sdm_display_id_(sdm_display_id) {
  }

  std::optional<bool> GetHardwareColorTransformOverride() const override {
    return true;
  }

  std::optional<std::vector<ColorMode>> GetColorModeOverrides() const override {
    return state_manager_->GetColorModes();
  }

  std::vector<DrmMode> FilterModes(
      const std::vector<DrmMode>& modes) const override {
    return state_manager_->FilterModes(modes);
  }

  std::optional<std::vector<ui::Hdr>> GetHdrTypesOverride() const override;

 private:
  const SdmAtomicStateManager* state_manager_;
  sdm::SDMDisplayCapsIntf* caps_intf_;
  uint64_t sdm_display_id_;
};

std::optional<std::vector<ui::Hdr>>
SdmDisplayCapabilities::GetHdrTypesOverride() const {
  uint32_t count = 0;
  float max_lum = 0;
  float max_avg_lum = 0;
  float min_lum = 0;

  auto error = caps_intf_->GetHdrCapabilities(sdm_display_id_, &count, nullptr,
                                              &max_lum, &max_avg_lum, &min_lum);
  if (error != sdm::kErrorNone) {
    ALOGE("GetHdrCapabilities failed for display %" PRIu64 ": %s",
          sdm_display_id_, ErrorToString(error).c_str());
    return std::nullopt;
  }

  std::vector<ui::Hdr> types;
  if (count > 0) {
    std::vector<int32_t> sdm_types(count);
    error = caps_intf_->GetHdrCapabilities(sdm_display_id_, &count,
                                           sdm_types.data(), &max_lum,
                                           &max_avg_lum, &min_lum);
    if (error != sdm::kErrorNone) {
      ALOGE("GetHdrCapabilities failed for display %" PRIu64 ": %s",
            sdm_display_id_, ErrorToString(error).c_str());
      return std::nullopt;
    }
    types.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
      types[i] = static_cast<ui::Hdr>(sdm_types[i]);
    }
  }

  ALOGI("SDM HDR Capabilities for display %" PRIu64
        ": max_lum=%f, max_avg_lum=%f, min_lum=%f, count=%u",
        sdm_display_id_, max_lum, max_avg_lum, min_lum, count);
  for (auto t : types) {
    ALOGI("  type: %d", static_cast<int>(t));
  }

  return types;
}

// TODO: These need to be plumbed back to drm_hwcomposer, or intentionally left
// unimplemented.
class StubCallbackInterface : public sdm::SDMCompositorCbIntf {
 public:
  StubCallbackInterface(SdmToConnectorMapper* connector_mapper,
                        SdmHotplugHandler* hotplug_handler)
      : connector_mapper_(connector_mapper), hotplug_handler_(hotplug_handler) {
  }
  void OnHotplug(uint64_t in_display, bool in_connected) override {
    ALOGV("StubCallbackInterface::OnHotplug(%" PRIu64 ", connected=%d)",
          in_display, in_connected);
    std::optional<uint32_t> connector_id;
    if (in_connected) {
      if (connector_mapper_ != nullptr) {
        connector_id = connector_mapper_->Register(in_display);
      }
    } else {
      if (connector_mapper_ != nullptr) {
        connector_id = connector_mapper_->GetConnectorIdForSdm(in_display);
        connector_mapper_->Unregister(in_display);
      }
    }

    if (connector_id.has_value()) {
      hotplug_handler_->HandleHotplugEvent(connector_id.value(), in_connected);
    } else {
      ALOGE(
          "StubCallbackInterface::OnHotplug: Could not resolve connector id "
          "for sdm display %" PRIu64,
          in_display);
    }
  }

  void OnRefresh(uint64_t in_display) override {
    ALOGV("StubCallbackInterface::OnRefresh(%" PRIu64 ")", in_display);
    if (connector_mapper_ != nullptr) {
      connector_mapper_->OnRefresh(in_display);
    }
  }

  void OnVsync(uint64_t /*in_display*/, int64_t /*in_timestamp*/,
               int32_t /*in_vsync_period_nanos*/) override {
    ALOGV("*STUB* StubCallbackInterface::OnVsync");
  }

  void OnSeamlessPossible(uint64_t /*in_display*/) override {
    ALOGV("*STUB* StubCallbackInterface::OnSeamlessPossible");
  }

  void OnVsyncIdle(uint64_t /*in_display*/) override {
    ALOGV("*STUB* StubCallbackInterface::OnVsyncIdle");
  }

  void OnVsyncPeriodTimingChanged(
      uint64_t /*in_display*/,
      const sdm::SDMVsyncPeriodChangeTimeline& /*timeline*/) override {
    ALOGV("*STUB* StubCallbackInterface::OnVsyncPeriodTimingChanged");
  }

 private:
  SdmToConnectorMapper* connector_mapper_;
  SdmHotplugHandler* hotplug_handler_;
};

// TODO: Remove this once the logspam has been addressed in SDM. These callbacks
// should not be needed.
class StubSideBandCompositorCallbacks
    : public sdm::SDMSideBandCompositorCbIntf {
 public:
  void NotifyQsyncChange(uint64_t /*display_id*/, bool /*qsync_enabled*/,
                         uint32_t /*refresh_rate*/,
                         uint32_t /*qsync_refresh_rate*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::NotifyQsyncChange");
  }

  void NotifyCameraSmoothInfo(sdm::SDMCameraSmoothOp /*op*/,
                              int32_t /*fps*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::NotifyCameraSmoothInfo");
  }

  void NotifyResolutionChange(uint64_t /*display_id*/,
                              sdm::SDMConfigAttributes& /*attr*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::NotifyResolutionChange");
  }

  void NotifyTUIEventDone(uint32_t /*ret*/, uint32_t /*disp_id*/,
                          sdm::SDMTUIEventType /*type*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::NotifyTUIEventDone");
  }

  void NotifyIdleStatus(bool /*status*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::NotifyIdleStatus");
  }

  void NotifyCWBStatus(int32_t /*status*/, void* /*buffer*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::NotifyCWBStatus");
  }

  void NotifyContentFps(const std::string& /*name*/, int32_t /*fps*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::NotifyContentFps");
  }

  // qservice
  void OnHdmiHotplug(bool /*connected*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::OnHdmiHotplug");
  }
  void OnCECMessageReceived(char* /*message*/, int /*len*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::OnCECMessageReceived");
  }

  // gl color convert callbacks
  void InitColorConvert(uint64_t /*display*/, bool /*secure*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::InitColorConvert");
  }
  void ColorConvertBlit(uint64_t /*display*/,
                        sdm::ColorConvertBlitContext* /*ctx*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::ColorConvertBlit");
  }
  void ResetColorConvert(uint64_t /*display*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::ResetColorConvert");
  }
  void DestroyColorConvert(uint64_t /*display*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::DestroyColorConvert");
  }

  // Histogram callbacks
  void StartHistogram(uint64_t /*display*/, int /*max_frames*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::StartHistogram");
  }
  void StopHistogram(uint64_t /*display*/, bool /*teardown*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::StopHistogram");
  }
  void NotifyHistogram(uint64_t /*display*/, int /*fd*/, uint64_t /*blob_id*/,
                       uint32_t /*panel_width*/,
                       uint32_t /*panel_height*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::NotifyHistogram");
  }
  std::string DumpHistogram(uint64_t /*display*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::DumpHistogram");
    return "";
  }
  void CollectHistogram(
      uint64_t /*display*/, uint64_t /*max_frames*/, uint64_t /*timestamp*/,
      int32_t /*samples_size*/[NUM_HISTOGRAM_COLOR_COMPONENTS],
      uint64_t* /*samples*/[NUM_HISTOGRAM_COLOR_COMPONENTS],
      uint64_t* /*numFrames*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::CollectHistogram");
  }
  sdm::DisplayError GetHistogramAttributes(
      uint64_t /*display*/, int32_t* /*format*/, int32_t* /*dataspace*/,
      uint8_t* /*supported_components*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::GetHistogramAttributes");
    return sdm::kErrorNone;
  }

  // gl layer stitch
  void StitchLayers(uint64_t /*display*/,
                    sdm::LayerStitchContext* /*params*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::StitchLayers");
  }
  void InitLayerStitch(uint64_t /*display*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::InitLayerStitch");
  }
  void DestroyLayerStitch(uint64_t /*display*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::DestroyLayerStitch");
  }

  sdm::nsecs_t SystemTime(int clock) override {
    ALOGW_IF(clock != sdm::SYSTEM_TIME_MONOTONIC,
             "SystemTime not implemented for clock: %d", clock);
    int64_t time_ns = ResourceManager::GetTimeMonotonicNs();
    return static_cast<sdm::nsecs_t>(time_ns);
  }

  int GetDemuraFilePaths(const sdm::GenericPayload& /*in*/,
                         sdm::GenericPayload* /*out*/) override {
    ALOGV("*STUB* StubSideBandCompositorCallbacks::GetDemuraFilePaths");
    return 0;
  }
};

// SDM imports the framebuffers. There currently isn't a need to access the
// framebuffers directly in the backend or the rest of drmhwc, so simply stub
// out the importer.
class StubFbImporter : public DrmFbImporter {
 public:
  auto GetOrCreateFbId(BufferInfo* /*bo*/)
      -> std::shared_ptr<DrmFbIdHandle> override {
    return nullptr;
  }
  ~StubFbImporter() override = default;
};

// AtomicCommitSink implementation for SDM. Currently does not support
// multi-display configuration and simply forwards the commit to the
// SdmAtomicStateManager.
class SdmAtomicCommitSink : public AtomicCommitSink {
 public:
  SdmAtomicCommitSink() = default;
  ~SdmAtomicCommitSink() override = default;

  CommitStatus TestAtomicCommit(
      const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>& args)
      const override {
    if (args.size() > 1) {
      ALOGW("Multi-display test commit not supported");
      return CommitStatus::InternalFailure();
    }
    if (args.empty()) {
      return CommitStatus::Success();
    }

    auto [state_manager, commit_args] = args[0];

    // Since we can't make a full atomic test commit for the SDM backend yet,
    // try to identify when drm_hwcomposer is probing for seamless config
    // changes.
    if (commit_args.display_mode.has_value() && commit_args.seamless) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
      auto* sdm_atomic_state_manager = static_cast<SdmAtomicStateManager*>(
          state_manager);
      if (sdm_atomic_state_manager->IsSeamlessConfigChange(
              *commit_args.display_mode)) {
        return CommitStatus::Success();
      }
      return CommitStatus::InternalFailure();
    }

    ALOGE(
        "SdmAtomicCommitSink::TestAtomicCommit: only seamless config change "
        "test is supported.");
    return CommitStatus::InternalFailure();
  }

  CommitStatusOr<
      std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>>
  ExecuteAtomicCommit(
      const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>& args)
      override {
    std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>> results;
    ALOGW_IF(args.size() > 1, "Multi-display atomic commit not supported");
    for (auto [state_manager, commit_args] : args) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-type-static-cast-downcast)
      auto* sdm_atomic_state_manager = static_cast<SdmAtomicStateManager*>(
          state_manager);
      auto result = sdm_atomic_state_manager->ExecuteAtomicCommit(commit_args);
      if (!result.has_value()) {
        ALOGE("Failed to execute commit");
        return CommitStatusOr<
            std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>>(
            CommitStatus::InternalFailure());
      }
      results.emplace_back(state_manager, result.value());
    }
    return CommitStatusOr<
        std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>>(
        results);
  }
};

// Returns true if a primary plane was found and bound to the pipeline, false
// otherwise.
bool BindPrimaryPlane(DrmDisplayPipeline* pipeline) {
  // Find planes that are compatible with the crtc.
  std::vector<DrmPlane*> compatible_planes;
  for (const auto& plane : pipeline->device->GetPlanes()) {
    if (plane->IsCrtcSupported(*pipeline->crtc->Get())) {
      compatible_planes.push_back(plane.get());
    }
  }
  if (compatible_planes.empty()) {
    ALOGE("Couldn't find any compatible planes for crtc %d",
          pipeline->crtc->Get()->GetId());
    return false;
  }

  // Find an unbound primary plane and bind it.
  for (auto& plane : compatible_planes) {
    if (plane->GetType() != DRM_PLANE_TYPE_PRIMARY) {
      continue;
    }
    pipeline->primary_plane = plane->BindPipeline(pipeline);
    if (pipeline->primary_plane != nullptr) {
      return true;
    }
  }
  return false;
}

// Find the encoder currently associated with this connector. If there is no
// such encoder, pick the first compatible one.
DrmEncoder* GetEncoderForConnector(const DrmConnector& connector) {
  auto encoder_id = connector.GetCurrentEncoderId();
  ALOGI_IF(encoder_id == 0,
           "No current encoder for connector. Will use the first one.");

  for (const auto& enc : connector.GetDev().GetEncoders()) {
    if (enc->GetId() == encoder_id) {
      return enc.get();
    }
    // Skip encoder that is already bound.
    if (enc->GetPipeline() != nullptr) {
      continue;
    }
    // Use the first compatible one.
    if (encoder_id == 0 && connector.SupportsEncoder(*enc)) {
      return enc.get();
    }
  }
  return nullptr;
}

// Find the crtc currently associated with this encoder. If there is no such
// crtc, pick the first compatible one.
DrmCrtc* GetCrtcForEncoder(DrmDevice& device, DrmEncoder& encoder) {
  auto crtc_id = encoder.GetCurrentCrtcId();
  ALOGI_IF(crtc_id == 0, "No current crtc for encoder. Get first one.");
  for (const auto& crtc : device.GetCrtcs()) {
    if (crtc->GetId() == crtc_id) {
      return crtc.get();
    }
    // Skip crtc that is already bound.
    if (crtc->GetPipeline() != nullptr) {
      continue;
    }
    if (crtc_id == 0 && encoder.SupportsCrtc(*crtc)) {
      return crtc.get();
    }
  }
  return nullptr;
}

constexpr const char* kDriverName = "msm_drm";

}  // namespace

// Default base class initialization will associate this backend with the
// msm_drm drm device.
SdmBackend::SdmBackend(DrmDevice& drm) : Backend(drm) {
}

SdmBackend::~SdmBackend() = default;

bool SdmBackend::Init() {
  if (initialized) {
    ALOGI("SdmBackend already initialized. Skipping SDM init.");
    return true;
  }
  ALOGI("Initializing SdmBackend");

  sdm::SDMInterfaceFactory* factory = sdm::GetSDMInterfaceFactory();
  if (factory == nullptr) {
    ALOGE("Failed to get SdmInterfaceFactoryV2");
    return false;
  }
  // Create all the required interfaces from the SDMInterfaceFactory.
  display_caps_intf = factory->CreateCapsIntf();
  if (display_caps_intf == nullptr) {
    ALOGE("CreateCapsIntf failed");
    return false;
  }
  draw_cycle_intf = factory->CreateDrawCycleIntf();
  if (draw_cycle_intf == nullptr) {
    ALOGE("CreateDrawCycleIntf failed");
    return false;
  }
  layer_builder_intf = factory->CreateLayerBuilderIntf();
  if (layer_builder_intf == nullptr) {
    ALOGE("CreateLayerBuilderIntf failed");
    return false;
  }
  settings_intf = factory->CreateSettingsIntf();
  if (settings_intf == nullptr) {
    ALOGE("CreateSettingsIntf failed");
    return false;
  }
  life_cycle_intf = factory->CreateLifeCycleIntf();
  if (life_cycle_intf == nullptr) {
    ALOGE("CreateLifeCycleIntf failed");
    return false;
  }
  sideband_intf = factory->CreateSideBandIntf();
  if (sideband_intf == nullptr) {
    ALOGE("CreateSideBandIntf failed");
    return false;
  }

  // Instantiate the required interfaces needed for initializing the interfaces
  // that were created above.
  buffer_allocator = std::make_unique<sdm::HWCBufferAllocator>();
  socket_handler = std::make_unique<sdm::HWCSocketHandler>();
  debug_callback = std::make_unique<SdmDebugCallback>();
  connector_mapper = std::make_unique<SdmToConnectorMapper>(
      display_caps_intf.get());
  callback_interface = std::make_unique<
      StubCallbackInterface>(connector_mapper.get(), &hotplug_handler);
  sideband_callbacks = std::make_unique<StubSideBandCompositorCallbacks>();

  // Initialize the interface with snapalloc.
  if (!SnapAllocHandle::Init(debug_callback.get())) {
    ALOGE("Failed to Initialize SnapMapper");
    return false;
  }

  // TODO: Figure out a better way to do this.
  BufferInfoGetter::Init(std::make_unique<SnapAllocBufferInfoGetter>());

  // Initialize the lifecycle interface.
  auto display_error = life_cycle_intf->Init(buffer_allocator.get(),
                                             socket_handler.get(),
                                             debug_callback.get());
  if (display_error != sdm::kErrorNone) {
    ALOGE("lifecycleintf Init failed with error %s",
          ErrorToString(display_error).c_str());
    return false;
  }
  life_cycle_intf->RegisterSideBandCallback(sideband_callbacks.get(), true);

  ALOGI("Finished initializing SdmBackend");
  initialized = true;
  return true;
}

std::unique_ptr<DrmDisplayPipeline> SdmBackend::CreatePipeline(
    DrmConnector& connector) {
  ALOGI("SdmBackend::CreatePipeline: %s", connector.GetName().c_str());

  // Don't create pipelines for virtual connectors.
  // NOLINTNEXTLINE(abseil-string-find-str-contains)
  if (connector.GetName().find("Virtual-") != std::string::npos) {
    ALOGI("Found virtual connector. Returning null pipeline.");
    return nullptr;
  }

  std::optional<SdmDisplayId> sdm_display_id = GetDisplayIdForConnector(
      connector);
  if (sdm_display_id == std::nullopt) {
    ALOGE("Failed to find built-in display id.");
    return nullptr;
  }

  // Update the connector properties.
  connector.UpdateModesAndProperties();

  // Find DRM resources associated with this pipeline.
  DrmEncoder* encoder = GetEncoderForConnector(connector);
  if (encoder == nullptr) {
    ALOGE("Failed to get encoder for connector %s.",
          connector.GetName().c_str());
    return nullptr;
  }

  DrmCrtc* crtc = GetCrtcForEncoder(connector.GetDev(), *encoder);
  if (crtc == nullptr) {
    ALOGE("Couldn't find crtc for connector %s.", connector.GetName().c_str());
    return nullptr;
  }

  // Use the same StubFbImporter for all pipelines.
  static StubFbImporter fb_importer;

  // Initialize the DrmDisplayPipeline and bind drm resources to the pipeline.
  auto pipe = std::make_unique<DrmDisplayPipeline>();
  pipe->device = &connector.GetDev();
  pipe->importer = &fb_importer;
  pipe->connector = connector.BindPipeline(pipe.get());
  pipe->crtc = crtc->BindPipeline(pipe.get());
  if (pipe->connector == nullptr || pipe->crtc == nullptr) {
    ALOGE("Failed to bind connector or crtc");
    return nullptr;
  }

  // Bind a primary plane.
  // TODO: SDM makes the decision as to which planes to use, and there is no way
  // to query this.
  //       Picking an arbitrary plane seems to work for an initial
  //       implementation.
  if (!BindPrimaryPlane(pipe.get())) {
    ALOGE("Failed to bind primary plane for pipeline");
    return nullptr;
  }

  // Initialize the layer builder for this display.
  auto display_error = layer_builder_intf->Init(buffer_allocator.get(),
                                                sdm_display_id.value());
  if (display_error != sdm::kErrorNone) {
    ALOGE("layer_builder_intf Init failed with error %s",
          ErrorToString(display_error).c_str());
    return nullptr;
  }

  // Construct backend implementations.
  auto state_manager = std::make_unique<
      SdmAtomicStateManager>(sdm_display_id.value(), life_cycle_intf.get(),
                             draw_cycle_intf.get(), settings_intf.get());
  pipe->capabilities = std::make_unique<
      SdmDisplayCapabilities>(sdm_display_id.value(), state_manager.get(),
                              display_caps_intf.get());
  pipe->atomic_state_manager = std::move(state_manager);
  pipe->planner = std::make_unique<
      SdmCompositionPlanner>(sdm_display_id.value(), layer_builder_intf.get(),
                             draw_cycle_intf.get());
  return pipe;
}

std::unique_ptr<AtomicCommitSink> SdmBackend::CreateAtomicCommitSink() {
  return std::make_unique<SdmAtomicCommitSink>();
}

std::optional<std::string> SdmBackend::Dump() {
  uint32_t out_size = 0;
  std::string out_dump;
  sideband_intf->Dump(&out_size, nullptr);
  if (out_size == 0) {
    return std::nullopt;
  }
  out_dump.resize(out_size + 1);
  sideband_intf->Dump(&out_size, out_dump.data());
  out_dump.resize(out_size);
  return out_dump;
}

std::unique_ptr<BufferInfoGetter> SdmBackend::CreateBufferInfoGetter() {
  return std::make_unique<SnapAllocBufferInfoGetter>();
}

std::optional<SdmBackend::SdmDisplayId> SdmBackend::GetDisplayIdForConnector(
    const DrmConnector& connector) {
  if (connector.IsInternal()) {
    return GetBuiltinDisplayId();
  }

  if (connector_mapper) {
    return connector_mapper->GetSdmIdForConnector(connector.GetId());
  }
  return std::nullopt;
}

std::optional<SdmBackend::SdmDisplayId> SdmBackend::GetBuiltinDisplayId() {
  // TODO: query this rather than hardcode.
  constexpr SdmDisplayId kPrimaryDisplayId = 0;
  return kPrimaryDisplayId;
}

// Register the SDM backend for msm_drm
// NOLINTNEXTLINE(cert-err58-cpp,readability-identifier-naming)
static const bool kRegisterSdm = []() {
  BackendManager::GetInstance()
      .Register(kDriverName,
                {
                    .creator = [](DrmDevice& drm) -> std::unique_ptr<Backend> {
                      return std::make_unique<SdmBackend>(drm);
                    },
                    .init = []() -> bool { return SdmBackend::Init(); },
                });
  return true;
}();

}  // namespace android::drm_hwcomposer
