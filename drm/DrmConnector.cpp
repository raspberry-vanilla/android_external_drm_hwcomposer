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

#include "DrmConnector.h"

#include <drm_mode.h>
#include <xf86drmMode.h>

#include <array>
#include <cerrno>
#include <charconv>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "compositor/DisplayInfo.h"
#include "drm/DrmColorspace.h"
#include "drm/DrmDevice.h"
#include "drm/DrmEncoder.h"
#include "drm/DrmMode.h"
#include "drm/DrmProperty.h"
#include "drm/DrmUnique.h"
#include "utils/log.h"

#ifndef DRM_MODE_CONNECTOR_SPI
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DRM_MODE_CONNECTOR_SPI 19
#endif

#ifndef DRM_MODE_CONNECTOR_USB
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DRM_MODE_CONNECTOR_USB 20
#endif

namespace android::drm_hwcomposer {

constexpr size_t kTypesCount = 21;

auto DrmConnector::GetConnectorProperty(const char *prop_name,
                                        DrmProperty *property,
                                        bool is_optional) -> bool {
  if (IsDeleted()) {
    return false;
  }

  auto err = drm_->GetProperty(GetId(), DRM_MODE_OBJECT_CONNECTOR, prop_name,
                               property);
  if (err == 0)
    return true;

  if (err == -ENODEV) {
    MarkDeleted();
    return false;
  }

  if (is_optional) {
    ALOGV("Could not get optional %s property from connector %d", prop_name,
          GetId());
  } else {
    ALOGE("Could not get %s property from connector %d", prop_name, GetId());
  }
  return false;
}

auto DrmConnector::CreateInstance(DrmDevice &dev, uint32_t connector_id,
                                  uint32_t index)
    -> std::unique_ptr<DrmConnector> {
  auto conn = dev.GetConnector(connector_id);
  if (!conn) {
    const int err = errno;
    if (err == ENOENT || err == ENODEV) {
      ALOGI("Connector %u vanished during probe (%s)", connector_id,
            std::generic_category().message(err).c_str());
    } else {
      ALOGE("Failed to get connector %d (%s)", connector_id,
            std::generic_category().message(err).c_str());
    }
    return {};
  }

  auto c = std::unique_ptr<DrmConnector>(
      new DrmConnector(std::move(conn), &dev, index));

  if (!c->Init()) {
    // If the connector vanished (-ENODEV) during property discovery,
    // DrmDevice::GetProperty() already logged ALOGI and GetConnectorProperty()
    // marked the connector deleted.
    if (!c->IsDeleted()) {
      ALOGE("Failed to initialize connector %d", connector_id);
    }
    return {};
  }

  return c;
}

DrmConnector::~DrmConnector() = default;

DrmConnector::DrmConnector(DrmModeConnectorUnique connector, DrmDevice *drm,
                           uint32_t index)
    : connector_(std::move(connector)),
      drm_(drm),
      connector_id_(connector_ != nullptr ? connector_->connector_id : 0),
      index_in_res_array_(index) {
}

void DrmConnector::PopulateColorspaceEnumMap(
    DrmProperty &colorspace_prop, std::map<DrmColorspace, uint64_t> &enum_map) {
  enum_map.clear();
  colorspace_prop.AddEnumToMap("Default", DrmColorspace::kDefault, enum_map);
  colorspace_prop.AddEnumToMap("SMPTE_170M_YCC", DrmColorspace::kSmpte170MYcc,
                               enum_map);
  colorspace_prop.AddEnumToMap("BT709_YCC", DrmColorspace::kBt709Ycc, enum_map);
  colorspace_prop.AddEnumToMap("XVYCC_601", DrmColorspace::kXvycc601, enum_map);
  colorspace_prop.AddEnumToMap("XVYCC_709", DrmColorspace::kXvycc709, enum_map);
  colorspace_prop.AddEnumToMap("SYCC_601", DrmColorspace::kSycc601, enum_map);
  colorspace_prop.AddEnumToMap("opYCC_601", DrmColorspace::kOpycc601, enum_map);
  colorspace_prop.AddEnumToMap("opRGB", DrmColorspace::kOprgb, enum_map);
  colorspace_prop.AddEnumToMap("BT2020_CYCC", DrmColorspace::kBt2020Cycc,
                               enum_map);
  colorspace_prop.AddEnumToMap("BT2020_RGB", DrmColorspace::kBt2020Rgb,
                               enum_map);
  colorspace_prop.AddEnumToMap("BT2020_YCC", DrmColorspace::kBt2020Ycc,
                               enum_map);
  colorspace_prop.AddEnumToMap("DCI-P3_RGB_D65", DrmColorspace::kDciP3RgbD65,
                               enum_map);
  colorspace_prop.AddEnumToMap("DCI-P3_RGB_Theater",
                               DrmColorspace::kDciP3RgbTheater, enum_map);
  colorspace_prop.AddEnumToMap("RGB_WIDE_FIXED", DrmColorspace::kRgbWideFixed,
                               enum_map);
  colorspace_prop.AddEnumToMap("RGB_WIDE_FLOAT", DrmColorspace::kRgbWideFloat,
                               enum_map);
  colorspace_prop.AddEnumToMap("BT601_YCC", DrmColorspace::kBt601Ycc, enum_map);
}

auto DrmConnector::UpdateColorspaceProperty() -> void {
  colorspace_enum_map_.clear();
  if (GetOptionalConnectorProperty("Colorspace", &colorspace_property_)) {
    PopulateColorspaceEnumMap(colorspace_property_, colorspace_enum_map_);
  }
}

auto DrmConnector::Init() -> bool {
  if (!GetConnectorProperty("DPMS", &dpms_property_) ||
      !GetConnectorProperty("CRTC_ID", &crtc_id_property_)) {
    return false;
  }

  if (IsWriteback()) {
    if (!GetConnectorProperty("WRITEBACK_PIXEL_FORMATS",
                              &writeback_pixel_formats_property_)) {
      ALOGE("Could not get WRITEBACK_PIXEL_FORMATS property");
      return false;
    }

    if (!GetConnectorProperty("WRITEBACK_FB_ID", &writeback_fb_id_property_)) {
      ALOGE("Could not get WRITEBACK_FB_ID property");
      return false;
    }

    if (!GetConnectorProperty("WRITEBACK_OUT_FENCE_PTR",
                              &writeback_out_fence_property_)) {
      ALOGE("Could not get WRITEBACK_OUT_FENCE_PTR property");
      return false;
    }
  }

  UpdateColorspaceProperty();

  GetOptionalConnectorProperty("content type", &content_type_property_);

  GetOptionalConnectorProperty("Content Protection",
                               &content_protection_property_);

  GetOptionalConnectorProperty("HDR_OUTPUT_METADATA",
                               &hdr_output_metadata_property_);

  GetOptionalConnectorProperty("HDCP Content Type",
                               &hdcp_content_type_property_);

  GetOptionalConnectorProperty("min bpc", &min_bpc_property_);

  if (GetOptionalConnectorProperty("panel orientation", &panel_orientation_)) {
    panel_orientation_
        .AddEnumToMapReverse("Normal",
                             PanelOrientation::kModePanelOrientationNormal,
                             panel_orientation_enum_map_);
    panel_orientation_
        .AddEnumToMapReverse("Upside Down",
                             PanelOrientation::kModePanelOrientationBottomUp,
                             panel_orientation_enum_map_);
    panel_orientation_
        .AddEnumToMapReverse("Left Side Up",
                             PanelOrientation::kModePanelOrientationLeftUp,
                             panel_orientation_enum_map_);
    panel_orientation_
        .AddEnumToMapReverse("Right Side Up",
                             PanelOrientation::kModePanelOrientationRightUp,
                             panel_orientation_enum_map_);
  }

  GetOptionalConnectorProperty("PATH", &path_property_);

  GetOptionalConnectorProperty("link-status", &link_status_property_);

  // DrmConnector::CreateInstance() already fetched connector_ via
  // drmModeGetConnector(). Seed modes_ from that initial mode list so
  // ResourceManager::Init() does not have to issue a redundant
  // drmModeGetConnector() EDID probe for every connected display at startup.
  modes_.clear();
  for (int i = 0; i < connector_->count_modes; ++i) {
    drmModeModeInfo raw_mode = *std::next(connector_->modes, i);
    bool exists = false;
    for (const DrmMode &mode : modes_) {
      if (mode == raw_mode) {
        exists = true;
        break;
      }
    }
    if (!exists) {
      modes_.emplace_back(&raw_mode);
    }
  }

  return !IsDeleted();
}

int DrmConnector::UpdateEdidProperty() {
  return GetOptionalConnectorProperty("EDID", &edid_property_) ? 0 : -EINVAL;
}

auto DrmConnector::GetEdidBlob() -> DrmModePropertyBlobUnique {
  auto ret = UpdateEdidProperty();
  if (ret != 0) {
    return {};
  }

  auto blob_id = GetEdidProperty().GetValue();
  if (!blob_id) {
    return {};
  }

  return MakeDrmModePropertyBlobUnique(*drm_->GetFd(), *blob_id);
}

auto DrmConnector::GetTileBlobData() -> std::optional<std::vector<char>> {
  DrmProperty tile_property;
  if (!GetOptionalConnectorProperty("TILE", &tile_property) ||
      !tile_property.IsBlob() || tile_property.GetValue().value_or(0) == 0) {
    return std::nullopt;
  }

  std::vector<char> tile_data;
  if (!tile_property.GetBlobData(tile_data) || tile_data.empty()) {
    return std::nullopt;
  }

  return tile_data;
}

auto DrmConnector::GetTileInfo() -> std::optional<DrmTileInfo> {
  auto tile_data = GetTileBlobData();
  if (!tile_data) {
    return std::nullopt;
  }

  std::string raw_tile(tile_data->data(), tile_data->size());
  while (!raw_tile.empty() && raw_tile.back() == '\0') {
    raw_tile.pop_back();
  }

  std::istringstream ss(raw_tile);
  DrmTileInfo info{};
  int single_monitor = 0;
  char c1 = 0;
  char c2 = 0;
  char c3 = 0;
  char c4 = 0;
  char c5 = 0;
  char c6 = 0;
  char c7 = 0;
  if (!(ss >> info.group_id >> c1 >> single_monitor >> c2 >> info.num_h_tile >>
        c3 >> info.num_v_tile >> c4 >> info.tile_h_loc >> c5 >>
        info.tile_v_loc >> c6 >> info.tile_h_size >> c7 >> info.tile_v_size) ||
      c1 != ':' || c2 != ':' || c3 != ':' || c4 != ':' || c5 != ':' ||
      c6 != ':' || c7 != ':') {
    ALOGW("Connector %s (id=%u): Failed to parse TILE blob '%s'",
          GetName().c_str(), GetId(), raw_tile.c_str());
    return std::nullopt;
  }

  info.tile_is_single_monitor = (single_monitor != 0);
  return info;
}

bool DrmConnector::SupportsEncoder(DrmEncoder &enc) const {
  for (int i = 0; i < connector_->count_encoders; i++) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    if (connector_->encoders[i] == enc.GetId()) {
      return true;
    }
  }

