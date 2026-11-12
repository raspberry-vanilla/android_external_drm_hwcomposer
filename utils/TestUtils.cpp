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

#include "utils/TestUtils.h"

#include <cstdlib>
#include <optional>
#include <string>

#include <gtest/gtest.h>

#ifdef ANDROID
#include <cutils/properties.h>
#endif

namespace android::drm_hwcomposer {

ScopedTestProperty::ScopedTestProperty(const char *key, const char *value)
    : key_(key) {
#ifdef ANDROID
  // Android system properties are global to the booted device and cannot be
  // unset once written; snapshot the prior value so running unit tests on a
  // DUT does not overwrite /vendor/build.prop defaults with "".
  char prev[PROPERTY_VALUE_MAX] = {};
  property_get(key, prev, "");
  prev_value_ = prev;
  EXPECT_EQ(property_set(key, value != nullptr ? value : ""), 0);
#else
  if (const char *prev = getenv(key); prev != nullptr) {
    prev_value_ = prev;
  }
  if (value != nullptr) {
    EXPECT_EQ(setenv(key, value, 1), 0);
  } else {
    EXPECT_EQ(unsetenv(key), 0);
  }
#endif
}

ScopedTestProperty::~ScopedTestProperty() {
#ifdef ANDROID
  EXPECT_EQ(property_set(key_.c_str(), prev_value_ ? prev_value_->c_str() : ""),
            0);
#else
  if (prev_value_.has_value()) {
    EXPECT_EQ(setenv(key_.c_str(), prev_value_->c_str(), 1), 0);
  } else {
    EXPECT_EQ(unsetenv(key_.c_str()), 0);
  }
#endif
}

}  // namespace android::drm_hwcomposer
