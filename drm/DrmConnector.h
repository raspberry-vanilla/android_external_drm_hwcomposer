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
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "drm/DrmColorspace.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmMode.h"
#include "drm/DrmProperty.h"
#include "drm/DrmUnique.h"

namespace android::drm_hwcomposer {

class DrmDevice;
class DrmProperty;
class DrmMode;

enum class DrmColorspace;
enum class PanelOrientation;

struct DrmTileInfo {
  int group_id = 0;
  bool tile_is_single_monitor = false;
  int num_h_tile = 0;
  int num_v_tile = 0;
  int tile_h_loc = 0;
  int tile_v_loc = 0;
  int tile_h_size = 0;
  int tile_v_size = 0;
};

class DrmConnector : public PipelineBindable<DrmConnector> {
  friend class FakeDrmConnector;

 public:
  static auto CreateInstance(DrmDevice &dev, uint32_t connector_id,
                             uint32_t index) -> std::unique_ptr<DrmConnector>;

  DrmConnector(const DrmProperty &) = delete;
  DrmConnector &operator=(const DrmProperty &) = delete;

  virtual ~DrmConnector();

  int UpdateEdidProperty();
  virtual auto GetEdidBlob() -> DrmModePropertyBlobUnique;

  auto GetTileInfo() -> std::optional<DrmTileInfo>;

  auto GetDev() const -> DrmDevice & {
    return *drm_;
  }

  virtual uint32_t GetId() const {
    return connector_id_;
  }

  auto GetIndexInResArray() const {
    return index_in_res_array_;
  }

  virtual uint32_t GetCurrentEncoderId() const {
    return connector_->encoder_id;
  }

  bool SupportsEncoder(DrmEncoder &enc) const;

  virtual bool IsInternal() const;
  virtual bool IsExternal() const;
  bool IsWriteback() const;
  bool IsValid() const;

  virtual std::string GetName() const;

  struct ProbedState {
    DrmModeConnectorUnique connector;
    std::vector<DrmMode> modes;
    std::map<DrmColorspace, uint64_t> colorspace_enum_map;
    DrmProperty link_status_property;
    DrmProperty content_protection_property;
    DrmProperty colorspace_property;
    DrmProperty hdr_output_metadata_property;
    int status = 0;
    bool handled_by_override = false;
    bool connector_deleted = false;
    bool link_status_updated = false;
    bool content_protection_updated = false;
    bool mst_properties_updated = false;
  };

  virtual int UpdateModesAndProperties();
  virtual auto ProbeModesAndPropertiesOffLock() -> ProbedState;
  auto ApplyProbedState(ProbedState &&probed) -> int;

  uint64_t GetProbeGeneration() const {
    return probe_generation_;
  }

  bool IsLinkStatusGood();
  bool IsCachedLinkStatusGood() const;

  /** Refreshes the cached content protection property value. Returns whether
   * the property was successfully read from DRM. */
  bool UpdateContentProtection();

  /** Reads and returns the content protection property value from DRM. Returns
   * false if the property could not be read. Does not update the cached value.
   */
  virtual bool QueryContentProtectionEnabled();

  /** Returns the cached content protection property value. */
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
  auto &GetCrtcIdProperty() {
    return crtc_id_property_;
  }

  auto &GetEdidProperty() const {
    return edid_property_;
  }

  auto &GetLinkStatusProperty() const {
    return link_status_property_;
  }

  auto &GetColorspaceProperty() const {
    return colorspace_property_;
  }

  auto GetColorspacePropertyValue(DrmColorspace c) {
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

  virtual bool IsConnected() const {
    return connector_->connection == DRM_MODE_CONNECTED;
  }

  virtual bool IsUnknownConnection() const {
    return connector_ != nullptr &&
           connector_->connection == DRM_MODE_UNKNOWNCONNECTION;
  }

  virtual uint32_t GetMmWidth() const;
  virtual uint32_t GetMmHeight() const;

  auto GetPanelOrientation() -> std::optional<PanelOrientation>;

  virtual uint32_t GetConnectorType() const {
    return connector_->connector_type;
  }

  bool IsMst() const;

  void MarkDeleted() {
    deleted_ = true;
  }

  bool IsDeleted() const {
    return deleted_;
  }

  bool HasReportedIncompatibleCable() const {
    return reported_incompatible_cable_;
  }

  void SetReportedIncompatibleCable(bool reported) {
    reported_incompatible_cable_ = reported;
  }

  std::optional<uint32_t> GetPhysicalPortBaseId() const;

  bool IsForceMode(drmModeModeInfo raw_mode, std::string force_mode);

 private:
  DrmConnector(DrmModeConnectorUnique connector, DrmDevice *drm,
               uint32_t index);

  DrmModeConnectorUnique connector_;
  DrmDevice *const drm_;
  const uint32_t connector_id_;

  auto Init() -> bool;
  static void PopulateColorspaceEnumMap(
      DrmProperty &colorspace_prop,
      std::map<DrmColorspace, uint64_t> &enum_map);
  auto UpdateColorspaceProperty() -> void;
  auto GetConnectorProperty(const char *prop_name, DrmProperty *property,
                            bool is_optional = false) -> bool;
  auto GetOptionalConnectorProperty(const char *prop_name,
                                    DrmProperty *property) -> bool {
    return GetConnectorProperty(prop_name, property, /*is_optional=*/true);
  }

  auto GetTileBlobData() -> std::optional<std::vector<char>>;

  const uint32_t index_in_res_array_;
  bool deleted_ = false;
  bool reported_incompatible_cable_ = false;
  uint64_t probe_generation_ = 0;

 protected:
  void IncrementProbeGeneration() {
    ++probe_generation_;
  }

 private:
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
  DrmProperty path_property_;

  DrmProperty link_status_property_;
  DrmProperty panel_orientation_;

  DrmProperty writeback_pixel_formats_property_;
  DrmProperty writeback_fb_id_property_;
  DrmProperty writeback_out_fence_property_;

  std::map<DrmColorspace, uint64_t> colorspace_enum_map_;
  std::map<uint64_t, PanelOrientation> panel_orientation_enum_map_;
};

}  // namespace android::drm_hwcomposer