  return false;
}

static bool IsPhysicalInternalConnectorType(uint32_t type) {
  return type == DRM_MODE_CONNECTOR_Unknown ||
         type == DRM_MODE_CONNECTOR_LVDS || type == DRM_MODE_CONNECTOR_eDP ||
         type == DRM_MODE_CONNECTOR_DSI || type == DRM_MODE_CONNECTOR_DPI ||
         type == DRM_MODE_CONNECTOR_SPI;
}

bool DrmConnector::IsInternal() const {
  auto type = connector_->connector_type;
  if (type == DRM_MODE_CONNECTOR_VIRTUAL) {
    if (drm_ != nullptr) {
      for (const auto &conn : drm_->GetConnectors()) {
        if (conn.get() != this &&
            (IsPhysicalInternalConnectorType(conn->GetConnectorType()) ||
             conn->IsExternal())) {
          return false;
        }
      }
    }
    return true;
  }
  return IsPhysicalInternalConnectorType(type);
}

bool DrmConnector::IsExternal() const {
  auto type = connector_->connector_type;
  return type == DRM_MODE_CONNECTOR_HDMIA ||
         type == DRM_MODE_CONNECTOR_DisplayPort ||
         type == DRM_MODE_CONNECTOR_DVID || type == DRM_MODE_CONNECTOR_DVII ||
         type == DRM_MODE_CONNECTOR_VGA || type == DRM_MODE_CONNECTOR_USB;
}

