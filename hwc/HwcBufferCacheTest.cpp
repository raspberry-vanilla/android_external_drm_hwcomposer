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

#include <gtest/gtest.h>

#include <memory>
#include <optional>

#include "bufferinfo/BufferInfo.h"
#include "hwc/HwcBufferCache.h"

namespace android::drm_hwcomposer {

struct HwcBufferCacheTest : public ::testing::Test {
  void SetUp() override {
    // Default importer that returns nullptr
    importer = [](BufferInfo&) { return nullptr; };
    cache = std::make_unique<HwcBufferCache>(importer);
  }

  HwcBufferCache::ImporterCallback importer;
  std::unique_ptr<HwcBufferCache> cache;
};

TEST_F(HwcBufferCacheTest, SetSlotStoresBufferInfo) {
  // NOLINTBEGIN(readability-magic-numbers)
  BufferInfo bi;
  bi.width = 100;
  bi.height = 200;

  cache->SetSlot(0, bi);

  auto retrieved_bi_opt = cache->GetBufferInfo(0);
  ASSERT_TRUE(retrieved_bi_opt.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const auto& retrieved_bi = retrieved_bi_opt.value();
  EXPECT_EQ(retrieved_bi.width, 100);
  EXPECT_EQ(retrieved_bi.height, 200);
  // NOLINTEND(readability-magic-numbers)
}

TEST_F(HwcBufferCacheTest, SetSlotCallsImporter) {
  // NOLINTBEGIN(readability-magic-numbers)
  BufferInfo bi;
  bi.width = 100;

  bool importer_called = false;
  cache = std::make_unique<HwcBufferCache>([&](BufferInfo& b) {
    importer_called = true;
    EXPECT_EQ(b.width, 100);
    return nullptr;
  });

  cache->SetSlot(1, bi);
  EXPECT_TRUE(importer_called);
  // NOLINTEND(readability-magic-numbers)
}

TEST_F(HwcBufferCacheTest, ClearRemovesEntries) {
  BufferInfo bi;
  cache->SetSlot(0, bi);
  ASSERT_TRUE(cache->GetBufferInfo(0).has_value());

  cache->Clear();
  EXPECT_FALSE(cache->GetBufferInfo(0).has_value());
}

TEST_F(HwcBufferCacheTest, SetSlotNulloptRemovesEntry) {
  BufferInfo bi;
  cache->SetSlot(0, bi);
  ASSERT_TRUE(cache->GetBufferInfo(0).has_value());

  cache->SetSlot(0, std::nullopt);
  EXPECT_FALSE(cache->GetBufferInfo(0).has_value());
}

}  // namespace android::drm_hwcomposer