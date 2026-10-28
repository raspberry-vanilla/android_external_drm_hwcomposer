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

#pragma once

#include <optional>
#include <string>

namespace android::drm_hwcomposer {

// RAII helper to temporarily set a system property during a test and restore
// its previous value upon scope exit. Under Android builds, system properties
// are backed by libcutils (property_get/property_set), whereas standalone
// non-Android builds read from environment variables (getenv/setenv/unsetenv).
class ScopedTestProperty {
 public:
  ScopedTestProperty(const char *key, const char *value);
  ~ScopedTestProperty();

  ScopedTestProperty(const ScopedTestProperty &) = delete;
  auto operator=(const ScopedTestProperty &) -> ScopedTestProperty & = delete;
  ScopedTestProperty(ScopedTestProperty &&) = delete;
  auto operator=(ScopedTestProperty &&) -> ScopedTestProperty & = delete;

 private:
  std::string key_;
  std::optional<std::string> prev_value_;
};

}  // namespace android::drm_hwcomposer