bool DrmConnector::IsWriteback() const {
#ifdef DRM_MODE_CONNECTOR_WRITEBACK
  return connector_->connector_type == DRM_MODE_CONNECTOR_WRITEBACK;
#else
  return false;
#endif
}

uint32_t DrmConnector::GetMmWidth() const {
  return connector_->mmWidth;
}

uint32_t DrmConnector::GetMmHeight() const {
  return connector_->mmHeight;
}

bool DrmConnector::IsValid() const {
  return IsInternal() || IsExternal() || IsWriteback();
}

std::string DrmConnector::GetName() const {
  constexpr std::array<const char *, kTypesCount> kNames =
      {"None",      "VGA",  "DVI-I",     "DVI-D",   "DVI-A", "Composite",
       "SVIDEO",    "LVDS", "Component", "DIN",     "DP",    "HDMI-A",
       "HDMI-B",    "TV",   "eDP",       "Virtual", "DSI",   "DPI",
       "Writeback", "SPI",  "USB"};

  if (connector_->connector_type < kTypesCount) {
    std::ostringstream name_buf;
    name_buf << kNames[connector_->connector_type] << "-"
             << connector_->connector_type_id;
    return name_buf.str();
  }

  ALOGE("Unknown type in connector %d, could not make his name", GetId());
  return "None";
}

