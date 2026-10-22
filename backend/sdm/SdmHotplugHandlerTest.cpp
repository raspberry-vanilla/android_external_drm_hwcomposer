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

#include "backend/sdm/SdmHotplugHandler.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <core/buffer_allocator.h>  // IWYU pragma: keep
#include <core/sdm_types.h>
#include <core/socket_handler.h>              // IWYU pragma: keep
#include <sdm_compositor_cb_intf.h>           // IWYU pragma: keep
#include <sdm_compositor_sideband_cb_intf.h>  // IWYU pragma: keep
#include <sdm_display_intf_caps.h>
#include <sdm_display_intf_lifecycle.h>

#include "backend/SdmBackend.h"
#include "backend/sdm/SdmToConnectorMapper.h"
#include "drm/DrmTestUtils.h"

namespace android::drm_hwcomposer {
namespace {

using ::testing::_;
using ::testing::DoAll;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SetArgPointee;

constexpr uint64_t kTestSdmDisplayId = 52;
constexpr int32_t kTestHwId = 247;
constexpr uint32_t kTestConnectorId = 247;

class MockSdmCapsIntf : public sdm::SDMDisplayCapsIntf {
 public:
  MOCK_METHOD(sdm::DisplayError, GetDisplayConnectionType,
              (uint64_t disp_id, sdm::DisplayClass* display_class), (override));
  MOCK_METHOD(sdm::DisplayError, GetHdrCapabilities,
              (uint64_t disp_id, uint32_t* out_num_types, int32_t* out_types,
               float* out_max_luminance, float* out_max_average_luminance,
               float* out_min_luminance),
              (override));
  MOCK_METHOD(sdm::DisplayError, GetDisplayHwId,
              (uint64_t disp_id, int32_t* disp_hw_id), (override));
};

class MockSdmLifeCycleIntf : public sdm::SDMDisplayLifeCycleIntf {
 public:
  MOCK_METHOD(sdm::DisplayError, Init,
              (sdm::BufferAllocator*, sdm::SocketHandler*,
               sdm::DebugCallbackIntf*),
              (override));
  MOCK_METHOD(void, RegisterCompositorCallback,
              (sdm::SDMCompositorCbIntf*, bool), (override));
  MOCK_METHOD(void, RegisterSideBandCallback,
              (sdm::SDMSideBandCompositorCbIntf*, bool), (override));
  MOCK_METHOD(sdm::DisplayError, SetPowerMode,
              (uint64_t disp_id, int32_t power_mode), (override));
};

}  // namespace

class SdmHotplugHandlerTest : public ::testing::Test {
 protected:
  static void SetTestLifeCycleIntf(
      std::shared_ptr<sdm::SDMDisplayLifeCycleIntf> intf) {
    SdmBackend::SetTestLifeCycleIntf(std::move(intf));
  }

  static SdmHotplugHandler& GetHotplugHandlerForTest() {
    return SdmBackend::GetHotplugHandlerForTest();
  }
};

TEST_F(SdmHotplugHandlerTest, RegisterAndUnregisterMapsSdmDisplayToConnector) {
  NiceMock<MockSdmCapsIntf> caps_intf;
  ON_CALL(caps_intf, GetDisplayHwId(kTestSdmDisplayId, _))
      .WillByDefault(
          DoAll(SetArgPointee<1>(kTestHwId), Return(sdm::kErrorNone)));

  SdmToConnectorMapper mapper(&caps_intf);
  ASSERT_EQ(mapper.Register(kTestSdmDisplayId),
            std::optional<uint32_t>(kTestConnectorId));
  EXPECT_EQ(mapper.GetConnectorIdForSdm(kTestSdmDisplayId),
            std::optional<uint32_t>(kTestConnectorId));
  EXPECT_EQ(mapper.GetSdmIdForConnector(kTestConnectorId),
            std::optional<uint64_t>(kTestSdmDisplayId));

  mapper.Unregister(kTestSdmDisplayId);
  EXPECT_EQ(mapper.GetConnectorIdForSdm(kTestSdmDisplayId), std::nullopt);
  EXPECT_EQ(mapper.GetSdmIdForConnector(kTestConnectorId), std::nullopt);
}

TEST_F(SdmHotplugHandlerTest,
       SetHotplugHandlerOrdersCallbackRegistrationAndTeardown) {
  auto life_cycle_intf = std::make_shared<NiceMock<MockSdmLifeCycleIntf>>();
  SetTestLifeCycleIntf(life_cycle_intf);

  FakeDrmDevice dev;
  SdmBackend backend(dev);
  int dispatched_events = 0;

  {
    InSequence seq;
    EXPECT_CALL(*life_cycle_intf, RegisterCompositorCallback(_, true))
        .WillOnce(Invoke([&](sdm::SDMCompositorCbIntf*, bool) {
          GetHotplugHandlerForTest().HandleHotplugEvent(kTestConnectorId, true);
          EXPECT_EQ(dispatched_events, 1);
        }));
    EXPECT_CALL(*life_cycle_intf, RegisterCompositorCallback(_, false))
        .WillOnce(Invoke([&](sdm::SDMCompositorCbIntf*, bool) {
          GetHotplugHandlerForTest().HandleHotplugEvent(kTestConnectorId,
                                                        false);
          EXPECT_EQ(dispatched_events, 2);
        }));
  }

  backend.SetHotplugHandler([&](uint32_t, bool) { ++dispatched_events; });
  EXPECT_EQ(dispatched_events, 1);

  backend.SetHotplugHandler(nullptr);
  EXPECT_EQ(dispatched_events, 2);

  GetHotplugHandlerForTest().HandleHotplugEvent(kTestConnectorId, true);
  EXPECT_EQ(dispatched_events, 2);
  GetHotplugHandlerForTest().SetHotplugHandler(nullptr);
  SetTestLifeCycleIntf(nullptr);
}

}  // namespace android::drm_hwcomposer
