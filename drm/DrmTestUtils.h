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

#include <drm/drm_mode.h>
#include <fcntl.h>  // IWYU pragma: keep
#include <sys/types.h>
#include <unistd.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "backend/Backend.h"
#include "backend/GenericBackend.h"
#include "bufferinfo/BufferInfo.h"
#include "drm/AtomicCommitSink.h"
#include "drm/AtomicStateManager.h"
#include "drm/CommitStatus.h"
#include "drm/DrmAtomicStateManager.h"
#include "drm/DrmConnector.h"
#include "drm/DrmCrtc.h"
#include "drm/DrmDevice.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmEncoder.h"
#include "drm/DrmFbImporter.h"
#include "drm/DrmMode.h"
#include "drm/DrmPlane.h"
#include "drm/DrmProperty.h"
#include "drm/DrmUnique.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {

constexpr uint16_t kFakeDefaultModeWidth = 1920;
constexpr uint16_t kFakeDefaultModeHeight = 1080;
constexpr uint32_t kFakeDefaultModeVRefresh = 60;
constexpr uint16_t kFakeModeHTotal = 2000;
constexpr uint16_t kFakeModeVTotal = 1000;
constexpr uint32_t kFakeModeClockMultiplier = 2000;
constexpr uint32_t kFakeConnectorBaseId = 100;
constexpr uint32_t kFakeConnectorCrtcPropId = 10;
constexpr uint32_t kFakeCrtcActivePropId = 20;
constexpr uint32_t kFakeCrtcModePropId = 21;
constexpr uint32_t kFakeCrtcOutFencePropId = 22;
constexpr uint32_t kFakePlaneBaseId = 300;
constexpr uint32_t kFakePlaneCrtcPropId = 30;
constexpr uint32_t kFakePlaneFbPropId = 31;
constexpr uint32_t kFakePlaneCrtcXPropId = 32;
constexpr uint32_t kFakePlaneCrtcYPropId = 33;
constexpr uint32_t kFakePlaneCrtcWPropId = 34;
constexpr uint32_t kFakePlaneCrtcHPropId = 35;
constexpr uint32_t kFakePlaneSrcXPropId = 36;
constexpr uint32_t kFakePlaneSrcYPropId = 37;
constexpr uint32_t kFakePlaneSrcWPropId = 38;
constexpr uint32_t kFakePlaneSrcHPropId = 39;
constexpr uint32_t kFakeContentProtectionPropId = 40;
constexpr uint32_t kFakeHdcpContentTypePropId = 41;
constexpr uint32_t kFakeFbBaseId = 500;
constexpr uint32_t kFakeBlobBaseId = 1000;
constexpr uint32_t kFakeConnectorMmWidth = 500;
constexpr uint32_t kFakeConnectorMmHeight = 300;
constexpr uint32_t kFakeBytesPerPixel = 4;

inline void InitFakeDrmProperty(DrmProperty& prop, uint32_t obj_id,
                                uint32_t prop_id, const char* name,
                                uint32_t flags = DRM_MODE_PROP_RANGE,
                                uint64_t value = 0) {
  drmModePropertyRes raw_prop{};
  raw_prop.prop_id = prop_id;
  raw_prop.flags = flags;
  std::strncpy(raw_prop.name, name, DRM_PROP_NAME_LEN - 1);
  uint64_t values[2] = {0, UINT64_MAX};
  raw_prop.count_values = 2;
  raw_prop.values = values;
  prop.Init(nullptr, obj_id, &raw_prop, value);
}

