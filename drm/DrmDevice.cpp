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

#include "DrmDevice.h"

#include <asm-generic/int-ll64.h>
#include <asm-generic/mman-common.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "backend/BackendManager.h"
#include "bufferinfo/BufferInfo.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "drm/DrmConnector.h"
#include "drm/DrmCrtc.h"
#include "drm/DrmEncoder.h"
#include "drm/DrmFbImporter.h"
#include "drm/DrmPlane.h"
#include "drm/DrmProperty.h"
#include "drm/DrmUnique.h"
#include "drm/ResourceManager.h"
#include "utils/fd.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android::drm_hwcomposer {

DrmDevice::~DrmDevice() {
  if (GetFd() && drmIsMaster(*GetFd()) != 0) {
    drmDropMaster(*GetFd());
  }
};

auto DrmDevice::CreateInstance(std::string const &path,
                               ResourceManager *res_man, uint32_t index)
    -> std::unique_ptr<DrmDevice> {
  auto device = std::unique_ptr<DrmDevice>(new DrmDevice(res_man, index));

  if (device->Init(path.c_str()) != 0) {
    return {};
  }

  return device;
}

DrmDevice::DrmDevice(ResourceManager *res_man, uint32_t index)
    : index_in_dev_array_(index),
      res_man_(res_man),
      external_displays_enabled_(true) {
  drm_fb_importer_ = std::make_unique<DrmFbCachedImporter>(*this);
}