auto DrmConnector::ProbeModesAndPropertiesOffLock() -> ProbedState {
  ProbedState probed{};
  const uint32_t id = GetId();
  auto conn = drm_->GetConnector(id);
  if (!conn) {
    const int err = errno;
    if (err == ENOENT || err == ENODEV) {
      ALOGI("Connector %u vanished during probe (%s)", id,
            std::generic_category().message(err).c_str());
    } else {
      ALOGE("Failed to get connector %d (%s)", id,
            std::generic_category().message(err).c_str());
    }
    probed.status = -ENODEV;
    probed.connector_deleted = true;
    return probed;
  }

  for (int i = 0; i < conn->count_modes; ++i) {
    drmModeModeInfo raw_mode = *std::next(conn->modes, i);
    bool exists = false;
    for (const DrmMode &mode : probed.modes) {
      if (mode == raw_mode) {
        exists = true;
        break;
      }
    }

    if (!exists) {
      probed.modes.emplace_back(&raw_mode);
    }
  }

  const int link_err = drm_->GetProperty(id, DRM_MODE_OBJECT_CONNECTOR,
                                         "link-status",
                                         &probed.link_status_property);
  if (link_err == 0) {
    probed.link_status_updated = true;
  } else if (link_err == -ENODEV) {
    probed.connector_deleted = true;
  }

  if (!probed.connector_deleted) {
    const int cp_err = drm_->GetProperty(id, DRM_MODE_OBJECT_CONNECTOR,
                                         "Content Protection",
                                         &probed.content_protection_property);
    if (cp_err == 0) {
      probed.content_protection_updated = true;
    } else if (cp_err == -ENODEV) {
      probed.connector_deleted = true;
    }
  }

  if (!probed.connector_deleted && IsMst()) {
    /* MST virtual connectors aggressively destroy and recreate these DRM
     * properties during link resets. Re probing these here ensures userspace
     * atomic commits aren't accessing stale property values
     */
    probed.mst_properties_updated = true;
    const int cs_err = drm_->GetProperty(id, DRM_MODE_OBJECT_CONNECTOR,
                                         "Colorspace",
                                         &probed.colorspace_property);
    if (cs_err == 0) {
      PopulateColorspaceEnumMap(probed.colorspace_property,
                                probed.colorspace_enum_map);
    } else if (cs_err == -ENODEV) {
      probed.connector_deleted = true;
    }

    if (!probed.connector_deleted) {
      const int hdr_err = drm_->GetProperty(id, DRM_MODE_OBJECT_CONNECTOR,
                                            "HDR_OUTPUT_METADATA",
                                            &probed
                                                 .hdr_output_metadata_property);
      if (hdr_err == -ENODEV) {
        probed.connector_deleted = true;
      }
    }
  }

  probed.connector = std::move(conn);
  if (probed.connector_deleted) {
    probed.status = -ENODEV;
  }
  return probed;
}

