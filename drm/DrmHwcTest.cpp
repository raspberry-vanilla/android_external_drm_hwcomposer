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

#include "drm/DrmHwc.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "compositor/DisplayInfo.h"
#include "drm/DrmDisplayPipeline.h"
#include "drm/DrmTestUtils.h"
#include "hwc/HwcDisplay.h"

namespace android::drm_hwcomposer {

namespace {

class RecordingDrmHwc : public DrmHwc {
 public:
  bool HasCallback() const override {
    return true;
  }
  void SendVsyncEventToClient(DisplayHandle /*display_handle*/,
                              int64_t /*timestamp*/,
                              uint32_t /*vsync_period*/) const override {
  }
  void SendVsyncPeriodTimingChangedEventToClient(
      DisplayHandle /*display_handle*/, int64_t /*timestamp*/) const override {
  }
  void SendRefreshEventToClient(DisplayHandle /*display_handle*/) override {
  }
  void SendHotplugEventToClient(DisplayHandle display_handle,
                                DisplayStatus display_status) override {
    hotplug_events.emplace_back(display_handle, display_status);
  }
  void SendHdcpLevelsChangedEventToClient(
      DisplayHandle /*display_handle*/,
      std::optional<HdcpContentType> /*current_hdcp_level*/) override {
  }

  std::vector<std::pair<DisplayHandle, DisplayStatus>> hotplug_events;
};

}  // namespace

struct DrmHwcTest : public ::testing::Test {
  void SetUp() override {
    fake_device = std::make_unique<FakeDrmDevice>(&hwc.GetResMan());
    fake_device->GetFakeAtomicCommitSink()->SetDriveStateManager(true);
  }

  void TearDown() override {
    hwc.DeinitDisplays();
  }

  std::shared_ptr<DrmDisplayPipeline> CreatePipeline(
      bool is_external = false) const {
    fake_device->AddPipelineResources(is_external);
    return fake_device->GetBackend().CreatePipeline(
        *fake_device->GetConnectors().back());
  }

  std::unique_ptr<FakeDrmDevice> fake_device;
  RecordingDrmHwc hwc;
};

TEST_F(DrmHwcTest, BindAndUnbindPrimaryDisplayTransitionsToHeadless) {
  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  ASSERT_NE(primary_pipe, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  HwcDisplay* primary = hwc.GetDisplay(kPrimaryDisplay);
  ASSERT_NE(primary, nullptr);
  EXPECT_FALSE(primary->IsInHeadlessMode());
  ASSERT_EQ(hwc.hotplug_events.size(), 1U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{kPrimaryDisplay},
                           DrmHwc::DisplayStatus::kConnected));

  hwc.hotplug_events.clear();
  EXPECT_TRUE(hwc.UnbindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  ASSERT_NE(hwc.GetDisplay(kPrimaryDisplay), nullptr);
  EXPECT_TRUE(hwc.GetDisplay(kPrimaryDisplay)->IsInHeadlessMode());
  ASSERT_EQ(hwc.hotplug_events.size(), 1U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{kPrimaryDisplay},
                           DrmHwc::DisplayStatus::kConnected));
}

TEST_F(DrmHwcTest, BindAndUnbindExternalDisplayAlongsidePrimary) {
  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  auto external_pipe = CreatePipeline(/*is_external=*/true);
  ASSERT_NE(primary_pipe, nullptr);
  ASSERT_NE(external_pipe, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  EXPECT_TRUE(hwc.BindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  ASSERT_NE(hwc.GetDisplay(kPrimaryDisplay), nullptr);
  ASSERT_NE(hwc.GetDisplay(1), nullptr);
  EXPECT_FALSE(hwc.GetDisplay(kPrimaryDisplay)->IsInHeadlessMode());
  EXPECT_FALSE(hwc.GetDisplay(1)->IsInHeadlessMode());
  ASSERT_EQ(hwc.hotplug_events.size(), 2U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{kPrimaryDisplay},
                           DrmHwc::DisplayStatus::kConnected));
  EXPECT_EQ(hwc.hotplug_events[1],
            std::make_pair(DisplayHandle{1},
                           DrmHwc::DisplayStatus::kConnected));

  // Unbinding the external display removes display 1 while keeping primary.
  hwc.hotplug_events.clear();
  EXPECT_TRUE(hwc.UnbindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  EXPECT_NE(hwc.GetDisplay(kPrimaryDisplay), nullptr);
  EXPECT_EQ(hwc.GetDisplay(1), nullptr);
  ASSERT_EQ(hwc.hotplug_events.size(), 1U);
  EXPECT_EQ(hwc.hotplug_events[0],
            std::make_pair(DisplayHandle{1},
                           DrmHwc::DisplayStatus::kDisconnected));

  EXPECT_TRUE(hwc.UnbindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
}

TEST_F(DrmHwcTest, ActiveDisplayCountsAndDumpState) {
  auto primary_pipe = CreatePipeline(/*is_external=*/false);
  auto external_pipe = CreatePipeline(/*is_external=*/true);
  ASSERT_NE(primary_pipe, nullptr);
  ASSERT_NE(external_pipe, nullptr);

  EXPECT_TRUE(hwc.BindDisplay(primary_pipe));
  EXPECT_TRUE(hwc.BindDisplay(external_pipe));
  hwc.FinalizeDisplayBinding();
  hwc.FlushHotplugEvents();

  const auto counts = hwc.PullActiveDisplayCounts();
  EXPECT_EQ(counts.num_active_physical_displays, 2);
  EXPECT_EQ(counts.num_active_external_displays, 1);
  EXPECT_EQ(counts.num_virtual_displays, 0);
  EXPECT_FALSE(hwc.DumpState().empty());

  EXPECT_TRUE(hwc.UnbindDisplay(external_pipe));
  EXPECT_TRUE(hwc.UnbindDisplay(primary_pipe));
  hwc.FinalizeDisplayBinding();
}

}  // namespace android::drm_hwcomposer
