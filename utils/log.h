/*
 * Copyright (C) 2022 The Android Open Source Project
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

#ifdef ANDROID

#include <log/log.h>
#include <log/log_main.h>  // IWYU pragma: export

#else

#include <cinttypes>
#include <cstdio>

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGE(args...) fprintf(stderr, "ERR: " args)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGW(args...) fprintf(stderr, "WARN: " args)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGI(args...) printf("INFO: " args)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGD(args...) printf("DBG:" args)
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGV(args...) printf("VERBOSE: " args)

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGE_IF(cond, args...) \
  if (cond)                     \
    ALOGE(args);                \
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGW_IF(cond, args...) \
  if (cond)                     \
    ALOGW(args);                \
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGI_IF(cond, args...) \
  if (cond)                     \
    ALOGI(args);                \
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGD_IF(cond, args...) \
  if (cond)                     \
    ALOGD(args);                \
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define ALOGV_IF(cond, args...) \
  if (cond)                     \
    ALOGV(args);

#endif
