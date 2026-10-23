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

#include "backend/sdm/SdmCompositionPlanner.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <SnapHandle.h>              // IWYU pragma: keep
#include <core/buffer_allocator.h>   // IWYU pragma: keep
#include <core/display_interface.h>  // IWYU pragma: keep
#include <core/layer_stack.h>
#include <core/sdm_types.h>
#include <sdm_display_intf_drawcycle.h>
#include <sdm_display_intf_layer_builder.h>
#include <utils/fence.h>

#include "compositor/CompositionPlanner.h"
#include "compositor/CompositorTestUtils.h"
#include "compositor/LayerData.h"
#include "compositor/LayerToPlaneJoiningPlan.h"  // IWYU pragma: keep
#include "hwc/HwcLayer.h"

namespace android::drm_hwcomposer {
namespace {

using ::testing::_;
using ::testing::DoAll;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::NotNull;
using ::testing::Return;
using ::testing::ReturnRef;
using ::testing::SetArgPointee;

constexpr uint64_t kTestSdmDisplayId = 52;
constexpr int64_t kFirstSdmLayerId = 10;
constexpr int64_t kSecondSdmLayerId = 11;
constexpr int64_t kThirdSdmLayerId = 12;
constexpr uint32_t kSparseZOrderHigh = 100;
constexpr int32_t kTestLayerWidth = 1920;
constexpr int32_t kTestLayerHeight = 1080;
constexpr int32_t kSdmCompositionClient = 1;
constexpr int32_t kSdmCompositionDevice = 2;

class MockSdmLayerBuilderIntf : public sdm::SDMDisplayLayerBuilderIntf {
 public:
  MOCK_METHOD(sdm::DisplayError, Init,
              (sdm::BufferAllocator * buffer_allocator, uint64_t disp_id),
              (override));
  MOCK_METHOD(sdm::DisplayError, CreateLayer,
              (uint64_t disp_id, int64_t* out_layer_id), (override));
  MOCK_METHOD(sdm::DisplayError, DestroyLayer,
              (uint64_t disp_id, int64_t layer_id), (override));
  MOCK_METHOD(sdm::DisplayError, SetLayerBuffer,
              (uint64_t disp_id, int64_t layer_id,
               vendor::qti::hardware::display::snapalloc::SnapHandle* buffer,
               std::shared_ptr<sdm::Fence> acquire_fence),
              (override));
  MOCK_METHOD(sdm::DisplayError, SetLayerBlendMode,
              (uint64_t disp_id, int64_t layer_id,
               sdm::LayerBlending blend_mode),
              (override));
  MOCK_METHOD(sdm::DisplayError, SetLayerDisplayFrame,
              (uint64_t disp_id, int64_t layer_id, sdm::SDMRect frame),
              (override));
  MOCK_METHOD(sdm::DisplayError, SetLayerColor,
              (uint64_t disp_id, int64_t layer_id, sdm::SDMColor color),
              (override));
  MOCK_METHOD(sdm::DisplayError, SetLayerPlaneAlpha,
              (uint64_t disp_id, int64_t layer_id, float alpha), (override));
  MOCK_METHOD(sdm::DisplayError, SetLayerSourceCrop,
              (uint64_t disp_id, int64_t layer_id, sdm::SDMRect crop),
              (override));
  MOCK_METHOD(sdm::DisplayError, SetLayerTransform,
              (uint64_t disp_id, int64_t layer_id, sdm::SDMTransform transform),
              (override));
  MOCK_METHOD(sdm::DisplayError, SetLayerZOrder,
              (uint64_t disp_id, int64_t layer_id, uint32_t z_order),
              (override));
  MOCK_METHOD(sdm::DisplayError, SetLayerCompositionType,
              (uint64_t disp_id, int64_t layer_id, int32_t type), (override));
  MOCK_METHOD(sdm::DisplayError, SetLayerDataspace,
              (uint64_t disp_id, int64_t layer_id, int32_t dataspace),
              (override));
  MOCK_METHOD(sdm::DisplayError, SetLayerBrightness,
              (uint64_t disp_id, int64_t layer_id, float brightness),
              (override));
};

class MockSdmDrawCycleIntf : public sdm::SDMDisplayDrawCycleIntf {
 public:
  MOCK_METHOD(sdm::DisplayError, SetActiveConfigIndex,
              (uint64_t disp_id, uint32_t index), (override));
  MOCK_METHOD(sdm::DisplayError, PresentDisplay,
              (uint64_t disp_id, std::shared_ptr<sdm::Fence>* out_retire_fence),
              (override));
  MOCK_METHOD(sdm::DisplayError, SetClientTarget,
              (uint64_t disp_id, const sdm::SnapHandle* target,
               std::shared_ptr<sdm::Fence> acquire_fence, int32_t dataspace,
               sdm::SDMRegion damage, uint32_t version),
              (override));
  MOCK_METHOD(sdm::DisplayError, WaitForDrawCycleToComplete, (uint64_t disp_id),
              (override));
  MOCK_METHOD(sdm::DisplayError, LayerStackUpdated, (uint64_t disp_id),
              (override));
  MOCK_METHOD(sdm::DisplayError, CommitOrPrepare,
              (uint64_t disp_id, bool validate_only,
               std::shared_ptr<sdm::Fence>* out_retire_fence,
               uint32_t* out_num_types, uint32_t* out_num_requests,
               bool* out_needs_commit),
              (override));
  MOCK_METHOD(sdm::DisplayError, AcceptDisplayChanges, (uint64_t disp_id),
              (override));
  MOCK_METHOD(sdm::DisplayError, GetChangedCompositionTypes,
              (uint64_t disp_id, uint32_t* out_num_elements,
               int64_t* out_layers, int32_t* out_types),
              (override));
  MOCK_METHOD(sdm::DisplayError, GetDisplayRequests,
              (uint64_t disp_id, int32_t* out_display_requests,
               uint32_t* out_num_elements, sdm::LayerId* out_layers,
               int32_t* out_layer_requests),
              (override));
};

struct SdmCompositionPlannerTest : public ::testing::Test {
  void SetUp() override {
    ON_CALL(display, CtmByGpu()).WillByDefault(Return(false));
    ON_CALL(display, GetClientLayer()).WillByDefault(ReturnRef(client_layer));
    ON_CALL(layer_intf, CreateLayer(kTestSdmDisplayId, NotNull()))
        .WillByDefault(Invoke([this](uint64_t /*disp_id*/, int64_t* out_id) {
          *out_id = next_sdm_layer_id++;
          return sdm::kErrorNone;
        }));
    ON_CALL(layer_intf, DestroyLayer(kTestSdmDisplayId, _))
        .WillByDefault(Return(sdm::kErrorNone));
    ON_CALL(draw_cycle_intf, CommitOrPrepare(kTestSdmDisplayId, _, _, _, _, _))
        .WillByDefault(Return(sdm::kErrorNeedsCommit));
    ON_CALL(draw_cycle_intf, AcceptDisplayChanges(kTestSdmDisplayId))
        .WillByDefault(Return(sdm::kErrorNone));
    ON_CALL(draw_cycle_intf,
            GetChangedCompositionTypes(kTestSdmDisplayId, NotNull(), _, _))
        .WillByDefault(DoAll(SetArgPointee<1>(0U), Return(sdm::kErrorNone)));
    ON_CALL(draw_cycle_intf,
            GetDisplayRequests(kTestSdmDisplayId, NotNull(), NotNull(), _, _))
        .WillByDefault(DoAll(SetArgPointee<1>(0), SetArgPointee<2>(0U),
                             Return(sdm::kErrorNone)));

    planner = std::make_unique<SdmCompositionPlanner>(kTestSdmDisplayId,
                                                      &layer_intf,
                                                      &draw_cycle_intf);
  }