auto DrmDevice::Init(const char *path) -> int {
  /* TODO: Use drmOpenControl here instead */
  constexpr int kMaxMasterRetries = 20;
  constexpr auto kMasterRetryDelay = std::chrono::milliseconds(50);

  for (int attempt = 1; attempt <= kMaxMasterRetries; ++attempt) {
    // NOLINTNEXTLINE(misc-include-cleaner)
    fd_ = MakeSharedFd(open(path, O_RDWR | O_CLOEXEC));
    if (!fd_) {
      ALOGE("Failed to open dri %s: %s", path,
            std::generic_category().message(errno).c_str());
      return -ENODEV;
    }

    // Check KMS capabilities on the already-open fd rather than opening and
    // closing the device prior to Init(), which would drop the kernel's
    // auto-granted DRM master. Because KMS resource counts are static and do
    // not require DRM master, only check on the first attempt so non-KMS
    // devices fail fast without redundant ioctls on subsequent retries.
    if (attempt == 1 && !IsKMSDev(*GetFd())) {
      return -ENODEV;
    }

    if (index_in_dev_array_ >= kMaxKmsDevices) {
      ALOGE(
          "Reached maximum of %u KMS display devices for 8-bit port encoding; "
          "dropping %s to avoid display port ID collisions.",
          kMaxKmsDevices, path);
      return -ENOSPC;
    }

    // Without CAP_SYS_ADMIN, the kernel only allows drmSetMaster() if the fd
    // was auto-granted master at open() time (file_priv->was_master == true),
    // in which case drmSetMaster() succeeds as a no-op. If another process
    // transiently held DRM master during open(), was_master is false and
    // drmSetMaster() will permanently fail with EACCES on this fd, so we must
    // close and re-open the device on retry.
    if (drmSetMaster(*GetFd()) == 0) {
      break;
    }

    ALOGW("Failed to set drm master on %s (errno=%d, attempt=%d/%d)", path,
          errno, attempt, kMaxMasterRetries);
    if (Properties::IgnoreDrmMasterFailure() || attempt == kMaxMasterRetries) {
      break;
    }

    fd_.reset();
    std::this_thread::sleep_for(kMasterRetryDelay);
  }

  if (drmIsMaster(*GetFd()) == 0) {
    if (!Properties::IgnoreDrmMasterFailure()) {
      ALOGE("DRM/KMS master access required");
      return -EACCES;
    }
    ALOGW("Continuing without DRM/KMS master access");
  }

  int ret = drmSetClientCap(*GetFd(), DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
  if (ret != 0) {
    ALOGE("Failed to set universal plane cap %d", ret);
    return ret;
  }

  ret = drmSetClientCap(*GetFd(), DRM_CLIENT_CAP_ATOMIC, 1);
  if (ret != 0) {
    ALOGE("Failed to set atomic cap %d", ret);
    return ret;
  }

#ifdef DRM_CLIENT_CAP_WRITEBACK_CONNECTORS
  ret = drmSetClientCap(*GetFd(), DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1);
  if (ret != 0) {
    ALOGI("Failed to set writeback cap %d", ret);
  }
#endif

  if (res_man_->UseColorPipeline()) {
    ret = drmSetClientCap(*GetFd(), DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE, 1);
    ALOGW_IF(ret != 0, "Failed to set color pipeline cap %d", ret);
  }

  uint64_t cap_value = 0;
  if (drmGetCap(*GetFd(), DRM_CAP_ADDFB2_MODIFIERS, &cap_value) != 0) {
    ALOGW("drmGetCap failed. Fallback to no modifier support.");
    cap_value = 0;
  }
  HasAddFb2ModifiersSupport_ = cap_value != 0;

  uint64_t cursor_width = 0;
  uint64_t cursor_height = 0;
  if (drmGetCap(*GetFd(), DRM_CAP_CURSOR_WIDTH, &cursor_width) == 0 &&
      drmGetCap(*GetFd(), DRM_CAP_CURSOR_HEIGHT, &cursor_height) == 0) {
    cap_cursor_size_ = std::pair<uint64_t, uint64_t>(cursor_width,
                                                     cursor_height);
  }

  external_displays_enabled_ = Properties::EnableExternalDisplays();

  auto res = MakeDrmModeResUnique(*GetFd());
  if (!res) {
    ALOGE("Failed to get DrmDevice resources");
    return -ENODEV;
  }

  min_resolution_ = std::pair<uint32_t, uint32_t>(res->min_width,
                                                  res->min_height);
  max_resolution_ = std::pair<uint32_t, uint32_t>(res->max_width,
                                                  res->max_height);

  for (int i = 0; i < res->count_crtcs; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto crtc = DrmCrtc::CreateInstance(*this, res->crtcs[i], i);
    if (crtc) {
      crtcs_.emplace_back(std::move(crtc));
    }
  }

  for (int i = 0; i < res->count_encoders; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto enc = DrmEncoder::CreateInstance(*this, res->encoders[i], i);
    if (enc) {
      encoders_.emplace_back(std::move(enc));
    }
  }

  for (int i = 0; i < res->count_connectors; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto conn = DrmConnector::CreateInstance(*this, res->connectors[i]);

    if (!conn) {
      continue;
    }

    if (conn->IsWriteback()) {
      writeback_connectors_.emplace_back(std::move(conn));
    } else if (IsConnectorAllowed(*conn)) {
      connectors_.emplace_back(std::move(conn));
    } else {
      ignored_connectors_.emplace_back(std::move(conn));
    }
  }
  InitializeConnectorPorts();

  auto plane_res = MakeDrmModePlaneResUnique(*GetFd());
  if (!plane_res) {
    ALOGE("Failed to get plane resources");
    return -ENOENT;
  }

  for (uint32_t i = 0; i < plane_res->count_planes; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    auto plane = DrmPlane::CreateInstance(*this, plane_res->planes[i]);
    if (plane) {
      planes_.emplace_back(std::move(plane));
    }
  }

  backend_ = BackendManager::GetInstance().CreateBackendForDevice(*this);
  if (!backend_) {
    ALOGE("Failed to create backend for device %s", GetName().c_str());
    return -ENODEV;
  }
  atomic_commit_sink_ = backend_->CreateAtomicCommitSink();
  if (!atomic_commit_sink_) {
    ALOGE("Failed to create atomic commit sink for device %s",
          GetName().c_str());
    return -ENODEV;
  }

  return 0;
}

auto DrmDevice::IsConnectorAllowed(DrmConnector &conn) const -> bool {
  return !conn.IsExternal() || external_displays_enabled_;
}

auto DrmDevice::RegisterUserPropertyBlob(const void *data, size_t length) const
    -> DrmModeUserPropertyBlobUnique {
  struct drm_mode_create_blob create_blob{};
  create_blob.length = length;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  create_blob.data = (__u64)data;

  auto ret = drmIoctl(*GetFd(), DRM_IOCTL_MODE_CREATEPROPBLOB, &create_blob);
  if (ret != 0) {
    ALOGE("Failed to create mode property blob %d", ret);
    return {};
  }

  return DrmModeUserPropertyBlobUnique(
      new uint32_t(create_blob.blob_id), [this](const uint32_t *it) {
        struct drm_mode_destroy_blob destroy_blob {};
        destroy_blob.blob_id = (__u32)*it;
        auto err = drmIoctl(*GetFd(), DRM_IOCTL_MODE_DESTROYPROPBLOB,
                            &destroy_blob);
        if (err != 0) {
          ALOGE("Failed to destroy mode property blob %" PRIu32 "/%d", *it,
                err);
        }
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        delete it;
      });
}

DrmCrtc *DrmDevice::FindCrtcById(uint32_t id) const {
  for (const auto &crtc : crtcs_) {
    if (crtc->GetId() == id) {
      return crtc.get();
    }
  };

  return nullptr;
}

DrmEncoder *DrmDevice::FindEncoderById(uint32_t id) const {
  for (const auto &enc : encoders_) {
    if (enc->GetId() == id) {
      return enc.get();
    }
  };

  return nullptr;
}

int DrmDevice::GetProperty(uint32_t obj_id, uint32_t obj_type,
                           const char *prop_name, DrmProperty *property) const {
  if (!GetFd()) {
    return -ENODEV;
  }

  auto props = GetObjectProperties(obj_id, obj_type);
  if (!props) {
    const int err = errno;
    if (obj_type == DRM_MODE_OBJECT_CONNECTOR &&
        (err == ENOENT || err == ENODEV)) {
      ALOGI("Connector %u vanished before getting properties (%s)", obj_id,
            std::generic_category().message(err).c_str());
      return -ENODEV;
    }
    ALOGE("Failed to get properties for %d/%x (%s)", obj_id, obj_type,
          std::generic_category().message(err).c_str());
    return err != 0 ? -err : -EIO;
  }

  bool found = false;
  for (int i = 0; !found && (size_t)i < props->count_props; ++i) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    drmModePropertyPtr p = drmModeGetProperty(*GetFd(), props->props[i]);
    if (strcmp(p->name, prop_name) == 0) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      property->Init(GetFd(), obj_id, p, props->prop_values[i]);
      found = true;
    }
    drmModeFreeProperty(p);
  }

  return found ? 0 : -ENOENT;
}

