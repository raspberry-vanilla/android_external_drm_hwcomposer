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

#include "backend/sdm/SdmHotplugHandler.h"  // IWYU pragma: keep

#include <cstdint>
#include <optional>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <core/buffer_allocator.h>  // IWYU pragma: keep
#include <core/sdm_types.h>
#include <sdm_display_intf_caps.h>

#include "backend/sdm/SdmToConnectorMapper.h"

namespace android::drm_hwcomposer {
namespace {

using ::testing::_;
using ::testing::DoAll;
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

TEST(SdmHotplugHandlerTest, RegisterAndUnregisterMapsSdmDisplayToConnector) {
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

}  // namespace
}  // namespace android::drm_hwcomposer
