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

#include <cstdint>
#include <map>
#include <memory>
#include <ostream>

#include "stats/Stats.h"

using ::testing::Eq;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::StrictMock;

namespace android::drm_hwcomposer {

static bool operator==(const CompositionAttributes& lhs,
                       const CompositionAttributes& rhs) {
  return lhs.display_handle == rhs.display_handle &&
         lhs.present_failed == rhs.present_failed &&
         lhs.validation_result == rhs.validation_result &&
         lhs.flatten_reason == rhs.flatten_reason;
}

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
         lhs.failed_kms_cursor_validate == rhs.failed_kms_cursor_validate &&
         lhs.layer_count == rhs.layer_count &&
         lhs.used_plane_count == rhs.used_plane_count;
}

// Stream insertion operator for better gtest failure messages.
static std::ostream& operator<<(std::ostream& os,
                                const CompositionStats& stats) {
  os << "CompositionStats { " << "total_frames: " << stats.total_frames
     << ", total_pixops: " << stats.total_pixops
     << ", gpu_pixops: " << stats.gpu_pixops
     << ", failed_kms_validate: " << stats.failed_kms_validate
     << ", failed_kms_present: " << stats.failed_kms_present
     << ", frames_flattened: " << stats.frames_flattened
     << ", cursor_plane_frames: " << stats.cursor_plane_frames
     << ", failed_kms_cursor_validate: " << stats.failed_kms_cursor_validate
     << ", layer_count: " << stats.layer_count
     << ", use_plane_count: " << stats.used_plane_count << " }";
  return os;
}

class MockStatsProvider : public StatsProvider {
 public:
  MOCK_METHOD((std::map<CompositionAttributes, CompositionStats>),
              PullCompositionStats, (), (override));
  MOCK_METHOD(ActiveDisplayCounts, PullActiveDisplayCounts, (), (override));
};

// Helper class to facilitate passing std::function to the
// StatsTracker and use gmock to validate expectations.
class MockStatsCallback {
 public:
  // Set expectations on the Invoke mock method.
  MOCK_METHOD(void, Invoke,
              (const CompositionAttributes&, const CompositionStats&,
               const CompositionStats&),
              ());

  // Pass this to StatsTracker::ReportCompositionStats.
  StatsTracker::Callback AsStdFunction() {
    return [this](const CompositionAttributes& a, const CompositionStats& c,
                  const CompositionStats& d) { this->Invoke(a, c, d); };
  }
};

class StatsTrackerTest : public ::testing::Test {
 protected:
  std::unique_ptr<NiceMock<MockStatsProvider>> mock_provider_;
  std::unique_ptr<StatsTracker> tracker_;

  void SetUp() override {
    mock_provider_ = std::make_unique<NiceMock<MockStatsProvider>>();
    tracker_ = std::make_unique<StatsTracker>(mock_provider_.get());
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
                            .failed_kms_cursor_validate = base / 50,
                            .layer_count = 2,
                            .used_plane_count = 2};
  }
};

// Initial call to ReportCompositionStats reports the same stats for cumulative
// and delta.
TEST_F(StatsTrackerTest, ReportCompositionStatsInitialCall) {
  const CompositionAttributes attr{.display_handle = 1};
  const CompositionStats current_stats = CreateStats(100);
  const std::map<CompositionAttributes, CompositionStats> provider_result{
      {attr, current_stats}};

  StrictMock<MockStatsCallback> mock_callback;

  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result));

  // Expect that delta is same as cumulative.
  EXPECT_CALL(mock_callback,
              Invoke(Eq(attr), Eq(current_stats), Eq(current_stats)));

  tracker_->ReportCompositionStats(mock_callback.AsStdFunction());
}