std::string DrmDevice::GetName() const {
  auto *ver = drmGetVersion(*GetFd());
  if (ver == nullptr) {
    ALOGW("Failed to get drm version for fd=%d", *GetFd());
    return "generic";
  }

  std::string name(ver->name);
  drmFreeVersion(ver);
  return name;
}

auto DrmDevice::IsKMSDev(int fd) -> bool {
  auto res = MakeDrmModeResUnique(fd);
  if (!res) {
    return false;
  }

  auto is_kms = res->count_crtcs > 0 && res->count_connectors > 0 &&
                res->count_encoders > 0;

  return is_kms;
}

auto DrmDevice::GetConnectors()
    -> const std::vector<std::unique_ptr<DrmConnector>> & {
  return connectors_;
}

auto DrmDevice::GetConnector(uint32_t connector_id) const
    -> DrmModeConnectorUnique {
  return MakeDrmModeConnectorUnique(*GetFd(), connector_id);
}

auto DrmDevice::GetResources() const -> DrmModeResUnique {
  return MakeDrmModeResUnique(*GetFd());
}

auto DrmDevice::GetObjectProperties(uint32_t obj_id, uint32_t obj_type) const
    -> DrmModeObjectPropertiesUnique {
  return MakeDrmModeObjectPropertiesUnique(*GetFd(), obj_id, obj_type);
}

auto DrmDevice::CreateConnectorInstance(uint32_t connector_id)
    -> std::unique_ptr<DrmConnector> {
  return DrmConnector::CreateInstance(*this, connector_id);
}

auto DrmDevice::CommitAtomicReset(drmModeAtomicReq &pset) -> int {
  return drmModeAtomicCommit(*GetFd(), &pset, DRM_MODE_ATOMIC_ALLOW_MODESET,
                             this);
}

