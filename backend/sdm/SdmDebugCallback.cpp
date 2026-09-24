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

#define LOG_TAG "drmhwc"  // NOLINT(cppcoreguidelines-macro-usage)

#include "SdmDebugCallback.h"

#include <cstdarg>
#include <cstdio>

#include <android/log.h>
#include <cutils/properties.h>
#include <cutils/trace.h>
#include <linux/limits.h>

#include <core/sdm_types.h>
#include <debug_callback_intf.h>

namespace android::drm_hwcomposer {
namespace {
android_LogPriority SdmLogLevelToAndroidLogLevel(sdm::DebugLogType type) {
  switch (type) {
    case sdm::DebugLogType::ERROR:
      return ANDROID_LOG_ERROR;
    case sdm::DebugLogType::WARNING:
      return ANDROID_LOG_WARN;
    case sdm::DebugLogType::INFO:
      return ANDROID_LOG_INFO;
    case sdm::DebugLogType::DEBUG:
      return ANDROID_LOG_DEBUG;
    case sdm::DebugLogType::VERBOSE:
      return ANDROID_LOG_VERBOSE;
  }
  return ANDROID_LOG_INFO;
}
}  // namespace

void SdmDebugCallback::Log(sdm::DebugLogType type, const char *log_tag,
                           const char *fmt, std::va_list &args) {
  auto log_level = SdmLogLevelToAndroidLogLevel(type);
  __android_log_vprint(log_level, log_tag, fmt, args);
}

int SdmDebugCallback::GetProperty(const char *property_name, int *value) {
  char property[PROPERTY_VALUE_MAX];
  if (property_get(property_name, property, nullptr) <= 0) {
    return sdm::kErrorNotSupported;
  }
  *value = property_get_int32(property_name, 0);
  return sdm::kErrorNone;
}

int SdmDebugCallback::GetProperty(const char *property_name, char *value) {
  if (property_get(property_name, value, nullptr) > 0) {
    return sdm::kErrorNone;
  }
  return sdm::kErrorNotSupported;
}

void SdmDebugCallback::BeginTrace(const char *class_name,
                                  const char *function_name,
                                  const char *custom_string) {
  if (atrace_is_tag_enabled(ATRACE_TAG_GRAPHICS) != 0) {
    char name[PATH_MAX] = {0};
    snprintf(name, sizeof(name), "%s::%s::%s", class_name, function_name,
             custom_string);
    atrace_begin(ATRACE_TAG_GRAPHICS, name);
  }
}

void SdmDebugCallback::EndTrace() {
  atrace_end(ATRACE_TAG_GRAPHICS);
}

void SdmDebugCallback::ATrace(const char *custom_string, const int bit) {
  ATRACE_INT(custom_string, bit);
}

}  // namespace android::drm_hwcomposer