auto DrmConnector::ApplyProbedState(ProbedState &&probed) -> int {
  ProbedState state = std::move(probed);
  ++probe_generation_;
  if (state.handled_by_override) {
    return state.status;
  }
  if (state.status != 0 || state.connector_deleted) {
    MarkDeleted();
    if (connector_) {
      connector_->connection = DRM_MODE_DISCONNECTED;
    }
    modes_.clear();
    reported_incompatible_cable_ = false;
    return state.status != 0 ? state.status : -ENODEV;
  }

  deleted_ = false;
  connector_ = std::move(state.connector);
  modes_ = std::move(state.modes);
  if (!IsConnected() || !modes_.empty()) {
    reported_incompatible_cable_ = false;
  }

  if (state.link_status_updated) {
    link_status_property_ = std::move(state.link_status_property);
  }
  if (state.content_protection_updated) {
    content_protection_property_ = std::move(state.content_protection_property);
  }
  if (state.mst_properties_updated) {
    colorspace_property_ = std::move(state.colorspace_property);
    colorspace_enum_map_ = std::move(state.colorspace_enum_map);
    hdr_output_metadata_property_ = std::move(
        state.hdr_output_metadata_property);
  }

  return 0;
}

int DrmConnector::UpdateModesAndProperties() {
  auto probed = DrmConnector::ProbeModesAndPropertiesOffLock();
  return ApplyProbedState(std::move(probed));
}

bool DrmConnector::IsCachedLinkStatusGood() const {
  auto link_status_property_value = link_status_property_.GetValue();
  return !link_status_property_value ||
         *link_status_property_value != DRM_MODE_LINK_STATUS_BAD;
}

bool DrmConnector::IsLinkStatusGood() {
  if (GetOptionalConnectorProperty("link-status", &link_status_property_)) {
    return IsCachedLinkStatusGood();
  }

  return true;
}

bool DrmConnector::UpdateContentProtection() {
  return GetOptionalConnectorProperty("Content Protection",
                                      &content_protection_property_);
}

bool DrmConnector::QueryContentProtectionEnabled() {
  DrmProperty content_protection_property;
  if (!GetOptionalConnectorProperty("Content Protection",
                                    &content_protection_property)) {
    ALOGE("Failed to query Content Protection property for connector %d",
          GetId());
    return false;
  }

  auto content_protection_property_value = content_protection_property
                                               .GetValue();
  return content_protection_property_value &&
         (content_protection_property_value ==
          DRM_MODE_CONTENT_PROTECTION_ENABLED);
}

bool DrmConnector::IsContentProtectionEnabled() const {
  auto content_protection_property_value = content_protection_property_
                                               .GetValue();
  return content_protection_property_value &&
         (content_protection_property_value ==
          DRM_MODE_CONTENT_PROTECTION_ENABLED);
}

std::optional<PanelOrientation> DrmConnector::GetPanelOrientation() {
  if (!panel_orientation_.GetValue().has_value()) {
    ALOGW("No panel orientation property available.");
    return {};
  }

  /* The value_or(0) satisfies the compiler warning. However,
   * panel_orientation_.GetValue() is guaranteed to have a value since we check
   * has_value() and return early otherwise.
   */
  uint64_t panel_orientation_value = panel_orientation_.GetValue().value_or(0);

  if (panel_orientation_enum_map_.count(panel_orientation_value) == 1) {
    return panel_orientation_enum_map_[panel_orientation_value];
  }

  ALOGE("Unknown panel orientation: panel_orientation = %" PRIu64,
        panel_orientation_value);
  return {};
}

/* Returns true if the connector is a Multi-Stream Transport (MST) connector */
bool DrmConnector::IsMst() const {
  // It is a MST connector if the path property is present and has a non-zero
  // value.
  return path_property_.GetId() != 0 &&
         path_property_.GetValue().value_or(0) != 0;
}

std::optional<uint32_t> DrmConnector::GetPhysicalPortBaseId() const {
  if (!IsMst() || !path_property_.IsBlob()) {
    return std::nullopt;
  }

  std::vector<char> path_data;
  if (!path_property_.GetBlobData(path_data) || path_data.empty()) {
    return std::nullopt;
  }

  std::string_view path(path_data.data(), path_data.size());

  // Check if it starts with "mst:"
  if (path.substr(0, 4) != "mst:") {
    return std::nullopt;
  }

  // Find the first dash after "mst:"
  size_t dash_pos = path.find('-', 4);
  if (dash_pos == std::string_view::npos) {
    return std::nullopt;
  }

  std::string_view base_id_str = path.substr(4, dash_pos - 4);
  uint32_t base_id = 0;
  auto [ptr, ec] = std::from_chars(base_id_str.data(),
                                   base_id_str.data() + base_id_str.size(),
                                   base_id);
  if (ec == std::errc() && ptr == base_id_str.data() + base_id_str.size()) {
    return base_id;
  }
  return std::nullopt;
}

}  // namespace android::drm_hwcomposer
