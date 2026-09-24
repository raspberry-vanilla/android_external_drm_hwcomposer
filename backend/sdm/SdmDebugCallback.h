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

#include <cstdarg>

#include <debug_callback_intf.h>

namespace android::drm_hwcomposer {

// sdm::DebugCallbackIntf provides an interface for logging and tracing.
class SdmDebugCallback : public sdm::DebugCallbackIntf {
 public:
  void Log(sdm::DebugLogType type, const char *log_tag, const char *fmt,
           std::va_list &args) override;

  int GetProperty(const char *property_name, int *value) override;
  int GetProperty(const char *property_name, char *value) override;
  void BeginTrace(const char *class_name, const char *function_name,
                  const char *custom_string) override;
  void EndTrace() override;
  void ATrace(const char *custom_string, int bit) override;
};

}  // namespace android::drm_hwcomposer