// Normalizes a connector's parsed kernel MST PATH ("mst:<root_id>-<port>...")
// by replacing `base_connector_id` (the root physical connector's raw kernel
// object ID, which can vary across boots) with the root connector's canonical
// native port index (0..N-1). This produces a stable topological key
// (e.g. root native port 1 -> branch port 2 -> branch port 1) for both sticky
// port matching and deterministic topological sorting.
auto DrmDevice::GetNormalizedMstPath(const DrmConnector &conn) const
    -> std::optional<DrmConnector::MstPath> {
  auto mst_path = conn.GetMstPath();
  if (!mst_path.has_value()) {
    return std::nullopt;
  }

  for (const auto *list : {&connectors_, &ignored_connectors_}) {
    for (const auto &c : *list) {
      if (!c->IsWriteback() && !c->IsMst() &&
          c->GetId() == mst_path->base_connector_id) {
        mst_path->base_connector_id = c->GetIndexInResArray();
        return mst_path;
      }
    }
  }

  return mst_path;
}

auto DrmDevice::HasInactiveStickySlot(
    const std::optional<DrmConnector::MstPath> &path) const -> bool {
  if (!path.has_value()) {
    return false;
  }
  for (uint32_t i = native_ports_count_; i < kMaxPortsPerDevice; ++i) {
    if (!dynamic_ports_[i].active && dynamic_ports_[i].sticky_path == path) {
      return true;
    }
  }
  return false;
}

// Allocates a dynamic port slot in [native_ports_count_, 63] for a hotplugged
// MST connector. If an inactive "sticky" slot is already bound to `path` from
// an earlier connection in this session, it is reclaimed so unplug/replug or
// DPMS wake races do not swap monitor port IDs. Otherwise, the first unused
// slot (last_used_seq == 0) is chosen, falling back to evicting the
// least-recently-used inactive sticky slot only when all 64 slots have been
// bound. Returns std::nullopt if all 64 slots on this KMS device are
// simultaneously active.
auto DrmDevice::AllocateDynamicPort(
    const std::optional<DrmConnector::MstPath> &path)
    -> std::optional<uint32_t> {
  bool path_already_sticky = false;

  // 1. Reuse the existing sticky slot for this normalized MST path if free.
  if (path.has_value()) {
    for (uint32_t i = native_ports_count_; i < kMaxPortsPerDevice; ++i) {
      if (dynamic_ports_[i].sticky_path == path) {
        if (!dynamic_ports_[i].active) {
          dynamic_ports_[i].active = true;
          dynamic_ports_[i].last_used_seq = ++port_alloc_seq_;
          return i;
        }
        path_already_sticky = true;
      }
    }
  }

  // 2. Pick the first unreserved slot (last_used_seq == 0), or evict the
  // least-recently-used inactive sticky slot (smallest last_used_seq > 0).
  uint32_t candidate = kMaxPortsPerDevice;
  uint64_t oldest_seq = std::numeric_limits<uint64_t>::max();
  for (uint32_t i = native_ports_count_; i < kMaxPortsPerDevice; ++i) {
    if (!dynamic_ports_[i].active &&
        dynamic_ports_[i].last_used_seq < oldest_seq) {
      oldest_seq = dynamic_ports_[i].last_used_seq;
      candidate = i;
    }
  }

  if (candidate < kMaxPortsPerDevice) {
    dynamic_ports_[candidate].active = true;
    if (path.has_value() && !path_already_sticky) {
      dynamic_ports_[candidate].sticky_path = path;
      dynamic_ports_[candidate].last_used_seq = ++port_alloc_seq_;
    } else {
      dynamic_ports_[candidate].sticky_path.reset();
      dynamic_ports_[candidate].last_used_seq = 0;
    }
    return candidate;
  }

  return std::nullopt;
}

void DrmDevice::MoveOverflowConnectorsToIgnored(
    const std::set<uint32_t> &overflow_ids,
    std::vector<DrmConnector *> *out_added_connectors) {
  if (overflow_ids.empty()) {
    return;
  }
  auto keep_it = std::stable_partition(connectors_.begin(), connectors_.end(),
                                       [&](const auto &conn) {
                                         return overflow_ids.count(
                                                    conn->GetId()) == 0;
                                       });
  std::move(keep_it, connectors_.end(),
            std::back_inserter(ignored_connectors_));
  connectors_.erase(keep_it, connectors_.end());
  if (out_added_connectors != nullptr) {
    out_added_connectors->erase(std::remove_if(out_added_connectors->begin(),
                                               out_added_connectors->end(),
                                               [&](const DrmConnector *conn) {
                                                 return overflow_ids.count(
                                                            conn->GetId()) != 0;
                                               }),
                                out_added_connectors->end());
  }
}

