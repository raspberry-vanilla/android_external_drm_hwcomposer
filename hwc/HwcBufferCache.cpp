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

#include "HwcBufferCache.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "bufferinfo/BufferInfo.h"
#include "drm/DrmFbImporter.h"

namespace android::drm_hwcomposer {

HwcBufferCache::HwcBufferCache(ImporterCallback importer)
    : importer_(std::move(importer)) {
}

void HwcBufferCache::SetSlot(int32_t slot_id,
                             const std::optional<BufferInfo>& bi) {
  if (bi == std::nullopt) {
    slots_.erase(slot_id);
    return;
  }
  slots_[slot_id] = {
      .bi = bi.value(),
      .fb = {},
  };
  // Do not log ALOGE when ImportFb returns false: importer_ legitimately
  // returns nullptr in headless mode and on backends without DRM FB import
  // (such as SdmBackend's StubFbImporter), while DrmFbCachedImporter logs
  // specific DRM errors directly.
  ImportFb(slots_[slot_id]);
}

std::optional<BufferInfo> HwcBufferCache::GetBufferInfo(int32_t slot_id) {
  auto* slot_entry = GetSlot(slot_id);
  if (slot_entry == nullptr) {
    return std::nullopt;
  }
  return slot_entry->bi;
}

std::shared_ptr<DrmFbIdHandle> HwcBufferCache::GetFb(int32_t slot_id) {
  auto* slot_entry = GetSlot(slot_id);
  if (slot_entry == nullptr) {
    return nullptr;
  }
  // Re-attempt ImportFb on the cached slot entry so slots populated while in
  // headless mode (where importer_ returns nullptr) are lazily imported once a
  // physical pipeline is attached.
  if (!ImportFb(*slot_entry)) {
    return nullptr;
  }
  return slot_entry->fb;
}

void HwcBufferCache::Clear() {
  slots_.clear();
}

bool HwcBufferCache::ImportFb(BufferSlot& slot) const {
  if (slot.fb == nullptr) {
    slot.fb = importer_(slot.bi);
  }
  return slot.fb != nullptr;
}

HwcBufferCache::BufferSlot* HwcBufferCache::GetSlot(int32_t slot_id) {
  auto it = slots_.find(slot_id);
  if (it == slots_.end()) {
    return nullptr;
  }
  return &it->second;
}

}  // namespace android::drm_hwcomposer
