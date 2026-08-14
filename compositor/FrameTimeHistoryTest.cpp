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

// NOLINTBEGIN(readability-magic-numbers)

#include <gtest/gtest.h>

#include "compositor/FrameTimeHistory.h"

#include <chrono>
#include <cmath>

namespace android::drm_hwcomposer {
namespace {

std::chrono::nanoseconds HzToNs(float hz) {
  return std::chrono::nanoseconds(std::lrint(1000000000.0F / hz));
}
}  // namespace

TEST(FrameTimeHistory, EmptyHistory) {
  FrameTimeHistory history;

  std::chrono::time_point<std::chrono::steady_clock> now;
  EXPECT_FLOAT_EQ(history.CalculateFps(now), 0.0F);
}

TEST(FrameTimeHistory, InfreqeuntLayerFps) {
  FrameTimeHistory history;

  std::chrono::time_point<std::chrono::steady_clock> now;
  for (int i = 0; i < 3; i++) {
    history.AddFrameTime(now);
    EXPECT_FLOAT_EQ(history.CalculateFps(now), 0.0F);
    now += HzToNs(60.0F);
  }

  // With 4 frames it should be above the threshold.
  history.AddFrameTime(now);
  EXPECT_FLOAT_EQ(history.CalculateFps(now), 60.0F);
}

TEST(FrameTimeHistory, InfreqeuntLayerFpsDueToAgeOut) {
  FrameTimeHistory history;

  std::chrono::time_point<std::chrono::steady_clock> now;
  for (int i = 0; i < 3; i++) {
    history.AddFrameTime(now);
    EXPECT_FLOAT_EQ(history.CalculateFps(now), 0.0F);
    now += HzToNs(100.0F);
  }

  // With 4 layers it should be above the threshold, but the added 3 are outside
  // of max time age so shouldn't count towards activity.
  now += FrameTimeHistory::kMaxFrameTimeAge;
  history.AddFrameTime(now);
  EXPECT_FLOAT_EQ(history.CalculateFps(now), 0.0F);
}

TEST(FrameTimeHistory, InfreqeuntLayerFpsDueToAgeOutWhenCalculating) {
  FrameTimeHistory history;

  std::chrono::time_point<std::chrono::steady_clock> now;
  for (int i = 0; i < 3; i++) {
    history.AddFrameTime(now);
    EXPECT_FLOAT_EQ(history.CalculateFps(now), 0.0F);
    now += HzToNs(100.0F);
  }

  // With 4 layers it should be above the threshold, but the added 3 are outside
  // of max time age so shouldn't count towards activity.
  history.AddFrameTime(now);

  EXPECT_FLOAT_EQ(history.CalculateFps(
                      now + FrameTimeHistory::kMaxFrameTimeAge - HzToNs(50.0F)),
                  0.0F);
}

TEST(FrameTimeHistory, UnorderedFrameTimeInsert) {
  FrameTimeHistory history;

  const std::chrono::time_point<std::chrono::steady_clock> now;
  history.AddFrameTime(now + 3 * HzToNs(60.0F));
  history.AddFrameTime(now + 0 * HzToNs(60.0F));
  history.AddFrameTime(now + 2 * HzToNs(60.0F));
  history.AddFrameTime(now + 1 * HzToNs(60.0F));

  EXPECT_FLOAT_EQ(history.CalculateFps(now + 3 * HzToNs(60.0F)), 60.0F);
}

TEST(FrameTimeHistory, ALotOfInserts) {
  FrameTimeHistory history;

  std::chrono::time_point<std::chrono::steady_clock> now;
  for (int i = 0; i < 1000; i++) {
    now += HzToNs(144.0F);
    history.AddFrameTime(now);
  }

  EXPECT_FLOAT_EQ(history.CalculateFps(now), 144.0F);
}

TEST(FrameTimeHistory, CalculateFpsInThePast) {
  FrameTimeHistory history;

  /*
  x-x-x-x-x--x--x--x--x
  where x is frame and - is 120Hz period.
  */
  std::chrono::time_point<std::chrono::steady_clock> now;
  history.AddFrameTime(now);
  for (int i = 0; i < 4; i++) {
    now += HzToNs(120.0F);
    history.AddFrameTime(now);
  }

  const std::chrono::time_point<std::chrono::steady_clock> last_120hz = now;
  for (int i = 0; i < 4; i++) {
    now += HzToNs(60.0F);
    history.AddFrameTime(now);
  }

  EXPECT_FLOAT_EQ(history.CalculateFps(last_120hz), 120.0F);
  // The calculated FPS should equal the geometric mean of the frame rates.
  EXPECT_FLOAT_EQ(history.CalculateFps(now),
                  (8 / ((4 / 120.0F) + (4 / 60.0F))));
}

}  // namespace android::drm_hwcomposer

// NOLINTEND(readability-magic-numbers)