void DrmDevice::AssignDynamicPorts(
    std::vector<DrmConnector *> &conns,
    std::vector<DrmConnector *> *out_added_connectors) {
  // Sort connectors so those reclaiming an existing inactive sticky slot are
  // assigned first (preventing a new path in the same batch from evicting
  // their sticky slot when the pool is full), and sort remaining new MST
  // connectors canonically by their normalized MstPath (root native port, then
  // branch hops) so batch/boot discovery order never depends on kernel
  // connector array order.
  std::sort(conns.begin(), conns.end(),
            [this](const DrmConnector *a, const DrmConnector *b) {
              const auto path_a = GetNormalizedMstPath(*a);
              const auto path_b = GetNormalizedMstPath(*b);
              const bool sticky_a = HasInactiveStickySlot(path_a);
              const bool sticky_b = HasInactiveStickySlot(path_b);
              return std::make_tuple(!sticky_a, !path_a.has_value(), path_a,
                                     a->GetId()) <
                     std::make_tuple(!sticky_b, !path_b.has_value(), path_b,
                                     b->GetId());
            });
  std::set<uint32_t> overflow_ids;
  for (auto *conn : conns) {
    auto port = AllocateDynamicPort(GetNormalizedMstPath(*conn));
    if (port.has_value()) {
      conn->SetIndexInResArray(*port);
    } else {
      ALOGE(
          "All %u ports on KMS device %u are simultaneously active; dropping "
          "connector ID %u to avoid display port ID collisions.",
          kMaxPortsPerDevice, index_in_dev_array_, conn->GetId());
      conn->SetIndexInResArray(kMaxPortsPerDevice);
      overflow_ids.insert(conn->GetId());
    }
  }
  MoveOverflowConnectorsToIgnored(overflow_ids, out_added_connectors);
}

void DrmDevice::InitializeConnectorPorts() {
  std::vector<DrmConnector *> native_conns;
  for (const auto *list : {&connectors_, &ignored_connectors_}) {
    for (const auto &c : *list) {
      if (!c->IsWriteback() && !c->IsMst()) {
        native_conns.push_back(c.get());
      }
    }
  }

  // Sort native non-MST connectors canonically with internal panels first,
  // then by (connector_type, connector_type_id, connector_id), so primary
  // internal displays always receive port 0 and physical external ports get
  // deterministic port IDs regardless of kernel driver registration order.
  std::sort(native_conns.begin(), native_conns.end(),
            [](const DrmConnector *a, const DrmConnector *b) {
              return std::make_tuple(!a->IsInternal(), a->GetConnectorType(),
                                     a->GetConnectorTypeId(), a->GetId()) <
                     std::make_tuple(!b->IsInternal(), b->GetConnectorType(),
                                     b->GetConnectorTypeId(), b->GetId());
            });

  native_ports_count_ = std::min<uint32_t>(static_cast<uint32_t>(
                                               native_conns.size()),
                                           kMaxPortsPerDevice);
  dynamic_ports_ = {};
  port_alloc_seq_ = 0;

  for (uint32_t i = 0; i < native_ports_count_; ++i) {
    native_conns[i]->SetIndexInResArray(i);
    dynamic_ports_[i].active = true;
    dynamic_ports_[i].last_used_seq = ++port_alloc_seq_;
  }

  std::set<uint32_t> overflow_ids;
  for (size_t i = native_ports_count_; i < native_conns.size(); ++i) {
    ALOGE(
        "Native connector ID %u on KMS device %u exceeds max %u ports per "
        "device; dropping connector to avoid display port ID collisions.",
        native_conns[i]->GetId(), index_in_dev_array_, kMaxPortsPerDevice);
    native_conns[i]->SetIndexInResArray(kMaxPortsPerDevice);
    overflow_ids.insert(native_conns[i]->GetId());
  }
  MoveOverflowConnectorsToIgnored(overflow_ids);

  std::vector<DrmConnector *> mst_conns;
  for (const auto *list : {&connectors_, &ignored_connectors_}) {
    for (const auto &c : *list) {
      if (!c->IsWriteback() && c->IsMst()) {
        mst_conns.push_back(c.get());
      }
    }
  }
  AssignDynamicPorts(mst_conns);
}

