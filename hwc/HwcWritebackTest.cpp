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

#include "hwc/HwcWriteback.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <utility>

#include <android-base/file.h>
#include <drm/drm_fourcc.h>

#include "bufferinfo/BufferInfo.h"
#include "drm/DrmConnector.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmFbImporter.h"
#include "drm/DrmTestUtils.h"
#include "utils/fd.h"

namespace android::drm_hwcomposer {
namespace {

constexpr uint16_t kTestWidth = 1920;
constexpr uint16_t kTestHeight = 1080;
constexpr uint32_t kBytesPerPixel = 4;
constexpr uint32_t kTestFbId = 700;

BufferInfo CreateTestBufferInfo() {
  BufferInfo bo{};
  bo.width = kTestWidth;
  bo.height = kTestHeight;
  bo.format = DRM_FORMAT_ABGR8888;
  bo.pitches[0] = kTestWidth * kBytesPerPixel;
  bo.prime_fds[0] = 1;
  return bo;
}

}  // namespace

TEST(HwcWritebackTest, ConstructWithNullConnectorAndValidBuffer) {
  TemporaryFile in_fence_tf;
  const SharedFd in_fence = MakeSharedFd(in_fence_tf.release());
  const auto fb = std::make_shared<FakeFbIdHandle>(kTestFbId);

  WritebackBuffer wb_buffer{
      .bi = CreateTestBufferInfo(),
      .fb = fb,
      .fence = in_fence,
  };

  HwcWriteback writeback(/*connector=*/nullptr, wb_buffer);
  EXPECT_EQ(writeback.GetConnector(), nullptr);
  EXPECT_FALSE(writeback.IsBufferConsumed());
  const auto &staged_buffer = writeback.GetBuffer();
  if (!staged_buffer.has_value()) {
    ADD_FAILURE() << "Expected staged writeback buffer";
    return;
  }
  EXPECT_EQ(staged_buffer->fb, fb);
  EXPECT_EQ(staged_buffer->fence, in_fence);
  EXPECT_EQ(writeback.TakeOutFence(), nullptr);
}

TEST(HwcWritebackTest, SetOutFenceConsumesBufferAndReleasesResourcesRaii) {
  TemporaryFile in_fence_tf;
  TemporaryFile out_fence_tf;

  std::weak_ptr<IDrmFbIdHandle> weak_fb;
  std::weak_ptr<int> weak_in_fence;

  std::unique_ptr<HwcWriteback> writeback;
  {
    auto fb = std::make_shared<FakeFbIdHandle>(kTestFbId);
    SharedFd in_fence = MakeSharedFd(in_fence_tf.release());
    weak_fb = fb;
    weak_in_fence = in_fence;

    WritebackBuffer wb_buffer{
        .bi = CreateTestBufferInfo(),
        .fb = std::move(fb),
        .fence = std::move(in_fence),
    };
    writeback = std::make_unique<HwcWriteback>(/*connector=*/nullptr,
                                               std::move(wb_buffer));
  }

  ASSERT_NE(writeback, nullptr);
  EXPECT_FALSE(writeback->IsBufferConsumed());
  EXPECT_FALSE(weak_fb.expired());
  EXPECT_FALSE(weak_in_fence.expired());

  // Setting the out-fence consumes the WritebackBuffer and immediately drops
  // HwcWriteback's references to the framebuffer and in-fence via RAII.
  SharedFd out_fence = MakeSharedFd(out_fence_tf.release());
  std::weak_ptr<int> weak_out_fence = out_fence;
  writeback->SetOutFence(std::move(out_fence));

  EXPECT_TRUE(writeback->IsBufferConsumed());
  EXPECT_FALSE(writeback->GetBuffer().has_value());
  EXPECT_TRUE(weak_fb.expired());
  EXPECT_TRUE(weak_in_fence.expired());
  EXPECT_FALSE(weak_out_fence.expired());

  // TakeOutFence transfers ownership of the completion fence to the caller.
  SharedFd taken_fence = writeback->TakeOutFence();
  ASSERT_NE(taken_fence, nullptr);
  EXPECT_GE(*taken_fence, 0);
  EXPECT_EQ(writeback->TakeOutFence(), nullptr);

  taken_fence.reset();
  EXPECT_TRUE(weak_out_fence.expired());
}

TEST(HwcWritebackTest, OneShotConnectorUnbindsOnDestruction) {
  auto fake_device = std::make_unique<FakeDrmDevice>();
  fake_device->AddPipelineResources(/*is_external=*/true);
  auto pipeline = DrmDisplayPipeline::CreatePipeline(
      *fake_device->GetConnectors().back());
  ASSERT_NE(pipeline, nullptr);

  fake_device->AddWritebackConnector(pipeline->encoder->Get()->GetId());
  DrmConnector *wb_conn = pipeline->FindWritebackConnectorForPipeline();
  ASSERT_NE(wb_conn, nullptr);
  EXPECT_EQ(wb_conn->GetPipeline(), nullptr);

  {
    auto bound_conn = wb_conn->BindPipeline(pipeline.get());
    ASSERT_NE(bound_conn, nullptr);
    EXPECT_EQ(wb_conn->GetPipeline(), pipeline.get());

    WritebackBuffer wb_buffer{
        .bi = CreateTestBufferInfo(),
        .fb = std::make_shared<FakeFbIdHandle>(kTestFbId),
        .fence = {},
    };
    HwcWriteback writeback(std::move(bound_conn), std::move(wb_buffer));
    EXPECT_EQ(writeback.GetConnector(), wb_conn);
    EXPECT_EQ(wb_conn->GetPipeline(), pipeline.get());
  }

  // Destroying HwcWriteback releases the sole BindingOwner<DrmConnector> and
  // unbinds the writeback connector via RAII.
  EXPECT_EQ(wb_conn->GetPipeline(), nullptr);
}

TEST(HwcWritebackTest,
     SharedVirtualDisplayConnectorRemainsBoundAfterDestruction) {
  auto fake_device = std::make_unique<FakeDrmDevice>();
  fake_device->AddPipelineResources(/*is_external=*/true);
  fake_device->AddWritebackConnector(
      fake_device->GetEncoders().back()->GetId());
  DrmConnector *wb_conn = fake_device->GetWritebackConnectors().back().get();
  ASSERT_NE(wb_conn, nullptr);

  auto virtual_pipeline = DrmDisplayPipeline::CreatePipeline(*wb_conn);
  ASSERT_NE(virtual_pipeline, nullptr);
  ASSERT_NE(virtual_pipeline->connector, nullptr);
  EXPECT_EQ(wb_conn->GetPipeline(), virtual_pipeline.get());

  {
    WritebackBuffer wb_buffer{
        .bi = CreateTestBufferInfo(),
        .fb = std::make_shared<FakeFbIdHandle>(kTestFbId),
        .fence = {},
    };
    HwcWriteback writeback(virtual_pipeline->connector, std::move(wb_buffer));
    EXPECT_EQ(writeback.GetConnector(), wb_conn);
    EXPECT_EQ(wb_conn->GetPipeline(), virtual_pipeline.get());
  }

  // Because virtual_pipeline->connector retains ownership of the BindingOwner,
  // destroying HwcWriteback leaves the connector bound to the virtual pipeline.
  EXPECT_EQ(wb_conn->GetPipeline(), virtual_pipeline.get());
}

}  // namespace android::drm_hwcomposer
