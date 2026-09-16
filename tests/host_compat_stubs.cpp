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

#include <sys/types.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>  // IWYU pragma: keep
#include <fstream>
#include <ios>
#include <sstream>
#include <string>

#include <android-base/file.h>
#include <hardware/hardware.h>

// ---------------------------------------------------------------------------
// Hardware Module Stub
//
// The real AOSP implementation in hardware/libhardware/hardware.c attempts
// to dlopen HAL shared libraries from Android device partitions
// (/system/lib64/hw, /vendor/lib64/hw). These paths do not exist in host Linux
// environments without full device sysroots, and probing will always return a
// failure code. Stubbing hw_get_module to return -1 avoids device-specific HAL
// loader dependencies in host testing.
// ---------------------------------------------------------------------------
extern "C" __attribute__((weak)) int hw_get_module(
    const char* /*id*/, const struct hw_module_t** /*module*/) {
  return -1;
}

// ---------------------------------------------------------------------------
// TemporaryFile & libbase File I/O Stubs
//
// The real AOSP implementations in system/libbase/file.cpp and test_utils.cpp
// depend on internal libbase logging, utf8 conversions, and LLVM C++23 runtime
// features (e.g. std::string::resize_and_overwrite) that are incompatible with
// host GCC/libstdc++ gnu++17 toolchains. These lightweight POSIX shims provide
// standalone equivalents using mkstemp, std::ifstream, and std::ofstream
// without pulling in the entire LLVM libc++ runtime.
// ---------------------------------------------------------------------------
void TemporaryFile::init(const std::string& tmp_dir) {
  snprintf(path, sizeof(path), "%s/TemporaryFile-XXXXXX", tmp_dir.c_str());
  fd = mkstemp(path);  // NOLINT(misc-include-cleaner)
}

TemporaryFile::TemporaryFile() : fd(-1), path{} {
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char* tmpdir = getenv("TMPDIR");
  init(tmpdir != nullptr ? tmpdir : "/tmp");
}

TemporaryFile::TemporaryFile(const std::string& tmp_dir) : fd(-1), path{} {
  init(tmp_dir);
}

TemporaryFile::~TemporaryFile() {
  if (fd >= 0) {
    close(fd);
  }
  if (remove_file_) {
    unlink(path);
  }
}

int TemporaryFile::release() {
  int result = fd;
  fd = -1;
  return result;
}

namespace android::base {

bool ReadFileToString(const std::string& content_path, std::string* content,
                      bool /*follow_symlinks*/) {
  std::ifstream f(content_path, std::ios::in | std::ios::binary);
  if (!f.is_open())
    return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  *content = ss.str();
  return true;
}

bool WriteStringToFile(const std::string& content,
                       const std::string& content_path,
                       bool /*follow_symlinks*/) {
  std::ofstream f(content_path,
                  std::ios::out | std::ios::binary | std::ios::trunc);
  if (!f.is_open())
    return false;
  f << content;
  return f.good();
}

bool WriteStringToFile(const std::string& content,
                       const std::string& content_path, mode_t /*mode*/,
                       uid_t /*owner*/, gid_t /*group*/, bool follow_symlinks) {
  return WriteStringToFile(content, content_path, follow_symlinks);
}

}  // namespace android::base