auto DrmDevice::SnapshotConnectorsUnderLock() const -> ConnectorSnapshot {
  ConnectorSnapshot snapshot{};
  for (const auto &conn : connectors_) {
    if (!conn->IsDeleted()) {
      snapshot.known_ids.insert(conn->GetId());
    }
  }
  for (const auto &conn : ignored_connectors_) {
    if (!conn->IsDeleted()) {
      snapshot.known_ids.insert(conn->GetId());
    }
  }
  for (const auto &conn : writeback_connectors_) {
    snapshot.known_ids.insert(conn->GetId());
  }
  return snapshot;
}

auto DrmDevice::ProbeConnectorsOffLock(const ConnectorSnapshot &snapshot)
    -> ProbedConnectorResources {
  ProbedConnectorResources probed{};
  auto res = GetResources();
  if (!res) {
    ALOGE("Failed to get DrmDevice resources");
    probed.had_probe_error = true;
    return probed;
  }

  probed.has_resources = true;
  for (int i = 0; i < res->count_connectors; ++i) {
    probed.resource_ids.insert(*std::next(res->connectors, i));
  }

  std::set<uint32_t> seen_ids = snapshot.known_ids;
  for (int i = 0; i < res->count_connectors; ++i) {
    const uint32_t conn_id = *std::next(res->connectors, i);
    if (seen_ids.count(conn_id) != 0) {
      continue;
    }
    seen_ids.insert(conn_id);
    auto conn = CreateConnectorInstance(conn_id);
    if (!conn) {
      probed.had_probe_error = true;
      continue;
    }
    if (conn->IsWriteback()) {
      continue;
    }
    if (conn->GetModes().empty()) {
      auto conn_probed = conn->ProbeModesAndPropertiesOffLock();
      if (conn->ApplyProbedState(std::move(conn_probed)) != 0 ||
          conn->IsDeleted()) {
        probed.had_probe_error = true;
        continue;
      }
    }
    probed.new_connectors.emplace_back(std::move(conn));
  }

  return probed;
}

auto DrmDevice::ApplyProbedConnectorsUnderLock(
    ProbedConnectorResources &&probed,
    std::vector<DrmConnector *> *out_added_connectors)
    -> std::vector<std::unique_ptr<DrmConnector>> {
  ProbedConnectorResources state = std::move(probed);
  std::vector<std::unique_ptr<DrmConnector>> stale_connectors;
  if (!state.has_resources) {
    return stale_connectors;
  }

  std::set<uint32_t> conn_ids_present;
  for (const auto &conn : writeback_connectors_) {
    conn_ids_present.insert(conn->GetId());
  }

  auto remove_stale = [&](std::vector<std::unique_ptr<DrmConnector>> &conns,
                          bool is_ignored) {
    auto stale_it = std::
        stable_partition(conns.begin(), conns.end(), [&](const auto &conn) {
          const bool in_resources = state.resource_ids.count(conn->GetId()) !=
                                    0;
          if (in_resources && !conn->IsDeleted()) {
            conn_ids_present.insert(conn->GetId());
            return true;
          }
          return false;
        });

    for (auto it = stale_it; it != conns.end(); ++it) {
      (*it)->MarkDeleted();
      if (!(*it)->IsWriteback()) {
        const uint32_t slot = (*it)->GetIndexInResArray();
        if (slot < kMaxPortsPerDevice) {
          dynamic_ports_[slot].active = false;
        }
      }
    }
    if (!is_ignored) {
      std::move(stale_it, conns.end(), std::back_inserter(stale_connectors));
    }

    conns.erase(stale_it, conns.end());
  };

  // Release port slots of stale connectors from both active and ignored lists
  // before scanning new resources so a rapid MST hub replug in the same
  // refresh pass can immediately reuse its sticky port slot.
  remove_stale(connectors_, false);
  remove_stale(ignored_connectors_, true);

  for (const auto *list : {&connectors_, &ignored_connectors_}) {
    for (const auto &conn : *list) {
      if (!conn->IsWriteback() &&
          conn->GetIndexInResArray() < kMaxPortsPerDevice) {
        dynamic_ports_[conn->GetIndexInResArray()].active = true;
      }
    }
  }

  std::vector<DrmConnector *> new_conns;
  for (auto &conn : state.new_connectors) {
    if (!conn || conn->IsDeleted() ||
        conn_ids_present.count(conn->GetId()) != 0) {
      continue;
    }
    conn_ids_present.insert(conn->GetId());
    new_conns.push_back(conn.get());
    if (IsConnectorAllowed(*conn)) {
      if (out_added_connectors != nullptr) {
        out_added_connectors->push_back(conn.get());
      }
      connectors_.emplace_back(std::move(conn));
    } else {
      ignored_connectors_.emplace_back(std::move(conn));
    }
  }
  AssignDynamicPorts(new_conns, out_added_connectors);

  return stale_connectors;
}