// Inspects the latest value queued for (obj_id, prop_id) inside an uncommitted
// drmModeAtomicReq by mirroring libdrm's internal _drmModeAtomicReq layout.
inline std::optional<uint64_t> GetAtomicReqPropertyValue(
    drmModeAtomicReqPtr req, uint32_t obj_id, uint32_t prop_id) {
  struct ReqItemView {
    uint32_t object_id;
    uint32_t property_id;
    uint64_t value;
    uint32_t cursor;
  };
  struct ReqView {
    uint32_t cursor;
    uint32_t size_items;
    ReqItemView* items;
  };
  if (req == nullptr) {
    return std::nullopt;
  }
  const int cursor = drmModeAtomicGetCursor(req);
  if (cursor <= 0) {
    return std::nullopt;
  }
  ReqView view{};
  std::memcpy(&view, req, sizeof(view));
  if (view.items == nullptr) {
    return std::nullopt;
  }
  std::vector<ReqItemView> items(static_cast<size_t>(cursor));
  std::memcpy(items.data(), view.items, items.size() * sizeof(ReqItemView));
  std::optional<uint64_t> result;
  for (const auto& item : items) {
    if (item.object_id == obj_id && item.property_id == prop_id) {
      result = item.value;
    }
  }
  return result;
}

inline DrmMode CreateFakeDrmMode(uint16_t width = kFakeDefaultModeWidth,
                                 uint16_t height = kFakeDefaultModeHeight,
                                 uint32_t vrefresh = kFakeDefaultModeVRefresh,
                                 uint32_t type = DRM_MODE_TYPE_PREFERRED,
                                 const char* name = "1920x1080") {
  drmModeModeInfo mode_info{};
  mode_info.hdisplay = width;
  mode_info.vdisplay = height;
  mode_info.htotal = kFakeModeHTotal;
  mode_info.vtotal = kFakeModeVTotal;
  mode_info.vrefresh = vrefresh;
  mode_info.clock = vrefresh * kFakeModeClockMultiplier;
  mode_info.type = type;
  std::strncpy(mode_info.name, name, DRM_DISPLAY_MODE_LEN - 1);
  return DrmMode(&mode_info);
}

class FakeAtomicCommitSink : public AtomicCommitSink {
 public:
  CommitStatus TestAtomicCommit(
      const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>& args)
      const override {
    ++test_commit_count_;
    last_test_args_.clear();
    for (const auto& [manager, commit_args] : args) {
      AtomicCommitArgs mutable_args = commit_args;
      if (drive_state_manager_ && manager != nullptr) {
        auto req = manager->GetAtomicModeReqForArgs(mutable_args);
        if (mutable_args.HasInputs() && !req) {
          return CommitStatus::Failure(-EINVAL);
        }
      }
      last_test_args_.push_back(mutable_args);
    }
    if (test_commit_hook_) {
      return test_commit_hook_(args);
    }
    return next_test_status_;
  }

  CommitStatusOr<
      std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>>
  ExecuteAtomicCommit(
      const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>& args)
      override {
    ++execute_commit_count_;
    commit_history_.emplace_back();
    for (const auto& [manager, commit_args] : args) {
      commit_history_.back().push_back(commit_args);
    }

    if (execute_commit_hook_) {
      return execute_commit_hook_(args);
    }

    if (!next_execute_status_.success) {
      return CommitStatusOr<
          std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>>(
          next_execute_status_);
    }

    std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>> results;
    effective_commit_history_.emplace_back();
    for (const auto& [manager, commit_args] : args) {
      AtomicCommitArgs mutable_args = commit_args;
      if (drive_state_manager_ && manager != nullptr) {
        auto req = manager->GetAtomicModeReqForArgs(mutable_args);
        effective_commit_history_.back().push_back(mutable_args);
        if (!mutable_args.HasInputs()) {
          results.emplace_back(manager, AtomicCommitResult{});
          continue;
        }
        if (!req) {
          return CommitStatusOr<
              std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>>(
              CommitStatus::Failure(-EINVAL));
        }
        manager->WaitLastFrame();
        auto result = manager->FinishPendingRequest(mutable_args,
                                                    std::move(req));
        results.emplace_back(manager, std::move(result));
      } else {
        effective_commit_history_.back().push_back(mutable_args);
        results.emplace_back(manager, AtomicCommitResult{});
      }
    }
    return CommitStatusOr<
        std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>>(
        results);
  }

