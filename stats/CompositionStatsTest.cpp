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

#include <functional>
#include <map>
#include <memory>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "stats/CompositionStats.h"

using ::testing::Eq;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::StrictMock;

namespace android::drm_hwcomposer {

// Equality operator to be used by Eq, ASSERT_EQ, etc. Needs to be static rather
// than in anonymous namespace to ensure gmock can find it.
static bool operator==(const CompositionStats& lhs,
                       const CompositionStats& rhs) {
  return lhs.total_frames == rhs.total_frames &&
         lhs.total_pixops == rhs.total_pixops &&
         lhs.gpu_pixops == rhs.gpu_pixops &&
         lhs.failed_kms_validate == rhs.failed_kms_validate &&
         lhs.failed_kms_present == rhs.failed_kms_present &&
         lhs.frames_flattened == rhs.frames_flattened &&
         lhs.cursor_plane_frames == rhs.cursor_plane_frames &&
         lhs.failed_kms_cursor_validate == rhs.failed_kms_cursor_validate;
}

// Stream insertion operator for better gtest failure messages.
static std::ostream& operator<<(std::ostream& os,
                                const CompositionStats& stats) {
  os << "CompositionStats { "
     << "total_frames: " << stats.total_frames
     << ", total_pixops: " << stats.total_pixops
     << ", gpu_pixops: " << stats.gpu_pixops
     << ", failed_kms_validate: " << stats.failed_kms_validate
     << ", failed_kms_present: " << stats.failed_kms_present
     << ", frames_flattened: " << stats.frames_flattened
     << ", cursor_plane_frames: " << stats.cursor_plane_frames
     << ", failed_kms_cursor_validate: " << stats.failed_kms_cursor_validate
     << " }";
  return os;
}

class MockCompositionStatsProvider : public CompositionStatsProvider {
 public:
  MOCK_METHOD((std::map<int64_t, CompositionStats>), PullCompositionStats, (),
              (override));
};

// Helper class to facilitate passing std::function to the
// CompositionStatsTracker and use gmock to validate expectations.
class MockStatsCallback {
 public:
  // Set expectations on the Invoke mock method.
  MOCK_METHOD(void, Invoke,
              (int64_t, const CompositionStats&, const CompositionStats&), ());

  // Pass this to CompositionStatsTracker::ReportStats.
  CompositionStatsTracker::Callback AsStdFunction() {
    return [this](int64_t id, const CompositionStats& c,
                  const CompositionStats& d) { this->Invoke(id, c, d); };
  }
};

class CompositionStatsTrackerTest : public ::testing::Test {
 protected:
  std::unique_ptr<NiceMock<MockCompositionStatsProvider>> mock_provider_;
  std::unique_ptr<CompositionStatsTracker> tracker_;

  void SetUp() override {
    mock_provider_ = std::make_unique<NiceMock<MockCompositionStatsProvider>>();
    tracker_ = std::make_unique<CompositionStatsTracker>(mock_provider_.get());
  }

  // Helper method to create sample stats. Actual values don't really matter.
  CompositionStats CreateStats(uint32_t base) {
    return CompositionStats{.total_frames = base,
                            .total_pixops = base * 1000,
                            .gpu_pixops = base * 500,
                            .failed_kms_validate = base / 10,
                            .failed_kms_present = base / 20,
                            .frames_flattened = base / 5,
                            .cursor_plane_frames = base / 2,
                            .failed_kms_cursor_validate = base / 50};
  }
};

// Initial call to ReportStats reports the same stats for cumulative and delta.
TEST_F(CompositionStatsTrackerTest, ReportStatsInitialCall) {
  const int64_t display_handle = 1;
  CompositionStats current_stats = CreateStats(100);
  std::map<int64_t, CompositionStats> provider_result = {
      {display_handle, current_stats}};

  StrictMock<MockStatsCallback> mock_callback;

  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result));

  // Expect that delta is same as cumulative.
  EXPECT_CALL(mock_callback,
              Invoke(Eq(display_handle), Eq(current_stats), Eq(current_stats)));

  tracker_->ReportStats(mock_callback.AsStdFunction());
}

