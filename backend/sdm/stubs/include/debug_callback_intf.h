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

namespace sdm {

enum class DebugLogType {
  ERROR,
  WARNING,
  INFO,
  DEBUG,
  VERBOSE,
};

class DebugCallbackIntf {
 public:
  virtual ~DebugCallbackIntf() = default;
  virtual void Log(DebugLogType type, const char *log_tag, const char *fmt,
                   std::va_list &args) = 0;
  virtual int GetProperty(const char *property_name, int *value) {
    (void)property_name;
    (void)value;
    return 0;
  }
  virtual int GetProperty(const char *property_name, char *value) = 0;
  virtual void BeginTrace(const char *class_name, const char *function_name,
                          const char *custom_string) = 0;
  virtual void EndTrace() = 0;
  virtual void ATrace(const char *custom_string, int bit) = 0;
};

}  // namespace sdm