  void SetDriveStateManager(bool drive) {
    drive_state_manager_ = drive;
  }

  void SetNextTestStatus(CommitStatus status) {
    next_test_status_ = status;
  }

  void SetNextExecuteStatus(CommitStatus status) {
    next_execute_status_ = status;
  }

  void SetTestCommitHook(
      std::function<CommitStatus(
          const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>&)>
          hook) {
    test_commit_hook_ = std::move(hook);
  }

  void SetExecuteCommitHook(
      std::function<CommitStatusOr<
          std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>>(
          const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>&)>
          hook) {
    execute_commit_hook_ = std::move(hook);
  }

  size_t GetTestCommitCount() const {
    return test_commit_count_;
  }

  size_t GetExecuteCommitCount() const {
    return execute_commit_count_;
  }

  const std::vector<std::vector<AtomicCommitArgs>>& GetCommitHistory() const {
    return commit_history_;
  }

  const std::vector<std::vector<AtomicCommitArgs>>& GetEffectiveCommitHistory()
      const {
    return effective_commit_history_;
  }

  void ClearHistory() {
    test_commit_count_ = 0;
    execute_commit_count_ = 0;
    last_test_args_.clear();
    commit_history_.clear();
    effective_commit_history_.clear();
  }

 private:
  bool drive_state_manager_ = false;
  mutable size_t test_commit_count_ = 0;
  size_t execute_commit_count_ = 0;
  mutable std::vector<AtomicCommitArgs> last_test_args_;
  std::vector<std::vector<AtomicCommitArgs>> commit_history_;
  std::vector<std::vector<AtomicCommitArgs>> effective_commit_history_;
  CommitStatus next_test_status_ = CommitStatus::Success();
  CommitStatus next_execute_status_ = CommitStatus::Success();
  std::function<CommitStatus(
      const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>&)>
      test_commit_hook_;
  std::function<CommitStatusOr<
      std::vector<std::pair<AtomicStateManager*, AtomicCommitResult>>>(
      const std::vector<std::pair<AtomicStateManager*, AtomicCommitArgs>>&)>
      execute_commit_hook_;
};

class NoOpBindable : public PipelineBindable<NoOpBindable> {};

class FakeDrmConnector : public DrmConnector {
 public:
  FakeDrmConnector(DrmDevice* dev, uint32_t encoder_id,
                   bool is_external = false, uint32_t width = 0,
                   uint32_t height = 0,
                   uint32_t connector_id = kFakeConnectorBaseId)
      : DrmConnector(nullptr, dev, 0),
        connector_id_(connector_id),
        encoder_id_(encoder_id),
        is_external_(is_external),
        width_(width),
        height_(height) {
    InitFakeDrmProperty(crtc_id_property_, connector_id_,
                        kFakeConnectorCrtcPropId, "CRTC_ID",
                        DRM_MODE_PROP_OBJECT, 0);
  }

  uint32_t GetId() const override {
    return connector_id_;
  }

  uint32_t GetCurrentEncoderId() const override {
    return encoder_id_;
  }

  bool IsExternal() const override {
    return is_external_;
  }

  bool IsInternal() const override {
    return !is_external_;
  }

  bool IsConnected() const override {
    return connected_;
  }

  void SetConnected(bool connected) {
    connected_ = connected;
  }

  std::string GetName() const override {
    return name_;
  }

  void SetName(std::string name) {
    name_ = std::move(name);
  }

  int UpdateModesAndProperties() override {
    return update_modes_result_;
  }

  void SetUpdateModesResult(int res) {
    update_modes_result_ = res;
  }

  uint32_t GetConnectorType() const override {
    return is_external_ ? DRM_MODE_CONNECTOR_DisplayPort
                        : DRM_MODE_CONNECTOR_eDP;
  }

  uint32_t GetMmWidth() const override {
    return width_;
  }

