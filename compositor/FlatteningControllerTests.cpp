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

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "compositor/FlatteningController.h"

using ::testing::StrictMock;

namespace android::drm_hwcomposer {

namespace {
constexpr auto kTestTimeout = std::chrono::milliseconds(100);
constexpr auto kTimeoutEpsilon = std::chrono::milliseconds(50);
constexpr DisplayHandle kHandle = {0};
}  // namespace

class MockFlatConCallbacks {
 public:
  MOCK_METHOD(void, Trigger, ());
};

class FlatteningControllerTest : public ::testing::Test {
 protected:
  static void EmptyTrigger() {
  }
};

TEST_F(FlatteningControllerTest, DisabledOnCreation) {
  FlatConCallbacks cbks = {.trigger = EmptyTrigger};
  auto flat_con = std::make_unique<FlatteningController>(kHandle, cbks,
                                                         kTestTimeout);

  EXPECT_FALSE(flat_con->ShouldFlatten());

  std::this_thread::sleep_for(kTestTimeout + kTimeoutEpsilon);

  EXPECT_FALSE(flat_con->ShouldFlatten());
}

TEST_F(FlatteningControllerTest, EnabledAfterNewFrame) {
  FlatConCallbacks cbks = {.trigger = EmptyTrigger};
  auto flat_con = std::make_unique<FlatteningController>(kHandle, cbks,
                                                         kTestTimeout);

  flat_con->NewFrame();
  EXPECT_FALSE(flat_con->ShouldFlatten());

  std::this_thread::sleep_for(kTestTimeout + kTimeoutEpsilon);

  EXPECT_TRUE(flat_con->ShouldFlatten());
}

TEST_F(FlatteningControllerTest, DisabledAfterCallingDisable) {
  FlatConCallbacks cbks = {.trigger = EmptyTrigger};
  auto flat_con = std::make_unique<FlatteningController>(kHandle, cbks,
                                                         kTestTimeout);

  flat_con->NewFrame();
  std::this_thread::sleep_for(kTestTimeout + kTimeoutEpsilon);
  EXPECT_TRUE(flat_con->ShouldFlatten());

  flat_con->DisableFlattening();
  EXPECT_FALSE(flat_con->ShouldFlatten());

  std::this_thread::sleep_for(kTestTimeout + kTimeoutEpsilon);
  EXPECT_FALSE(flat_con->ShouldFlatten());
}

TEST_F(FlatteningControllerTest, TriggersCallbackAfterTimeout) {
  StrictMock<MockFlatConCallbacks> mock_cb;
  FlatConCallbacks cbks = {.trigger = [&]() { mock_cb.Trigger(); }};
  auto flat_con = std::make_unique<FlatteningController>(kHandle, cbks,
                                                         kTestTimeout);

  EXPECT_CALL(mock_cb, Trigger()).Times(1);

  flat_con->NewFrame();

  std::this_thread::sleep_for(kTestTimeout + kTimeoutEpsilon);
}

TEST_F(FlatteningControllerTest, ShouldFlattenAfterFirstNewFrame) {
  StrictMock<MockFlatConCallbacks> mock_cb;
  FlatConCallbacks cbks = {.trigger = [&]() { mock_cb.Trigger(); }};
  auto flat_con = std::make_unique<FlatteningController>(kHandle, cbks,
                                                         kTestTimeout);

  EXPECT_CALL(mock_cb, Trigger()).Times(1);

  // Enable the controller and start the timer.
  flat_con->NewFrame();

  std::this_thread::sleep_for(kTestTimeout + kTimeoutEpsilon);
  EXPECT_TRUE(flat_con->ShouldFlatten());

  // First NewFrame is in response to the refresh callback.
  flat_con->NewFrame();
  EXPECT_TRUE(flat_con->ShouldFlatten());
}

TEST_F(FlatteningControllerTest, ShouldNotFlattenAfterSecondNewFrame) {
  StrictMock<MockFlatConCallbacks> mock_cb;
  FlatConCallbacks cbks = {.trigger = [&]() { mock_cb.Trigger(); }};
  auto flat_con = std::make_unique<FlatteningController>(kHandle, cbks,
                                                         kTestTimeout);

  EXPECT_CALL(mock_cb, Trigger()).Times(1);

  // Enable the controller and start the timer.
  flat_con->NewFrame();
  std::this_thread::sleep_for(kTestTimeout + kTimeoutEpsilon);
  EXPECT_TRUE(flat_con->ShouldFlatten());

  // First NewFrame is in response to the refresh callback, should stay
  // flattened.
  flat_con->NewFrame();
  EXPECT_TRUE(flat_con->ShouldFlatten());

  // Second NewFrame should not flatten.
  flat_con->NewFrame();
  EXPECT_FALSE(flat_con->ShouldFlatten());
}

TEST_F(FlatteningControllerTest, TriggersCallbackAtCorrectTime) {
  std::mutex mutex;
  std::condition_variable cv;
  bool triggered = false;

  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point trigger_time;

  FlatConCallbacks cbks = {.trigger = [&]() {
    std::lock_guard<std::mutex> lock(mutex);
    trigger_time = std::chrono::steady_clock::now();
    triggered = true;
    cv.notify_one();
  }};
  auto flat_con = std::make_unique<FlatteningController>(kHandle, cbks,
                                                         kTestTimeout);

  start_time = std::chrono::steady_clock::now();
  flat_con->NewFrame();

  std::unique_lock<std::mutex> lock(mutex);
  cv.wait_for(lock, kTestTimeout * 2, [&] { return triggered; });

  EXPECT_TRUE(triggered);
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      trigger_time - start_time);
  EXPECT_GE(duration, kTestTimeout);
  EXPECT_LT(duration, kTestTimeout + kTimeoutEpsilon);
}

TEST_F(FlatteningControllerTest, ResetsTimeoutOnNewFrame) {
  std::mutex mutex;
  std::condition_variable cv;
  bool triggered = false;

  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point trigger_time;

  FlatConCallbacks cbks = {.trigger = [&]() {
    std::lock_guard<std::mutex> lock(mutex);
    trigger_time = std::chrono::steady_clock::now();
    triggered = true;
    cv.notify_one();
  }};
  auto flat_con = std::make_unique<FlatteningController>(kHandle, cbks,
                                                         kTestTimeout);

  flat_con->NewFrame();
  std::this_thread::sleep_for(kTestTimeout / 2);

  // NewFrame is called before the original timeout has expired.
  start_time = std::chrono::steady_clock::now();
  flat_con->NewFrame();

  std::unique_lock<std::mutex> lock(mutex);
  cv.wait_for(lock, kTestTimeout * 2, [&] { return triggered; });

  EXPECT_TRUE(triggered);
  auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
      trigger_time - start_time);
  EXPECT_GE(duration, kTestTimeout);
  EXPECT_LT(duration, kTestTimeout + kTimeoutEpsilon);
}

}  // namespace android::drm_hwcomposer
