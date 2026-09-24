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
#include <memory>
#include <optional>
#include <string>

#include "backend/Backend.h"
#include "backend/sdm/SdmHotplugHandler.h"
#include "backend/sdm/SdmToConnectorMapper.h"
#include "drm/DrmDisplayPipeline.h"

namespace sdm {
class SocketHandler;
class DebugCallbackIntf;
class HWCBufferAllocator;
class HWCSocketHandler;
class SDMCompositorCbIntf;
class SDMDisplayCapsIntf;
class SDMDisplayDrawCycleIntf;
class SDMDisplayLayerBuilderIntf;
class SDMDisplaySettingsIntf;
class SDMDisplaySideBandIntf;
class SDMDisplayLifeCycleIntf;
class SDMSideBandCompositorCbIntf;
}  // namespace sdm

namespace android::drm_hwcomposer {

// SdmBackend interfaces with Qualcomm's SDM library to implement a
// drm_hwcomposer Backend.
class SdmBackend : public Backend {
 public:
  using SdmDisplayId = uint64_t;

  explicit SdmBackend(DrmDevice& drm);
  ~SdmBackend() override;
  std::unique_ptr<DrmDisplayPipeline> CreatePipeline(
      DrmConnector& connector) override;
  std::unique_ptr<BufferInfoGetter> CreateBufferInfoGetter() override;
  std::unique_ptr<AtomicCommitSink> CreateAtomicCommitSink() override;
  std::optional<std::string> Dump() override;

  bool UseBackendHotplug() const override {
    return true;
  }

  void SetRefreshCallbackForConnector(uint32_t connector_id,
                                      RefreshCallback callback) override;
  void SetHotplugHandler(HotplugHandler handler) override;

  static bool Init();

 private:
  static std::optional<SdmDisplayId> GetDisplayIdForConnector(
      const DrmConnector& connector);
  static std::optional<SdmDisplayId> GetBuiltinDisplayId();

  // Interfaces used to interact with SDM. These are created by SDM and returned
  // to the SdmBackend.
  static std::shared_ptr<sdm::SDMDisplayLifeCycleIntf> life_cycle_intf;
  static std::shared_ptr<sdm::SDMDisplayLayerBuilderIntf> layer_builder_intf;
  static std::shared_ptr<sdm::SDMDisplayCapsIntf> display_caps_intf;
  static std::shared_ptr<sdm::SDMDisplayDrawCycleIntf> draw_cycle_intf;
  static std::shared_ptr<sdm::SDMDisplaySettingsIntf> settings_intf;
  static std::shared_ptr<sdm::SDMDisplaySideBandIntf> sideband_intf;

  // Interfaces that need to be implemented by us and passed to SDM. These are
  // created by the SdmBackend and passed to different SDM interfaces as needed.
  static std::unique_ptr<sdm::HWCBufferAllocator> buffer_allocator;
  static std::unique_ptr<sdm::HWCSocketHandler> socket_handler;
  static std::unique_ptr<sdm::DebugCallbackIntf> debug_callback;
  static std::unique_ptr<sdm::SDMCompositorCbIntf> callback_interface;
  static std::unique_ptr<sdm::SDMSideBandCompositorCbIntf> sideband_callbacks;

  static std::unique_ptr<SdmToConnectorMapper> connector_mapper;
  static SdmHotplugHandler hotplug_handler;
  static bool initialized;
};

}  // namespace android::drm_hwcomposer