  uint32_t GetMmHeight() const override {
    return height_;
  }

  bool QueryContentProtectionEnabled() override {
    return content_protection_property_.GetId() != 0 &&
           content_protection_enabled_simulated_;
  }

  void SetContentProtectionEnabled(bool enabled) {
    content_protection_enabled_simulated_ = enabled;
  }

  void SetContentProtectionPropertySupported(bool supported) {
    if (supported) {
      InitFakeDrmProperty(content_protection_property_, connector_id_,
                          kFakeContentProtectionPropId, "Content Protection",
                          DRM_MODE_PROP_ENUM, 0);
    }
  }

  void SetHdcpContentTypePropertySupported(bool supported) {
    if (supported) {
      InitFakeDrmProperty(hdcp_content_type_property_, connector_id_,
                          kFakeHdcpContentTypePropId, "HDCP Content Type",
                          DRM_MODE_PROP_ENUM, 0);
    }
  }

  void SetModes(std::vector<DrmMode> modes) {
    modes_ = std::move(modes);
  }

  void SetCrtcIdPropertyValue(uint64_t value) {
    InitFakeDrmProperty(crtc_id_property_, connector_id_,
                        kFakeConnectorCrtcPropId, "CRTC_ID",
                        DRM_MODE_PROP_OBJECT, value);
  }

 private:
  const uint32_t connector_id_;
  const uint32_t encoder_id_;
  const bool is_external_;
  const uint32_t width_;
  const uint32_t height_;
  bool connected_ = true;
  int update_modes_result_ = 0;
  std::string name_ = "FakeConnector-1";
  bool content_protection_enabled_simulated_ = false;
};

class FakeDrmEncoder : public DrmEncoder {
 public:
  FakeDrmEncoder(uint32_t id, uint32_t crtc_id)
      : DrmEncoder(nullptr, 0), id_(id), crtc_id_(crtc_id) {
  }

  uint32_t GetId() const override {
    return id_;
  }

  uint32_t GetCurrentCrtcId() const override {
    return crtc_id_;
  }

 private:
  const uint32_t id_;
  const uint32_t crtc_id_;
};

class FakeDrmCrtc : public DrmCrtc {
 public:
  explicit FakeDrmCrtc(uint32_t id) : DrmCrtc(nullptr, 0), id_(id) {
    InitFakeDrmProperty(active_property_, id_, kFakeCrtcActivePropId, "ACTIVE",
                        DRM_MODE_PROP_RANGE, 0);
    InitFakeDrmProperty(mode_property_, id_, kFakeCrtcModePropId, "MODE_ID",
                        DRM_MODE_PROP_BLOB, 0);
    InitFakeDrmProperty(out_fence_ptr_property_, id_, kFakeCrtcOutFencePropId,
                        "OUT_FENCE_PTR", DRM_MODE_PROP_RANGE, 0);
  }

  uint32_t GetId() const override {
    return id_;
  }

 private:
  const uint32_t id_;
};

class FakeDrmPlane : public DrmPlane {
 public:
  FakeDrmPlane(DrmDevice& dev, uint32_t type, bool is_valid = true,
               uint32_t plane_id = kFakePlaneBaseId)
      : DrmPlane(dev, nullptr), is_valid_(is_valid), plane_id_(plane_id) {
    type_ = type;
    InitFakeDrmProperty(crtc_property_, plane_id_, kFakePlaneCrtcPropId,
                        "CRTC_ID", DRM_MODE_PROP_OBJECT, 0);
    InitFakeDrmProperty(fb_property_, plane_id_, kFakePlaneFbPropId, "FB_ID",
                        DRM_MODE_PROP_OBJECT, 0);
    InitFakeDrmProperty(crtc_x_property_, plane_id_, kFakePlaneCrtcXPropId,
                        "CRTC_X");
    InitFakeDrmProperty(crtc_y_property_, plane_id_, kFakePlaneCrtcYPropId,
                        "CRTC_Y");
    InitFakeDrmProperty(crtc_w_property_, plane_id_, kFakePlaneCrtcWPropId,
                        "CRTC_W");
    InitFakeDrmProperty(crtc_h_property_, plane_id_, kFakePlaneCrtcHPropId,
                        "CRTC_H");
    InitFakeDrmProperty(src_x_property_, plane_id_, kFakePlaneSrcXPropId,
                        "SRC_X");
    InitFakeDrmProperty(src_y_property_, plane_id_, kFakePlaneSrcYPropId,
                        "SRC_Y");
    InitFakeDrmProperty(src_w_property_, plane_id_, kFakePlaneSrcWPropId,
                        "SRC_W");
    InitFakeDrmProperty(src_h_property_, plane_id_, kFakePlaneSrcHPropId,
                        "SRC_H");
  }