// Subsequent calls to ReportStats with no change in cumulative stats.
TEST_F(CompositionStatsTrackerTest, ReportStatsSubsequentCallNoChange) {
  const int64_t display_handle = 1;
  CompositionStats stats = CreateStats(100);
  std::map<int64_t, CompositionStats> provider_result = {
      {display_handle, stats}};
  CompositionStats zero_delta = {};

  // Same provider result for both calls.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillRepeatedly(Return(provider_result));

  StrictMock<MockStatsCallback> mock_callback;

  // Initial call.
  EXPECT_CALL(mock_callback, Invoke(Eq(display_handle), Eq(stats), Eq(stats)));
  tracker_->ReportStats(mock_callback.AsStdFunction());

  // Second call. Delta should be zero.
  EXPECT_CALL(mock_callback,
              Invoke(Eq(display_handle), Eq(stats), Eq(zero_delta)));
  tracker_->ReportStats(mock_callback.AsStdFunction());
}

// Test that the delta is reported as expected.
TEST_F(CompositionStatsTrackerTest, ReportStatsSubsequentCallWithChange) {
  const int64_t display_handle = 1;
  CompositionStats stats1 = CreateStats(100);
  CompositionStats stats2 = CreateStats(150);
  std::map<int64_t, CompositionStats> provider_result1 = {
      {display_handle, stats1}};
  std::map<int64_t, CompositionStats> provider_result2 = {
      {display_handle, stats2}};
  CompositionStats expected_delta = stats2 - stats1;

  StrictMock<MockStatsCallback> mock_callback;

  // First call with the initial stats.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result1));
  EXPECT_CALL(mock_callback,
              Invoke(Eq(display_handle), Eq(stats1), Eq(stats1)));
  tracker_->ReportStats(mock_callback.AsStdFunction());

  // Second call with updated stats and non-trivial delta.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result2));
  EXPECT_CALL(mock_callback,
              Invoke(Eq(display_handle), Eq(stats2), Eq(expected_delta)));
  tracker_->ReportStats(mock_callback.AsStdFunction());
}

// Test that stats for multiple displays are reported correctly.
TEST_F(CompositionStatsTrackerTest, ReportStatsMultipleDisplays) {
  const int64_t display_handle1 = 10;
  const int64_t display_handle2 = 20;
  CompositionStats display1_stats1 = CreateStats(100);
  CompositionStats display2_stats1 = CreateStats(200);
  CompositionStats display1_stats2 = CreateStats(110);
  CompositionStats display2_stats2 = CreateStats(250);

  std::map<int64_t, CompositionStats> provider_result1 =
      {{display_handle1, display1_stats1}, {display_handle2, display2_stats1}};
  std::map<int64_t, CompositionStats> provider_result2 =
      {{display_handle1, display1_stats2}, {display_handle2, display2_stats2}};

  CompositionStats display1_expected_delta1 = display1_stats1;
  CompositionStats display1_expected_delta2 = display1_stats2 - display1_stats1;
  CompositionStats display2_expected_delta1 = display2_stats1;
  CompositionStats display2_expected_delta2 = display2_stats2 - display2_stats1;

  StrictMock<MockStatsCallback> mock_callback;

  // Initial call. Ordering between displays doesn't matter.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result1));
  EXPECT_CALL(mock_callback, Invoke(Eq(display_handle1), Eq(display1_stats1),
                                    Eq(display1_expected_delta1)));
  EXPECT_CALL(mock_callback, Invoke(Eq(display_handle2), Eq(display2_stats1),
                                    Eq(display2_expected_delta1)));
  tracker_->ReportStats(mock_callback.AsStdFunction());

  // Updated call. Ordering between displays doesn't matter.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result2));
  EXPECT_CALL(mock_callback, Invoke(Eq(display_handle1), Eq(display1_stats2),
                                    Eq(display1_expected_delta2)));
  EXPECT_CALL(mock_callback, Invoke(Eq(display_handle2), Eq(display2_stats2),
                                    Eq(display2_expected_delta2)));
  tracker_->ReportStats(mock_callback.AsStdFunction());
}