auto DrmDevice::RefreshConnectors()
    -> std::vector<std::unique_ptr<DrmConnector>> {
  auto snapshot = SnapshotConnectorsUnderLock();
  auto probed = ProbeConnectorsOffLock(snapshot);
  return ApplyProbedConnectorsUnderLock(std::move(probed));
}

auto DrmDevice::ResetConnectorsAndCrtcs() -> void {
  auto pset = MakeDrmModeAtomicReqUnique();
  if (!pset) {
    ALOGE("Failed to allocate property set");
    return;
  }

  std::set<uint32_t> crtcs_to_skip;
  static const bool kSkipInternalReset = Properties::SkipInternalDisplayReset();
  auto is_non_internal_virtual_connector = [](const DrmConnector &conn) {
    return conn.GetConnectorType() == DRM_MODE_CONNECTOR_VIRTUAL &&
           !conn.DrmConnector::IsInternal();
  };

  std::vector<DrmConnector *> reset_connectors;
  std::vector<DrmCrtc *> reset_crtcs;
  std::vector<DrmPlane *> reset_planes;

  for (const auto &conn : connectors_) {
    if (is_non_internal_virtual_connector(*conn)) {
      auto crtc_id = conn->GetCrtcIdProperty().GetValue();
      if (crtc_id && *crtc_id != 0) {
        crtcs_to_skip.insert(*crtc_id);
      }
      continue;
    }
    if (kSkipInternalReset && conn->IsInternal()) {
      auto crtc_id = conn->GetCrtcIdProperty().GetValue();
      if (crtc_id && *crtc_id != 0) {
        crtcs_to_skip.insert(*crtc_id);
      }
      continue;
    }
    if (!conn->GetCrtcIdProperty().AtomicSet(*pset.get(), 0)) {
      ALOGE("Failed to Set Crtc Id Prop to Null for Conn = %d", conn->GetId());
      return;
    }
    reset_connectors.push_back(conn.get());
  }
  for (const auto &conn : ignored_connectors_) {
    if (is_non_internal_virtual_connector(*conn)) {
      auto crtc_id = conn->GetCrtcIdProperty().GetValue();
      if (crtc_id && *crtc_id != 0) {
        crtcs_to_skip.insert(*crtc_id);
      }
      continue;
    }
    if (!conn->GetCrtcIdProperty().AtomicSet(*pset.get(), 0)) {
      ALOGE("Failed to Set Crtc Id Prop to Null for Ignored Conn = %d",
            conn->GetId());
      return;
    }
    reset_connectors.push_back(conn.get());
  }
  for (const auto &crtc : crtcs_) {
    if (crtcs_to_skip.find(crtc->GetId()) != crtcs_to_skip.end()) {
      continue;
    }
    if (!crtc->GetModeProperty().AtomicSet(*pset.get(), 0) ||
        !crtc->GetActiveProperty().AtomicSet(*pset.get(), 0)) {
      ALOGE(
          "Failed to set Mode Property or Active Property to Null for CRTC = "
          "%d",
          crtc->GetId());
      return;
    }
    reset_crtcs.push_back(crtc.get());
  }
  for (const auto &plane : planes_) {
    const auto plane_crtc_id = plane->GetCrtcProperty().GetValue();
    const auto plane_fb_id = plane->GetFbProperty().GetValue();
    if (plane_crtc_id.value_or(0) == 0 && plane_fb_id.value_or(0) == 0) {
      continue;
    }
    const bool is_on_skipped_crtc = plane_crtc_id.has_value() &&
                                    crtcs_to_skip.find(*plane_crtc_id) !=
                                        crtcs_to_skip.end();
    if (is_on_skipped_crtc) {
      continue;
    }
    if (plane->AtomicDisablePlane(*pset.get()) != 0) {
      ALOGE("Failed to disable Plane = %d", plane->GetId());
      return;
    }
    reset_planes.push_back(plane.get());
  }
  auto err = CommitAtomicReset(*pset.get());
  if (err != 0) {
    ALOGE("Failed to commit pset ret=%d", err);
    return;
  }
  for (auto *conn : reset_connectors) {
    conn->GetCrtcIdProperty().UpdateValue(0);
  }
  for (auto *crtc : reset_crtcs) {
    crtc->GetModeProperty().UpdateValue(0);
    crtc->GetActiveProperty().UpdateValue(0);
  }
  for (auto *plane : reset_planes) {
    plane->GetCrtcProperty().UpdateValue(0);
    plane->GetFbProperty().UpdateValue(0);
  }
}