  uint32_t GetId() const override {
    return plane_id_;
  }

  bool IsCrtcSupported(const DrmCrtc& /*crtc*/) const override {
    return true;
  }

  bool IsValidForLayer(const LayerData* /*layer*/) override {
    return is_valid_;
  }

  void SetCrtcIdPropertyValue(uint64_t value) {
    InitFakeDrmProperty(crtc_property_, plane_id_, kFakePlaneCrtcPropId,
                        "CRTC_ID", DRM_MODE_PROP_OBJECT, value);
  }

  void SetFbIdPropertyValue(uint64_t value) {
    InitFakeDrmProperty(fb_property_, plane_id_, kFakePlaneFbPropId, "FB_ID",
                        DRM_MODE_PROP_OBJECT, value);
  }

 private:
  const bool is_valid_;
  const uint32_t plane_id_;
};

class FakeFbIdHandle : public DrmFbIdHandle {
 public:
  explicit FakeFbIdHandle(uint32_t fb_id) : DrmFbIdHandle(fb_id) {
  }
};

class FakeDrmFbImporter : public DrmFbImporter {
 public:
  std::shared_ptr<DrmFbIdHandle> GetOrCreateFbId(BufferInfo* /*bo*/) override {
    ++import_count_;
    if (!allow_import_) {
      return nullptr;
    }
    return std::make_shared<FakeFbIdHandle>(++next_fb_id_);
  }

  void SetAllowImport(bool allow) {
    allow_import_ = allow;
  }

  size_t GetImportCount() const {
    return import_count_;
  }

 private:
  bool allow_import_ = true;
  uint32_t next_fb_id_ = kFakeFbBaseId;
  size_t import_count_ = 0;
};

class FakeDrmDevice : public DrmDevice {
 public:
  explicit FakeDrmDevice(ResourceManager* res_man = nullptr)
      : DrmDevice(res_man, 0) {
    backend_ = std::make_unique<GenericBackend>(*this);
    auto sink = std::make_unique<FakeAtomicCommitSink>();
    fake_sink_ = sink.get();
    atomic_commit_sink_ = std::move(sink);
    auto importer = std::make_unique<FakeDrmFbImporter>();
    fake_importer_ = importer.get();
    drm_fb_importer_ = std::move(importer);
  }

  DrmModeUserPropertyBlobUnique RegisterUserPropertyBlob(
      const void* /*data*/, size_t /*length*/) const override {
    if (!allow_blob_creation_) {
      return {};
    }
    blob_ids_.push_back(++next_blob_id_);
    return {&blob_ids_.back(), [](uint32_t* /*it*/) {}};
  }

  class FakeDumbBufferFd : public PrimeFdsSharedBase {
   public:
    SharedFd fd;
  };