  NiceMock<MockCompositorDisplay> display;
  NiceMock<MockSdmLayerBuilderIntf> layer_intf;
  NiceMock<MockSdmDrawCycleIntf> draw_cycle_intf;
  HwcLayer client_layer{nullptr};
  int64_t next_sdm_layer_id = kFirstSdmLayerId;
  std::unique_ptr<SdmCompositionPlanner> planner;
};

TEST_F(SdmCompositionPlannerTest,
       ValidateDeviceLayerCreatesSdmLayerAndBuildsDevicePlan) {
  const IRect full_frame{0, 0, kTestLayerWidth, kTestLayerHeight};
  HwcLayer layer1 = CompositorTestUtils::CreateLayer(&display, full_frame, 0,
                                                     CompositionType::kDevice);
  std::vector<const HwcLayer*> layers = {&layer1};
  ON_CALL(display, GetOrderLayersByZPos()).WillByDefault(Return(layers));

  EXPECT_CALL(layer_intf, CreateLayer(kTestSdmDisplayId, NotNull())).Times(1);
  EXPECT_CALL(layer_intf,
              SetLayerCompositionType(kTestSdmDisplayId, kFirstSdmLayerId,
                                      kSdmCompositionDevice))
      .Times(1)
      .WillOnce(Return(sdm::kErrorNone));
  EXPECT_CALL(draw_cycle_intf, LayerStackUpdated(kTestSdmDisplayId))
      .Times(1)
      .WillOnce(Return(sdm::kErrorNone));
  EXPECT_CALL(draw_cycle_intf,
              CommitOrPrepare(kTestSdmDisplayId, true, _, _, _, _))
      .Times(1)
      .WillOnce(Return(sdm::kErrorNeedsCommit));
  EXPECT_CALL(draw_cycle_intf, AcceptDisplayChanges(kTestSdmDisplayId))
      .Times(1)
      .WillOnce(Return(sdm::kErrorNone));

  auto result = planner->ValidateDisplay(&display);
  EXPECT_FALSE(result.short_circuited);
  EXPECT_EQ(result.composition.flatten_reason,
            CompositionPlanner::FlattenReason::kNone);
  ASSERT_EQ(result.composition.composition_types.size(), 1U);
  EXPECT_EQ(result.composition.composition_types[&layer1],
            CompositionType::kDevice);
  ASSERT_NE(result.composition.composition_plan, nullptr);
  EXPECT_EQ(result.composition.composition_plan->plan.size(), 1U);
  EXPECT_FALSE(result.composition.composition_plan->client_z_order.has_value());
}

TEST_F(SdmCompositionPlannerTest,
       ValidateAppliesChangedCompositionTypesFromSdm) {
  const IRect full_frame{0, 0, kTestLayerWidth, kTestLayerHeight};
  HwcLayer layer1 = CompositorTestUtils::CreateLayer(&display, full_frame, 0,
                                                     CompositionType::kDevice);
  std::vector<const HwcLayer*> layers = {&layer1};
  ON_CALL(display, GetOrderLayersByZPos()).WillByDefault(Return(layers));

  EXPECT_CALL(draw_cycle_intf,
              GetChangedCompositionTypes(kTestSdmDisplayId, NotNull(), nullptr,
                                         nullptr))
      .WillOnce(DoAll(SetArgPointee<1>(1U), Return(sdm::kErrorNone)));
  EXPECT_CALL(draw_cycle_intf,
              GetChangedCompositionTypes(kTestSdmDisplayId, NotNull(),
                                         NotNull(), NotNull()))
      .WillOnce(DoAll(SetArgPointee<1>(1U), SetArgPointee<2>(kFirstSdmLayerId),
                      SetArgPointee<3>(kSdmCompositionClient),
                      Return(sdm::kErrorNone)));

  auto result = planner->ValidateDisplay(&display);
  ASSERT_EQ(result.composition.composition_types.size(), 1U);
  EXPECT_EQ(result.composition.composition_types[&layer1],
            CompositionType::kClient);
  ASSERT_NE(result.composition.composition_plan, nullptr);
  ASSERT_TRUE(result.composition.composition_plan->client_z_order.has_value());
  EXPECT_EQ(result.composition.composition_plan->client_z_order.value_or(-1),
            0);
}

TEST_F(SdmCompositionPlannerTest,
       ValidateAssignsDenseSequentialZOrderForDuplicateOrSparseLayerZPos) {
  const IRect full_frame{0, 0, kTestLayerWidth, kTestLayerHeight};
  HwcLayer layer1 = CompositorTestUtils::CreateLayer(&display, full_frame, 0,
                                                     CompositionType::kDevice);
  HwcLayer layer2 = CompositorTestUtils::CreateLayer(&display, full_frame, 0,
                                                     CompositionType::kDevice);
  HwcLayer layer3 = CompositorTestUtils::CreateLayer(&display, full_frame,
                                                     kSparseZOrderHigh,
                                                     CompositionType::kDevice);
  std::vector<const HwcLayer*> layers = {&layer1, &layer2, &layer3};
  ON_CALL(display, GetOrderLayersByZPos()).WillByDefault(Return(layers));

  EXPECT_CALL(layer_intf,
              SetLayerZOrder(kTestSdmDisplayId, kFirstSdmLayerId, 0U))
      .Times(1)
      .WillOnce(Return(sdm::kErrorNone));
  EXPECT_CALL(layer_intf,
              SetLayerZOrder(kTestSdmDisplayId, kSecondSdmLayerId, 1U))
      .Times(1)
      .WillOnce(Return(sdm::kErrorNone));
  EXPECT_CALL(layer_intf,
              SetLayerZOrder(kTestSdmDisplayId, kThirdSdmLayerId, 2U))
      .Times(1)
      .WillOnce(Return(sdm::kErrorNone));

  auto result = planner->ValidateDisplay(&display);
  EXPECT_EQ(result.composition.flatten_reason,
            CompositionPlanner::FlattenReason::kNone);
}

}  // namespace
}  // namespace android::drm_hwcomposer
