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

#include "ResourceManager.h"

#include <android-base/strings.h>
#include <cutils/properties.h>
#include <linux/time.h>
#include <sys/stat.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "backend/BackendManager.h"
#include "bufferinfo/BufferInfoGetter.h"
#include "bufferinfo/BufferInfoMapperMetadata.h"
#include "drm/DrmConnector.h"
#include "drm/DrmDevice.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/UEventListener.h"
#include "utils/log.h"
#include "utils/properties.h"

namespace android::drm_hwcomposer {

namespace {

// Quirk flags for connectors identified via EDID manufacturer ID.
enum ConnectorQuirk : uint32_t {
  // Monitor uses a TCON/scaler that initially serves a dummy low-resolution
  // EDID on boot. The real EDID becomes available only after the DP link is
  // torn down and re-established, giving the scaler time to complete its
  // internal initialization. Seen on Novatek-based gaming monitors connected
  // via USB-C DP-alt (e.g. ASUS VG28UQL1A).
  kQuirkDelayedEdidOnBoot = 1 << 0,
};

struct EdidQuirkEntry {
  const char *mfg_id;    // 3-letter PNP manufacturer ID
  std::optional<uint16_t> product_code; // nullopt = match any product
  uint32_t quirks;       // Bitmask of ConnectorQuirk flags
};

// Table of known monitors/scalers requiring special handling.
// To add a new quirk: append an entry with the manufacturer PNP ID
// (from EDID bytes 8-9) and the applicable quirk flags.
constexpr EdidQuirkEntry kEdidQuirks[] = {
    // Novatek Microelectronics TCON/scaler: boots with dummy 2K EDID over
    // DP1.2/HBR2, real 4K EDID (DP1.4/HBR3) available ~2-3s after boot
    // on a fresh DP link session.
    {"NVT", std::nullopt, kQuirkDelayedEdidOnBoot},
};

// Decode the PNP manufacturer ID from an EDID blob and return matching quirks.
constexpr size_t kMinEdidBlobSize = 16;
constexpr int kDefaultRecoveryDelayMs = 3000;

uint32_t GetEdidQuirks(DrmConnector *conn) {
  auto blob = conn->GetEdidBlob();
  if (!blob || blob->length < kMinEdidBlobSize)
    return 0;

  std::string pnp_id;
  uint16_t product = 0;

#if HAS_LIBDISPLAY_INFO
  // TODO(aswolfers): Move EdidWrapper ownership to DrmConnector to avoid decoding the
  // EDID blob a second time here (HwcDisplay already creates one).
  auto edid_wrapper = LibdisplayEdidWrapper::Create(std::move(blob));
  if (edid_wrapper) {
    auto info = edid_wrapper->GetVendorProductInfo();
    pnp_id = info.manufacturer.substr(0, 3);
    product = info.product_code;
  }
#else
  // NOLINTBEGIN(readability-magic-numbers, cppcoreguidelines-pro-bounds-pointer-arithmetic)
  // Fall back to raw EDID parsing when libdisplay-info is not available.
  const auto *edid = static_cast<const uint8_t *>(blob->data);

  // Validate EDID header (00 FF FF FF FF FF FF 00)
  if (edid[0] != 0x00 || edid[1] != 0xFF || edid[6] != 0xFF ||
      edid[7] != 0x00) {
    return 0;
  }

  // Manufacturer PNP ID at bytes 8-9 (big-endian 16-bit).
  // Decode 3 letters: bits[14:10]=1st, bits[9:5]=2nd, bits[4:0]=3rd
  // where A=1, B=2, ..., Z=26
  uint16_t mfg = (static_cast<uint16_t>(edid[8]) << 8) | edid[9];
  char id[4] = {
      static_cast<char>(((mfg >> 10) & 0x1F) + 'A' - 1),
      static_cast<char>(((mfg >> 5) & 0x1F) + 'A' - 1),
      static_cast<char>((mfg & 0x1F) + 'A' - 1),
      '\0'};
  pnp_id = id;

  // EDID product code at bytes 10-11 (little-endian)
  product = static_cast<uint16_t>(edid[10]) |
            (static_cast<uint16_t>(edid[11]) << 8);
  // NOLINTEND(readability-magic-numbers, cppcoreguidelines-pro-bounds-pointer-arithmetic)
#endif

  if (pnp_id.empty())
    return 0;

  uint32_t quirks = 0;
  for (const auto &entry : kEdidQuirks) {
    if (pnp_id == entry.mfg_id &&
        (!entry.product_code || *entry.product_code == product)) {
      quirks |= entry.quirks;
    }
  }
  return quirks;
}

// Convenience: check if a connector has the delayed-EDID-on-boot quirk.
bool HasDelayedEdidQuirk(DrmConnector *conn) {
  return (GetEdidQuirks(conn) & kQuirkDelayedEdidOnBoot) != 0;
}

}  // namespace


ResourceManager::ResourceManager(
    PipelineToFrontendBindingInterface *p2f_bind_interface)
    : frontend_interface_(p2f_bind_interface) {
}

ResourceManager::~ResourceManager() = default;

void ResourceManager::Init() {
  if (initialized_) {
    ALOGE("Already initialized");
    return;
  }

  color_pipeline_enabled_ = Properties::UseColorPipeline();
  force_color_mode_ = Properties::ForceColorMode();
  persistent_hdr_enabled_ = Properties::PersistentHdrEnabled();

  // Could be a valid path or it can have at the end of it the wildcard %
  // which means that it will try open all devices until an error is met.
  std::string path_pattern = Properties::GetDevicePath();
  if (path_pattern.empty()) {
    path_pattern = "/dev/dri/card%";
  }
  if (path_pattern.back() != '%') {
    auto dev = DrmDevice::CreateInstance(path_pattern, this, 0);
    if (dev) {
      drms_.emplace_back(std::move(dev));
    }
  } else {
    path_pattern.resize(path_pattern.size() - 1);
    for (int idx = 0;; ++idx) {
      std::ostringstream path;
      path << path_pattern << idx;

      struct stat buf {};
      if (stat(path.str().c_str(), &buf) != 0)
        break;

      auto dev = DrmDevice::CreateInstance(path.str(), this, idx);
      if (dev) {
        drms_.emplace_back(std::move(dev));
      }
    }
  }
  ALOGE_IF(drms_.empty(), "No DRM devices available.");

  auto display_str = Properties::InternalDisplayNames();
  auto display_names = base::Tokenize(display_str, ",");
  displays_.insert(display_names.begin(), display_names.end());

  scale_with_gpu_ = Properties::ScaleWithGpu();
  ctm_handling_ = Properties::GetCtmHandling();

  if (BufferInfoGetter::GetInstance() == nullptr) {
    std::unique_ptr<BufferInfoGetter> buffer_info_getter;
    if (!drms_.empty()) {
      buffer_info_getter = drms_[0]->GetBackend().CreateBufferInfoGetter();
    }
    if (!buffer_info_getter) {
      ALOGE(
          "Failed to create BufferInfoGetter from backend, falling back to "
          "BufferInfoMapperMetadata.");
      buffer_info_getter = BufferInfoMapperMetadata::CreateInstance();
    }
    BufferInfoGetter::Init(std::move(buffer_info_getter));
  }

  for (auto &drm : drms_) {
    drm->ResetConnectorsAndCrtcs();
  }

  uevent_listener_ = UEventListener::CreateInstance([this] {
    {
      std::scoped_lock lock(GetMainLock());
      for (auto &drm : drms_) {
        auto stale_connectors = drm->RefreshConnectors();
        DetachStalePipelines(stale_connectors);
      }
      UpdateFrontendDisplays();
    }
    frontend_interface_->FlushHotplugEvents();
  });

  UpdateFrontendDisplays();
  MaybeScheduleDelayedEdidRecovery();
  frontend_interface_->FlushHotplugEvents();

  initialized_ = true;
}

void ResourceManager::DeInit() {
  if (!initialized_) {
    ALOGE("Not initialized");
    return;
  }

  CancelDelayedEdidRecovery();

  uevent_listener_.reset();

  DetachAllFrontendDisplays();
  drms_.clear();

  initialized_ = false;
}

const std::set<std::string>& ResourceManager::GetInternalDisplayNames() {
  return displays_;
}

auto ResourceManager::GetTimeMonotonicNs() -> int64_t {
  struct timespec ts {};
  // NOLINTNEXTLINE(misc-include-cleaner)
  clock_gettime(CLOCK_MONOTONIC, &ts);
  constexpr int64_t kNsInSec = 1000000000LL;
  return (int64_t(ts.tv_sec) * kNsInSec) + int64_t(ts.tv_nsec);
}

void ResourceManager::UpdateFrontendDisplays() {
  auto ordered_connectors = GetOrderedConnectors();

  for (auto *conn : ordered_connectors) {
    conn->UpdateModes();
    auto connected = conn->IsConnected();
    auto attached = attached_pipelines_.count(conn) != 0;

    if (connected != attached) {
      ALOGI("%s connector %s", connected ? "Attaching" : "Detaching",
            conn->GetName().c_str());

      if (connected) {
        std::shared_ptr<DrmDisplayPipeline>
            pipeline = conn->GetDev().GetBackend().CreatePipeline(*conn);
        ALOGE_IF(pipeline == nullptr,
                 "Failed to create pipeline for connector %s",
                 conn->GetName().c_str());
        if (pipeline) {
          frontend_interface_->BindDisplay(pipeline);
          attached_pipelines_[conn] = std::move(pipeline);
        }
      } else {
        auto &pipeline = attached_pipelines_[conn];
        frontend_interface_->UnbindDisplay(pipeline);
        attached_pipelines_.erase(conn);
      }
    }
    if (connected) {
      if (!conn->IsLinkStatusGood()) {
        conn->SetLinkRecoveryRequired(true);
        frontend_interface_->NotifyDisplayLinkStatus(attached_pipelines_[conn]);
      }

      // If content protection is not enabled anymore, inform frontend so it
      // can terminate HDCP handling for this display.
      if (conn->UpdateContentProtection() &&
          !conn->IsContentProtectionEnabled()) {
        frontend_interface_->NotifyHdcpTermination(attached_pipelines_[conn]);
      }
    }
  }
  frontend_interface_->FinalizeDisplayBinding();
}

void ResourceManager::DetachStalePipelines(
    const std::vector<std::unique_ptr<DrmConnector>> &stale_connectors) {
  for (const auto &conn : stale_connectors) {
    auto it = attached_pipelines_.find(conn.get());
    if (it != attached_pipelines_.end()) {
      ALOGI("Detaching pipeline for stale connector %s (id=%d)",
            conn->GetName().c_str(), conn->GetId());
      frontend_interface_->UnbindDisplay(it->second);
      attached_pipelines_.erase(it);
    }
  }
}

void ResourceManager::DetachAllFrontendDisplays() {
  for (auto &p : attached_pipelines_) {
    frontend_interface_->UnbindDisplay(p.second);
  }
  attached_pipelines_.clear();
  frontend_interface_->FinalizeDisplayBinding();
}

auto ResourceManager::GetOrderedConnectors() -> std::vector<DrmConnector *> {
  /* Put internal displays first then external to
   * ensure Internal will take Primary slot
   */

  std::vector<DrmConnector *> ordered_connectors;

  for (auto &drm : drms_) {
    for (const auto &conn : drm->GetConnectors()) {
      if (conn->IsInternal()) {
        ordered_connectors.emplace_back(conn.get());
      }
    }
  }

  for (auto &drm : drms_) {
    for (const auto &conn : drm->GetConnectors()) {
      if (conn->IsExternal()) {
        ordered_connectors.emplace_back(conn.get());
      }
    }
  }

  return ordered_connectors;
}

auto ResourceManager::GetVirtualDisplayPipeline()
    -> std::shared_ptr<DrmDisplayPipeline> {
  for (auto &drm : drms_) {
    for (const auto &conn : drm->GetWritebackConnectors()) {
      auto pipeline = DrmDisplayPipeline::CreatePipeline(*conn);
      if (!pipeline) {
        ALOGE("Failed to create pipeline for writeback connector %s",
              conn->GetName().c_str());
      }
      if (pipeline) {
        return pipeline;
      }
    }
  }
  return {};
}

auto ResourceManager::GetWritebackConnectorsCount() -> uint32_t {
  uint32_t count = 0;
  for (auto &drm : drms_) {
    count += drm->GetWritebackConnectors().size();
  }
  return count;
}

std::optional<std::string> ResourceManager::DumpBackends() {
  std::stringstream output;
  for (auto &drm : drms_) {
    auto dump = drm->GetBackend().Dump();
    if (dump.has_value()) {
      output << "\n<start " << drm->GetName() << ">\n";
      output << dump.value();
      output << "\n<end " << drm->GetName() << ">\n";
    }
  }
  auto result = output.str();
  return result.empty() ? std::nullopt : std::make_optional(result);
}


void ResourceManager::MaybeScheduleDelayedEdidRecovery() {
  // Check if any attached external DP connector matches a delayed-EDID quirk
  // (indicates the monitor's scaler hasn't finished booting yet).
  DrmConnector *quirk_conn = nullptr;
  for (auto &[conn, pipeline] : attached_pipelines_) {
    if (conn->IsConnected() && conn->IsExternal() &&
        conn->GetConnectorType() == DRM_MODE_CONNECTOR_DisplayPort && // NOLINT(misc-include-cleaner)
        HasDelayedEdidQuirk(conn)) {
      quirk_conn = conn;
      break;
    }
  }

  if (quirk_conn == nullptr) {
    return;
  }

  // Configurable delay (ms) for the scaler to finish its boot sequence.
  // Default 3000ms is sufficient for Novatek-based TCONs.
  int delay_ms = property_get_int32(
      "vendor.hwc.delayed_edid_recovery_ms", kDefaultRecoveryDelayMs);
  if (delay_ms <= 0) {
    ALOGW("Delayed-EDID recovery disabled via property on %s",
          quirk_conn->GetName().c_str());
    return;
  }

  ALOGI("Delayed-EDID quirk matched on %s, scheduling DP link-reset reprobe "
        "in %d ms",
        quirk_conn->GetName().c_str(), delay_ms);

  edid_recovery_pending_ = true;
  edid_recovery_thread_ = std::thread([this, delay_ms] {
    // Wait for the scaler to complete its internal boot sequence.
    // Use a condition variable so DeInit() can cancel this wait.
    {
      std::unique_lock lock(edid_recovery_mutex_);
      if (edid_recovery_cv_.wait_for(lock,
                                     std::chrono::milliseconds(delay_ms),
                                     [this] { return !edid_recovery_pending_; })) {
        ALOGI("Delayed-EDID recovery cancelled");
        return;
      }
    }

    std::unique_lock lock(GetMainLock());

    // Unbind all external DP connectors that match delayed-EDID quirk.
    // This disables the CRTC -> tears down the DP link from the source side,
    // forcing the kernel to do full link re-training on next modeset.
    for (auto it = attached_pipelines_.begin();
         it != attached_pipelines_.end();) {
      auto *conn = it->first;
      if (conn->IsConnected() && conn->IsExternal() &&
          conn->GetConnectorType() == DRM_MODE_CONNECTOR_DisplayPort && // NOLINT(misc-include-cleaner)
          HasDelayedEdidQuirk(conn)) {
        ALOGI("Tearing down DP link on %s for delayed-EDID recovery",
              conn->GetName().c_str());
        frontend_interface_->UnbindDisplay(it->second);
        it = attached_pipelines_.erase(it);
      } else {
        ++it;
      }
    }
    frontend_interface_->FinalizeDisplayBinding();

    // Release MainLock during DP link teardown wait to avoid
    // blocking other display operations.
    lock.unlock();

    // Brief pause for the link to fully go down before reprobing.
    std::this_thread::sleep_for(std::chrono::seconds(1));

    lock.lock();

    // Re-probe: kernel re-reads DPCD/EDID since link was torn down.
    // If scaler now serves real 4K EDID, display re-attaches at full res.
    for (auto &drm : drms_) {
      auto stale = drm->RefreshConnectors();
      DetachStalePipelines(stale);
    }
    UpdateFrontendDisplays();

    edid_recovery_pending_ = false;
    ALOGI("Delayed-EDID recovery reprobe complete");
  });
}

void ResourceManager::CancelDelayedEdidRecovery() {
  if (edid_recovery_pending_) {
    {
      std::lock_guard lock(edid_recovery_mutex_);
      edid_recovery_pending_ = false;
    }
    edid_recovery_cv_.notify_all();
  }
  if (edid_recovery_thread_.joinable()) {
    edid_recovery_thread_.join();
  }
}

}  // namespace android::drm_hwcomposer
