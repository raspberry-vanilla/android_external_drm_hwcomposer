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

#include <cstdint>
#include <memory>
#include <optional>

#include "bufferinfo/BufferInfo.h"
#include "drm/DrmFbImporter.h"
#include "hwc/HwcBufferCache.h"

namespace android::drm_hwcomposer {

namespace {

constexpr uint32_t kTestFbId = 777;
constexpr uint16_t kTestBufferWidth = 1920;
constexpr uint16_t kTestBufferHeight = 1080;

class TestFbIdHandle : public DrmFbIdHandle {
 public:
  explicit TestFbIdHandle(uint32_t fb_id) : DrmFbIdHandle(fb_id) {
  }
};

}  // namespace

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

TEST_F(HwcBufferCacheTest, GetFbReimportsWhenInitialImportReturnedNullptr) {
  bool allow_import = false;
  int import_attempts = 0;
  cache = std::make_unique<HwcBufferCache>(
      [&](BufferInfo&) -> std::shared_ptr<DrmFbIdHandle> {
        ++import_attempts;
        if (!allow_import) {
          return nullptr;
        }
        return std::make_shared<TestFbIdHandle>(kTestFbId);
      });

  BufferInfo bi{};
  bi.width = kTestBufferWidth;
  bi.height = kTestBufferHeight;

  // Initial slot population while in headless mode (importer returns nullptr).
  cache->SetSlot(0, bi);
  EXPECT_EQ(import_attempts, 1);
  EXPECT_EQ(cache->GetFb(0), nullptr);
  EXPECT_EQ(import_attempts, 2);

  // Transition to physical pipeline (importer now succeeds). Accessing the
  // cached slot via GetFb(0) must re-import rather than stay poisoned with
  // nullptr, and subsequent GetFb(0) calls must reuse the cached handle.
  allow_import = true;
  auto fb = cache->GetFb(0);
  ASSERT_NE(fb, nullptr);
  EXPECT_EQ(fb->GetFbId(), kTestFbId);
  EXPECT_EQ(import_attempts, 3);
  EXPECT_EQ(cache->GetFb(0), fb);
  EXPECT_EQ(import_attempts, 3);
}

TEST_F(HwcBufferCacheTest,
       GetFbReturnsNullptrRepeatedlyWhenImporterAlwaysFails) {
  int import_attempts = 0;
  cache = std::make_unique<HwcBufferCache>(
      [&](BufferInfo&) -> std::shared_ptr<DrmFbIdHandle> {
        ++import_attempts;
        return nullptr;
      });

  BufferInfo bi{};
  bi.width = kTestBufferWidth;
  bi.height = kTestBufferHeight;

  cache->SetSlot(0, bi);
  EXPECT_EQ(import_attempts, 1);
  EXPECT_EQ(cache->GetFb(0), nullptr);
  EXPECT_EQ(import_attempts, 2);
  EXPECT_EQ(cache->GetFb(0), nullptr);
  EXPECT_EQ(import_attempts, 3);
}

}  // namespace android::drm_hwcomposer
