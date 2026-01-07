/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include <xf86drmMode.h>

#include <cstdint>
#include <string>
#include <vector>

#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmProperty.h"
#include "drm/DrmUnique.h"

namespace android::drm_hwcomposer {

class DrmDevice;
class DrmProperty;
class DrmMode;

enum class Colorspace;
enum class PanelOrientation;

class DrmConnector : public PipelineBindable<DrmConnector> {
  friend class FakeDrmConnector;

 public:
  static auto CreateInstance(DrmDevice &dev, uint32_t connector_id,
                             uint32_t index) -> std::unique_ptr<DrmConnector>;

  DrmConnector(const DrmProperty &) = delete;
  DrmConnector &operator=(const DrmProperty &) = delete;

  virtual ~DrmConnector();

  int UpdateEdidProperty();
  auto GetEdidBlob() -> DrmModePropertyBlobUnique;

  auto GetDev() const -> DrmDevice & {
    return *drm_;
  }

  auto GetId() const {
    return connector_->connector_id;
  }

  auto GetIndexInResArray() const {
    return index_in_res_array_;
  }

  virtual uint32_t GetCurrentEncoderId() const {
    return connector_->encoder_id;
  }

  bool SupportsEncoder(DrmEncoder &enc) const;

  bool IsInternal() const;
  bool IsExternal() const;
  bool IsWriteback() const;
  bool IsValid() const;

  std::string GetName() const;

  int UpdateModes();

  bool IsLinkStatusGood();

  void UpdateContentProtection();

  bool IsContentProtectionEnabled() const;

  auto &GetModes() const {
    return modes_;
  }

  auto &GetDpmsProperty() const {
    return dpms_property_;
  }

  auto &GetCrtcIdProperty() const {
    return crtc_id_property_;
  }

  auto &GetEdidProperty() const {
    return edid_property_;
  }

  auto &GetColorspaceProperty() const {
    return colorspace_property_;
  }

  auto GetColorspacePropertyValue(Colorspace c) {
    return colorspace_enum_map_[c];
  }

  auto &GetContentTypeProperty() const {
    return content_type_property_;
  }

  auto &GetContentProtectionProperty() const {
    return content_protection_property_;
  }

  auto &GetMinBpcProperty() const {
    return min_bpc_property_;
  }

  auto &GetHdrOutputMetadataProperty() const {
    return hdr_output_metadata_property_;
  }

  auto &GetHdcpContentTypeProperty() const {
    return hdcp_content_type_property_;
  }

  auto &GetWritebackFbIdProperty() const {
    return writeback_fb_id_property_;
  }

  auto &GetWritebackOutFenceProperty() const {
    return writeback_out_fence_property_;
  }

  auto &GetPanelOrientationProperty() const {
    return panel_orientation_;
  }

  auto IsConnected() const {
    return connector_->connection == DRM_MODE_CONNECTED;
  }

  auto GetMmWidth() const {
    return connector_->mmWidth;
  }

  auto GetMmHeight() const {
    return connector_->mmHeight;
  };

  auto GetPanelOrientation() -> std::optional<PanelOrientation>;

 private:
  DrmConnector(DrmModeConnectorUnique connector, DrmDevice *drm,
               uint32_t index);

  DrmModeConnectorUnique connector_;
  DrmDevice *const drm_;

  auto Init() -> bool;
  auto GetConnectorProperty(const char *prop_name, DrmProperty *property,
                            bool is_optional = false) -> bool;
  auto GetOptionalConnectorProperty(const char *prop_name,
                                    DrmProperty *property) -> bool {
    return GetConnectorProperty(prop_name, property, /*is_optional=*/true);
  }

  const uint32_t index_in_res_array_;

  std::vector<DrmMode> modes_;

  DrmProperty dpms_property_;
  DrmProperty crtc_id_property_;
  DrmProperty edid_property_;
  DrmProperty colorspace_property_;
  DrmProperty content_type_property_;
  DrmProperty content_protection_property_;
  DrmProperty min_bpc_property_;
  DrmProperty hdr_output_metadata_property_;
  DrmProperty hdcp_content_type_property_;

  DrmProperty link_status_property_;
  DrmProperty panel_orientation_;

  DrmProperty writeback_pixel_formats_property_;
  DrmProperty writeback_fb_id_property_;
  DrmProperty writeback_out_fence_property_;

  std::map<Colorspace, uint64_t> colorspace_enum_map_;
  std::map<uint64_t, PanelOrientation> panel_orientation_enum_map_;
};

}  // namespace android::drm_hwcomposer