// Subsequent calls to ReportCompositionStats with no change in cumulative
// stats.
TEST_F(StatsTrackerTest, ReportCompositionStatsSubsequentCallNoChange) {
  const CompositionAttributes attr{.display_handle = 1};
  const CompositionStats stats = CreateStats(100);
  const std::map<CompositionAttributes, CompositionStats> provider_result{
      {attr, stats}};
  const CompositionStats zero_delta{};

  // Same provider result for both calls.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillRepeatedly(Return(provider_result));

  StrictMock<MockStatsCallback> mock_callback;

  // Initial call.
  EXPECT_CALL(mock_callback, Invoke(Eq(attr), Eq(stats), Eq(stats)));
  tracker_->ReportCompositionStats(mock_callback.AsStdFunction());

  // Second call. Delta should be zero.
  EXPECT_CALL(mock_callback, Invoke(Eq(attr), Eq(stats), Eq(zero_delta)));
  tracker_->ReportCompositionStats(mock_callback.AsStdFunction());
}

// Test that the delta is reported as expected.
TEST_F(StatsTrackerTest, ReportCompositionStatsSubsequentCallWithChange) {
  const CompositionAttributes attr{.display_handle = 1};
  const CompositionStats stats1 = CreateStats(100);
  const CompositionStats stats2 = CreateStats(150);
  const std::map<CompositionAttributes, CompositionStats> provider_result1{
      {attr, stats1}};
  const std::map<CompositionAttributes, CompositionStats> provider_result2{
      {attr, stats2}};
  const CompositionStats expected_delta = stats2 - stats1;

  StrictMock<MockStatsCallback> mock_callback;

  // First call with the initial stats.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result1));
  EXPECT_CALL(mock_callback, Invoke(Eq(attr), Eq(stats1), Eq(stats1)));
  tracker_->ReportCompositionStats(mock_callback.AsStdFunction());

  // Second call with updated stats and non-trivial delta.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result2));
  EXPECT_CALL(mock_callback, Invoke(Eq(attr), Eq(stats2), Eq(expected_delta)));
  tracker_->ReportCompositionStats(mock_callback.AsStdFunction());
}

// Test that stats for multiple attributes are reported correctly.
TEST_F(StatsTrackerTest, ReportCompositionStatsMultipleAttributes) {
  const CompositionAttributes attr1{.display_handle = 10};
  const CompositionAttributes attr2{.display_handle = 20};
  const CompositionStats attr1_stats1 = CreateStats(100);
  const CompositionStats attr2_stats1 = CreateStats(200);
  const CompositionStats attr1_stats2 = CreateStats(110);
  const CompositionStats attr2_stats2 = CreateStats(250);

  const std::map<CompositionAttributes, CompositionStats>
      provider_result1{{attr1, attr1_stats1}, {attr2, attr2_stats1}};
  const std::map<CompositionAttributes, CompositionStats>
      provider_result2{{attr1, attr1_stats2}, {attr2, attr2_stats2}};

  const CompositionStats attr1_expected_delta1 = attr1_stats1;
  const CompositionStats attr1_expected_delta2 = attr1_stats2 - attr1_stats1;
  const CompositionStats attr2_expected_delta1 = attr2_stats1;
  const CompositionStats attr2_expected_delta2 = attr2_stats2 - attr2_stats1;

  StrictMock<MockStatsCallback> mock_callback;

  // Initial call. Ordering between attributes doesn't matter.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result1));
  EXPECT_CALL(mock_callback,
              Invoke(Eq(attr1), Eq(attr1_stats1), Eq(attr1_expected_delta1)));
  EXPECT_CALL(mock_callback,
              Invoke(Eq(attr2), Eq(attr2_stats1), Eq(attr2_expected_delta1)));
  tracker_->ReportCompositionStats(mock_callback.AsStdFunction());

  // Updated call. Ordering between attributes doesn't matter.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result2));
  EXPECT_CALL(mock_callback,
              Invoke(Eq(attr1), Eq(attr1_stats2), Eq(attr1_expected_delta2)));
  EXPECT_CALL(mock_callback,
              Invoke(Eq(attr2), Eq(attr2_stats2), Eq(attr2_expected_delta2)));
  tracker_->ReportCompositionStats(mock_callback.AsStdFunction());
}

