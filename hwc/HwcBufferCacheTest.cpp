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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <optional>

#include "bufferinfo/BufferInfo.h"
#include "hwc/HwcBufferCache.h"

using ::testing::_;
using ::testing::Invoke;
using ::testing::Return;

namespace android::drm_hwcomposer {

class HwcBufferCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Default importer that returns nullptr
    importer_ = [](BufferInfo&) { return nullptr; };
    cache_ = std::make_unique<HwcBufferCache>(importer_);
  }

  HwcBufferCache::ImporterCallback importer_;
  std::unique_ptr<HwcBufferCache> cache_;
};

TEST_F(HwcBufferCacheTest, SetSlot_StoresBufferInfo) {
  BufferInfo bi;
  bi.width = 100;
  bi.height = 200;

  cache_->SetSlot(0, bi);

  auto retrieved_bi = cache_->GetBufferInfo(0);
  ASSERT_TRUE(retrieved_bi.has_value());
  EXPECT_EQ(retrieved_bi->width, 100);
  EXPECT_EQ(retrieved_bi->height, 200);
}

TEST_F(HwcBufferCacheTest, SetSlot_CallsImporter) {
  BufferInfo bi;
  bi.width = 100;

  bool importer_called = false;
  cache_ = std::make_unique<HwcBufferCache>([&](BufferInfo& b) {
    importer_called = true;
    EXPECT_EQ(b.width, 100);
    return nullptr;
  });

  cache_->SetSlot(1, bi);
  EXPECT_TRUE(importer_called);
}

TEST_F(HwcBufferCacheTest, Clear_RemovesEntries) {
  BufferInfo bi;
  cache_->SetSlot(0, bi);
  ASSERT_TRUE(cache_->GetBufferInfo(0).has_value());

  cache_->Clear();
  EXPECT_FALSE(cache_->GetBufferInfo(0).has_value());
}

TEST_F(HwcBufferCacheTest, SetSlot_Nullopt_RemovesEntry) {
  BufferInfo bi;
  cache_->SetSlot(0, bi);
  ASSERT_TRUE(cache_->GetBufferInfo(0).has_value());

  cache_->SetSlot(0, std::nullopt);
  EXPECT_FALSE(cache_->GetBufferInfo(0).has_value());
}

}  // namespace android::drm_hwcomposer