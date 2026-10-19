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

#include <cutils/native_handle.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "bufferinfo/BufferInfoGetter.h"
#include "bufferinfo/GrallocBufferCache.h"
#include "drm/DrmFbImporter.h"
#include "hwc/HwcBufferCache.h"

using ::testing::NiceMock;
using ::testing::Return;

namespace android::drm_hwcomposer {

namespace {

constexpr uint32_t kTestGrallocFbId = 888;
constexpr uint16_t kTestGrallocWidth = 1920;
constexpr uint16_t kTestGrallocHeight = 1080;

class TestGrallocFbIdHandle : public DrmFbIdHandle {
 public:
  explicit TestGrallocFbIdHandle(uint32_t fb_id) : DrmFbIdHandle(fb_id) {
  }
};

}  // namespace

class MockBufferInfoGetter : public BufferInfoGetter {
 public:
  MOCK_METHOD(std::optional<BufferInfo>, GetBoInfo, (buffer_handle_t),
              (override));
};

struct GrallocBufferCacheTest : public ::testing::Test {
  void SetUp() override {
    auto mock = std::make_unique<NiceMock<MockBufferInfoGetter>>();
    mock_getter = mock.get();
    BufferInfoGetter::Init(std::move(mock));

    // Default importer that returns nullptr (sufficient for these tests as we
    // focus on caching logic) GrallocBufferCache passes this to HwcBufferCache.
    importer = [](BufferInfo&) { return nullptr; };
    cache = std::make_unique<GrallocBufferCache>(importer);
  }

  void TearDown() override {
    BufferInfoGetter::Init(nullptr);
  }

  MockBufferInfoGetter* mock_getter = nullptr;
  HwcBufferCache::ImporterCallback importer;
  std::unique_ptr<GrallocBufferCache> cache;
};

TEST_F(GrallocBufferCacheTest, HandleNextBufferNewHandleImportsAndCaches) {
  // NOLINTBEGIN(readability-magic-numbers)
  native_handle dummy_handle = {};
  buffer_handle_t handle = &dummy_handle;

  BufferInfo expected_bi;
  expected_bi.width = 100;
  expected_bi.height = 200;

  EXPECT_CALL(*mock_getter, GetBoInfo(handle)).WillOnce(Return(expected_bi));

  auto result = cache->HandleNextBuffer(handle, /*fence_fd=*/{},
                                        /*slot_id=*/0);

  ASSERT_TRUE(result.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  const auto& result_val = result.value();
  EXPECT_EQ(result_val.bi.width, 100);
  EXPECT_EQ(result_val.bi.height, 200);

  // Now verify it's cached by calling with nullopt
  auto cached_result = cache->HandleNextBuffer(std::nullopt, /*fence_fd=*/{},
                                               /*slot_id=*/0);
  ASSERT_TRUE(cached_result.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  EXPECT_EQ(cached_result->bi.width, 100);
  // NOLINTEND(readability-magic-numbers)
}

TEST_F(GrallocBufferCacheTest, HandleNextBufferGetBoInfoFailsReturnsNullopt) {
  native_handle dummy_handle = {};
  buffer_handle_t handle = &dummy_handle;

  EXPECT_CALL(*mock_getter, GetBoInfo(handle)).WillOnce(Return(std::nullopt));

  auto result = cache->HandleNextBuffer(handle, /*fence_fd=*/{},
                                        /*slot_id=*/0);

  EXPECT_FALSE(result.has_value());
}

TEST_F(GrallocBufferCacheTest, ClearSlotRemovesEntry) {
  native_handle dummy_handle = {};
  buffer_handle_t handle = &dummy_handle;
  BufferInfo expected_bi;

  EXPECT_CALL(*mock_getter, GetBoInfo(handle)).WillOnce(Return(expected_bi));

  cache->HandleNextBuffer(handle, /*fence_fd=*/{}, /*slot_id=*/0);

  cache->ClearSlot(0);

  auto result = cache->HandleNextBuffer(std::nullopt, /*fence_fd=*/{},
                                        /*slot_id=*/0);
  EXPECT_FALSE(result.has_value());
}

TEST_F(GrallocBufferCacheTest,
       HandleNextBufferReimportsCachedSlotAfterInitialNullFb) {
  bool allow_import = false;
  cache = std::make_unique<GrallocBufferCache>(
      [&](BufferInfo&) -> std::shared_ptr<DrmFbIdHandle> {
        if (!allow_import) {
          return nullptr;
        }
        return std::make_shared<TestGrallocFbIdHandle>(kTestGrallocFbId);
      });

  native_handle dummy_handle = {};
  buffer_handle_t handle = &dummy_handle;
  BufferInfo expected_bi{};
  expected_bi.width = kTestGrallocWidth;
  expected_bi.height = kTestGrallocHeight;

  EXPECT_CALL(*mock_getter, GetBoInfo(handle)).WillOnce(Return(expected_bi));

  // First frame arrives while display is headless: BufferInfo is cached in
  // slot 0, but fb import returns nullptr.
  auto initial_result = cache->HandleNextBuffer(handle, /*fence_fd=*/{},
                                                /*slot_id=*/0);
  ASSERT_TRUE(initial_result.has_value());
  if (!initial_result.has_value()) {
    return;
  }
  EXPECT_EQ(initial_result->fb, nullptr);

  // Display attaches a physical pipeline; next frame reuses cached slot 0
  // (raw_handle == std::nullopt).
  allow_import = true;
  auto cached_result = cache->HandleNextBuffer(std::nullopt, /*fence_fd=*/{},
                                               /*slot_id=*/0);
  ASSERT_TRUE(cached_result.has_value());
  if (!cached_result.has_value()) {
    return;
  }
  ASSERT_NE(cached_result->fb, nullptr);
  EXPECT_EQ(cached_result->fb->GetFbId(), kTestGrallocFbId);
}

}  // namespace android::drm_hwcomposer