// No entries in the provider result.
TEST_F(StatsTrackerTest, ReportCompositionStatsEmptyResult) {
  const std::map<CompositionAttributes, CompositionStats> empty_result{};

  // StrictMock will fail if there are any unexpected calls to Invoke.
  StrictMock<MockStatsCallback> mock_callback;
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(empty_result));
  tracker_->ReportCompositionStats(mock_callback.AsStdFunction());
}

// Attributes added in between calls to ReportCompositionStats.
TEST_F(StatsTrackerTest, ReportCompositionStatsAttributesAdded) {
  const CompositionAttributes attr1{.display_handle = 10};
  const CompositionAttributes attr2{.display_handle = 20};
  const CompositionStats attr1_stats1 = CreateStats(100);
  const CompositionStats attr1_stats2 = CreateStats(110);
  const CompositionStats attr2_stats = CreateStats(50);

  const std::map<CompositionAttributes, CompositionStats> provider_result1{
      {attr1, attr1_stats1}};
  const std::map<CompositionAttributes, CompositionStats>
      provider_result2{{attr1, attr1_stats2}, {attr2, attr2_stats}};

  const CompositionStats attr1_expected_delta1 = attr1_stats1;
  const CompositionStats attr1_expected_delta2 = attr1_stats2 - attr1_stats1;
  const CompositionStats attr2_expected_delta = attr2_stats;

  StrictMock<MockStatsCallback> mock_callback;

  // First call only contains attr1.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result1));
  EXPECT_CALL(mock_callback,
              Invoke(Eq(attr1), Eq(attr1_stats1), Eq(attr1_expected_delta1)));
  tracker_->ReportCompositionStats(mock_callback.AsStdFunction());

  // Second call has both attributes.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result2));
  EXPECT_CALL(mock_callback,
              Invoke(Eq(attr1), Eq(attr1_stats2), Eq(attr1_expected_delta2)));
  EXPECT_CALL(mock_callback,
              Invoke(Eq(attr2), Eq(attr2_stats), Eq(attr2_expected_delta)));
  tracker_->ReportCompositionStats(mock_callback.AsStdFunction());
}

// Attributes removed in between calls to ReportCompositionStats.
TEST_F(StatsTrackerTest, ReportCompositionStatsAttributesRemoved) {
  const CompositionAttributes attr1{.display_handle = 10};
  const CompositionAttributes attr2{.display_handle = 20};
  const CompositionStats attr1_stats1 = CreateStats(100);
  const CompositionStats attr2_stats = CreateStats(200);
  const CompositionStats attr1_stats2 = CreateStats(110);

  const std::map<CompositionAttributes, CompositionStats>
      provider_result1{{attr1, attr1_stats1}, {attr2, attr2_stats}};
  const std::map<CompositionAttributes, CompositionStats> provider_result2{
      {attr1, attr1_stats2}};

  const CompositionStats attr1_expected_delta1 = attr1_stats1;
  const CompositionStats attr2_expected_delta = attr2_stats;
  const CompositionStats attr1_expected_delta2 = attr1_stats2 - attr1_stats1;

  StrictMock<MockStatsCallback> mock_callback;

  // Initial call has both attributes.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result1));
  EXPECT_CALL(mock_callback,
              Invoke(Eq(attr1), Eq(attr1_stats1), Eq(attr1_expected_delta1)));
  EXPECT_CALL(mock_callback,
              Invoke(Eq(attr2), Eq(attr2_stats), Eq(attr2_expected_delta)));
  tracker_->ReportCompositionStats(mock_callback.AsStdFunction());

  // Second call has only attr1. StrictMock will fail if Invoke is called
  // for attr2.
  EXPECT_CALL(*mock_provider_, PullCompositionStats())
      .WillOnce(Return(provider_result2));
  EXPECT_CALL(mock_callback,
              Invoke(Eq(attr1), Eq(attr1_stats2), Eq(attr1_expected_delta2)));
  tracker_->ReportCompositionStats(mock_callback.AsStdFunction());
}

}  // namespace android::drm_hwcomposer
