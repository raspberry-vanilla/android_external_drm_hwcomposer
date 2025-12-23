/*
 * Copyright (C) 2025 The Android Open Source Project
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
#include <functional>
#include <map>
#include <memory>
#include <optional>

#include "bufferinfo/BufferInfo.h"

namespace android::drm_hwcomposer {

class DrmFbIdHandle;
class DrmFbImporter;

class HwcBufferCache {
 public:
  using ImporterCallback = std::function<std::shared_ptr<DrmFbIdHandle>(
      BufferInfo&)>;

  explicit HwcBufferCache(ImporterCallback importer);
  void SetSlot(int32_t slot_id, const std::optional<BufferInfo>& bi);
  std::optional<BufferInfo> GetBufferInfo(int32_t slot_id) const;
  std::shared_ptr<DrmFbIdHandle> GetFb(int32_t slot_id) const;
  void Clear();

 private:
  struct BufferSlot {
    BufferInfo bi;
    std::shared_ptr<DrmFbIdHandle> fb;
  };

  bool ImportFb(BufferSlot& slot) const;
  std::optional<BufferSlot> GetSlot(int32_t slot_id) const;

  std::map<int32_t /*slot_id*/, BufferSlot> slots_;
  ImporterCallback importer_;
};

}  // namespace android::drm_hwcomposer