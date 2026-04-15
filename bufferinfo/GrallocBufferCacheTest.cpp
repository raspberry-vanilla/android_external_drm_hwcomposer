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

#include <cutils/native_handle.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <utility>

#include "bufferinfo/BufferInfoGetter.h"
#include "bufferinfo/GrallocBufferCache.h"
#include "hwc/HwcBufferCache.h"

using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;

namespace android::drm_hwcomposer {

class MockBufferInfoGetter : public BufferInfoGetter {
 public:
  MOCK_METHOD(std::optional<BufferInfo>, GetBoInfo, (buffer_handle_t),
              (override));
};

class GrallocBufferCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    auto mock_getter = std::make_unique<NiceMock<MockBufferInfoGetter>>();
    mock_getter_ = mock_getter.get();
    BufferInfoGetter::Init(std::move(mock_getter));

    // Default importer that returns nullptr (sufficient for these tests as we
    // focus on caching logic) GrallocBufferCache passes this to HwcBufferCache.
    importer_ = [](BufferInfo&) { return nullptr; };
    cache_ = std::make_unique<GrallocBufferCache>(importer_);
  }

  void TearDown() override {
    BufferInfoGetter::Init(nullptr);
  }

  MockBufferInfoGetter* mock_getter_;
  HwcBufferCache::ImporterCallback importer_;
  std::unique_ptr<GrallocBufferCache> cache_;
};

TEST_F(GrallocBufferCacheTest, HandleNextBuffer_NewHandle_ImportsAndCaches) {
  native_handle dummy_handle = {};
  buffer_handle_t handle = &dummy_handle;

  BufferInfo expected_bi;
  expected_bi.width = 100;
  expected_bi.height = 200;

  EXPECT_CALL(*mock_getter_, GetBoInfo(handle)).WillOnce(Return(expected_bi));

  auto result = cache_->HandleNextBuffer(handle, /*fence_fd=*/{},
                                         /*slot_id=*/0);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->bi.width, 100);
  EXPECT_EQ(result->bi.height, 200);

  // Now verify it's cached by calling with nullopt
  auto cached_result = cache_->HandleNextBuffer(std::nullopt, /*fence_fd=*/{},
                                                /*slot_id=*/0);
  ASSERT_TRUE(cached_result.has_value());
  EXPECT_EQ(cached_result->bi.width, 100);
}

TEST_F(GrallocBufferCacheTest, HandleNextBuffer_GetBoInfoFails_ReturnsNullopt) {
  native_handle dummy_handle = {};
  buffer_handle_t handle = &dummy_handle;

  EXPECT_CALL(*mock_getter_, GetBoInfo(handle)).WillOnce(Return(std::nullopt));

  auto result = cache_->HandleNextBuffer(handle, /*fence_fd=*/{},
                                         /*slot_id=*/0);

  EXPECT_FALSE(result.has_value());
}

TEST_F(GrallocBufferCacheTest, ClearSlot_RemovesEntry) {
  native_handle dummy_handle = {};
  buffer_handle_t handle = &dummy_handle;
  BufferInfo expected_bi;

  EXPECT_CALL(*mock_getter_, GetBoInfo(handle)).WillOnce(Return(expected_bi));

  cache_->HandleNextBuffer(handle, /*fence_fd=*/{}, /*slot_id=*/0);

  cache_->ClearSlot(0);

  auto result = cache_->HandleNextBuffer(std::nullopt, /*fence_fd=*/{},
                                         /*slot_id=*/0);
  EXPECT_FALSE(result.has_value());
}

}  // namespace android::drm_hwcomposer