auto DrmDevice::GetPlanes() -> const std::vector<std::unique_ptr<DrmPlane>> & {
  return planes_;
}

auto DrmDevice::GetCrtcs() -> const std::vector<std::unique_ptr<DrmCrtc>> & {
  return crtcs_;
}

auto DrmDevice::GetEncoders()
    -> const std::vector<std::unique_ptr<DrmEncoder>> & {
  return encoders_;
}

class DumbBufferFd : public PrimeFdsSharedBase {
 public:
  SharedFd fd;
};

auto DrmDevice::CreateDumbBuffer(uint32_t width, uint32_t height,
                                 uint32_t format) -> std::optional<BufferInfo> {
  return CreateDumbBuffer(width, height, format, 0);
}

// NOLINTBEGIN(cppcoreguidelines-avoid-goto)
auto DrmDevice::CreateDumbBuffer(uint32_t width, uint32_t height,
                                 uint32_t format, uint32_t prime_fd_flags)
    -> std::optional<BufferInfo> {
  const uint32_t bpp = BufferInfoGetter::DrmFormatToBpp(format);
  std::optional<BufferInfo> result;
  void *ptr = MAP_FAILED;
  struct drm_mode_create_dumb create = {
      .height = height,
      .width = width,
      .bpp = bpp,
      .flags = 0,
  };

  int ret = drmIoctl(*fd_, DRM_IOCTL_MODE_CREATE_DUMB, &create);
  if (ret != 0) {
    ALOGE("Failed to DRM_IOCTL_MODE_CREATE_DUMB %d", ret);
    return {};
  }

  struct drm_mode_map_dumb map = {
      .handle = create.handle,
  };

  auto dumb_buffer_fd = std::make_shared<DumbBufferFd>();

  BufferInfo buffer_info = {
      .width = width,
      .height = height,

      .format = format,
      .pitches = {create.pitch},
      .sizes = {static_cast<uint32_t>(create.size)},
      .prime_fds = {-1, -1, -1, -1},
      .modifiers = {DRM_FORMAT_MOD_NONE},

      .color_encoding = BufferColorEncoding::kUndefined,
      .sample_range = BufferSampleRange::kUndefined,
      .blend_mode = BufferBlendMode::kNone,

      .fds_shared = dumb_buffer_fd,
  };

  ret = drmIoctl(*fd_, DRM_IOCTL_MODE_MAP_DUMB, &map);
  if (ret != 0) {
    ALOGE("Failed to DRM_IOCTL_MODE_MAP_DUMB %d", errno);
    goto done;
  }

  // NOLINTNEXTLINE(misc-include-cleaner)
  ptr = mmap(nullptr, create.size, PROT_READ | PROT_WRITE, MAP_SHARED, *fd_,
             (off_t)map.offset);
  if (ptr == MAP_FAILED) {
    ALOGE("Failed to mmap dumb buffer %d", errno);
    goto done;
  }

  memset(ptr, 0, create.size);

  if (munmap(ptr, create.size) != 0) {
    ALOGE("Failed to unmap dumb buffer: %d", errno);
  }

  ret = drmPrimeHandleToFD(*fd_, create.handle, prime_fd_flags,
                           &buffer_info.prime_fds[0]);
  if (ret != 0) {
    ALOGE("Failed to export dumb buffer as FD: %d", errno);
    goto done;
  }

  dumb_buffer_fd->fd = MakeSharedFd(buffer_info.prime_fds[0]);

  result = buffer_info;

done:
  if (create.handle > 0) {
    struct drm_mode_destroy_dumb destroy = {
        .handle = create.handle,
    };
    drmIoctl(*fd_, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
  }

  return result;
}
// NOLINTEND(cppcoreguidelines-avoid-goto)

}  // namespace android::drm_hwcomposer