// No displays in the provider result.
TEST_F(CompositionStatsTrackerTest, ReportStatsEmptyResult) {
  std::map<int64_t, CompositionStats> empty_result = {};

  // StrictMock will fail if there are any unexpected calls to Invoke.
  StrictMock<MockStatsCallback> mock_callback;
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(empty_result));
  tracker_->ReportStats(mock_callback.AsStdFunction());
}

// Display added in between calls to ReportStats.
TEST_F(CompositionStatsTrackerTest, ReportStatsDisplayAdded) {
  const int64_t display_handle1 = 10;
  const int64_t display_handle2 = 20;
  const CompositionStats display1_stats1 = CreateStats(100);
  const CompositionStats display1_stats2 = CreateStats(110);
  const CompositionStats display2_stats = CreateStats(50);

  std::map<int64_t, CompositionStats> provider_result1 = {
      {display_handle1, display1_stats1}};
  std::map<int64_t, CompositionStats> provider_result2 =
      {{display_handle1, display1_stats2}, {display_handle2, display2_stats}};

  const CompositionStats display1_expected_delta1 = display1_stats1;
  const CompositionStats display1_expected_delta2 = display1_stats2 -
                                                    display1_stats1;
  const CompositionStats display2_expected_delta = display2_stats;

  StrictMock<MockStatsCallback> mock_callback;

  // First call only contains display 1.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result1));
  EXPECT_CALL(mock_callback, Invoke(Eq(display_handle1), Eq(display1_stats1),
                                    Eq(display1_expected_delta1)));
  tracker_->ReportStats(mock_callback.AsStdFunction());

  // Second call has both displays.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result2));
  EXPECT_CALL(mock_callback, Invoke(Eq(display_handle1), Eq(display1_stats2),
                                    Eq(display1_expected_delta2)));
  EXPECT_CALL(mock_callback, Invoke(Eq(display_handle2), Eq(display2_stats),
                                    Eq(display2_expected_delta)));
  tracker_->ReportStats(mock_callback.AsStdFunction());
}

// Display removed in between calls to ReportStats.
TEST_F(CompositionStatsTrackerTest, ReportStatsDisplayRemoved) {
  const int64_t display_handle1 = 10;
  const int64_t display_handle2 = 20;
  const CompositionStats display1_stats1 = CreateStats(100);
  const CompositionStats display2_stats = CreateStats(200);
  const CompositionStats display1_stats2 = CreateStats(110);

  std::map<int64_t, CompositionStats> provider_result1 =
      {{display_handle1, display1_stats1}, {display_handle2, display2_stats}};
  std::map<int64_t, CompositionStats> provider_result2 = {
      {display_handle1, display1_stats2}};

  const CompositionStats display1_expected_delta1 = display1_stats1;
  const CompositionStats display2_expected_delta = display2_stats;
  const CompositionStats display1_expected_delta2 = display1_stats2 -
                                                    display1_stats1;

  StrictMock<MockStatsCallback> mock_callback;

  // Initial call has both displays.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result1));
  EXPECT_CALL(mock_callback, Invoke(Eq(display_handle1), Eq(display1_stats1),
                                    Eq(display1_expected_delta1)));
  EXPECT_CALL(mock_callback, Invoke(Eq(display_handle2), Eq(display2_stats),
                                    Eq(display2_expected_delta)));
  tracker_->ReportStats(mock_callback.AsStdFunction());

  // Second call has only display 1. StrictMock will fail if Invoke is called
  // for display_handle2.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result2));
  EXPECT_CALL(mock_callback, Invoke(Eq(display_handle1), Eq(display1_stats2),
                                    Eq(display1_expected_delta2)));
  tracker_->ReportStats(mock_callback.AsStdFunction());
}

}  // namespace android::drm_hwcomposer