  std::optional<BufferInfo> CreateDumbBuffer(
      uint32_t width, uint32_t height, uint32_t format,
      uint32_t /*prime_fd_flags*/) override {
    if (!allow_dumb_buffer_) {
      return std::nullopt;
    }
    BufferInfo bo{};
    bo.width = width;
    bo.height = height;
    bo.format = format;
    bo.pitches[0] = width * kFakeBytesPerPixel;
    bo.sizes[0] = bo.pitches[0] * height;
    int raw_fd = -1;
    if (std::FILE* tmp = std::tmpfile(); tmp != nullptr) {
      // NOLINTNEXTLINE(misc-include-cleaner)
      raw_fd = fcntl(fileno(tmp), F_DUPFD_CLOEXEC, 0);
      std::fclose(tmp);
    }
    if (raw_fd >= 0 &&
        ftruncate(raw_fd, static_cast<off_t>(bo.sizes[0])) == 0) {
      auto holder = std::make_shared<FakeDumbBufferFd>();
      holder->fd = MakeSharedFd(raw_fd);
      bo.prime_fds[0] = raw_fd;
      bo.fds_shared = std::move(holder);
    } else {
      if (raw_fd >= 0) {
        close(raw_fd);
      }
      bo.prime_fds[0] = 1;
    }
    return bo;
  }

  void SetAllowBlobCreation(bool allow) {
    allow_blob_creation_ = allow;
  }

  void SetAllowDumbBuffer(bool allow) {
    allow_dumb_buffer_ = allow;
  }

  FakeAtomicCommitSink* GetFakeAtomicCommitSink() const {
    return fake_sink_;
  }

  FakeDrmFbImporter* GetFakeFbImporter() const {
    return fake_importer_;
  }

  void SetDefaultFbImporter(std::unique_ptr<DrmFbImporter> importer) {
    drm_fb_importer_ = std::move(importer);
  }

  void AddConnector(std::unique_ptr<DrmConnector> connector) {
    connectors_.emplace_back(std::move(connector));
  }

  void AddEncoder(std::unique_ptr<DrmEncoder> encoder) {
    encoders_.emplace_back(std::move(encoder));
  }

  void AddCrtc(std::unique_ptr<DrmCrtc> crtc) {
    crtcs_.emplace_back(std::move(crtc));
  }

  void AddPlane(std::unique_ptr<DrmPlane> plane) {
    planes_.emplace_back(std::move(plane));
  }

  void AddPipelineResources(bool is_external = false) {
    AddPipelineResourcesWithModes({CreateFakeDrmMode()}, is_external);
  }

  void AddPipelineResourcesWithModes(std::vector<DrmMode> modes,
                                     bool is_external = false) {
    AddEncoder(std::make_unique<FakeDrmEncoder>(/*id=*/next_id_,
                                                /*crtc_id=*/next_id_));
    AddCrtc(std::make_unique<FakeDrmCrtc>(/*id=*/next_id_ + 1));
    auto connector = std::make_unique<FakeDrmConnector>(this,
                                                        /*encoder_id=*/next_id_,
                                                        is_external,
                                                        kFakeConnectorMmWidth,
                                                        kFakeConnectorMmHeight,
                                                        kFakeConnectorBaseId +
                                                            next_id_);
    connector->SetModes(std::move(modes));
    AddConnector(std::move(connector));
    encoders_.back() = std::make_unique<FakeDrmEncoder>(
        /*id=*/next_id_, /*crtc_id=*/next_id_ + 1);
    AddPlane(std::make_unique<FakeDrmPlane>(*this, DRM_PLANE_TYPE_PRIMARY,
                                            /*is_valid=*/true,
                                            kFakePlaneBaseId + next_id_));
    ++next_id_;
  }

 private:
  uint32_t next_id_ = 0;
  mutable uint32_t next_blob_id_ = kFakeBlobBaseId;
  mutable std::deque<uint32_t> blob_ids_;
  bool allow_blob_creation_ = true;
  bool allow_dumb_buffer_ = true;
  FakeAtomicCommitSink* fake_sink_ = nullptr;
  FakeDrmFbImporter* fake_importer_ = nullptr;
};

}  // namespace android::drm_hwcomposer
