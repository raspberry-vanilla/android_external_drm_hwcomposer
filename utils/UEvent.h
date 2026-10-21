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

#if !defined(__GLIBC__)
#include <asm-generic/poll.h>
#include <asm-generic/socket.h>
#include <linux/eventfd.h>
#endif
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__GLIBC__) && !defined(_UAPI_LINUX_SOCKET_H)
#define _UAPI_LINUX_SOCKET_H
using __kernel_sa_family_t = unsigned short;
#define DRM_HWC_DEFINED_UAPI_LINUX_SOCKET_H
#endif
#include <linux/netlink.h>
#if defined(DRM_HWC_DEFINED_UAPI_LINUX_SOCKET_H)
#undef _UAPI_LINUX_SOCKET_H
#undef DRM_HWC_DEFINED_UAPI_LINUX_SOCKET_H
#endif

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "utils/fd.h"
#include "utils/log.h"

namespace android::drm_hwcomposer {

class UEvent {
 public:
  static auto CreateInstance() -> std::unique_ptr<UEvent> {
    auto fd = MakeUniqueFd(
        socket(PF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT));

    if (!fd) {
      ALOGE("Failed to open uevent socket: errno=%i", errno);
      return {};
    }

    constexpr int kNetlinkRecvBufferSize = 1024 * 1024;
    if (setsockopt(*fd, SOL_SOCKET, SO_RCVBUF, &kNetlinkRecvBufferSize,
                   sizeof(kNetlinkRecvBufferSize)) != 0) {
      ALOGW("Failed to set uevent socket SO_RCVBUF: errno=%i", errno);
    }

    struct sockaddr_nl addr {};
    addr.nl_family = AF_NETLINK;
    addr.nl_pid = 0;
    addr.nl_groups = UINT32_MAX;

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    const int ret = bind(*fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret != 0) {
      ALOGE("Failed to bind uevent socket: errno=%i", errno);
      return {};
    }

    return CreateInstanceWithFd(std::move(fd));
  }

  static auto CreateInstanceWithFd(UniqueFd fd) -> std::unique_ptr<UEvent> {
    if (!fd) {
      return {};
    }
    auto stop_event_fd = MakeUniqueFd(eventfd(0, EFD_CLOEXEC));
    if (!stop_event_fd) {
      ALOGE("Failed to create eventfd: errno=%i", errno);
      return {};
    }

    return std::unique_ptr<UEvent>(new UEvent(fd, stop_event_fd));
  }

  auto ReadNext() -> std::optional<std::string> {
    return ReadNextWithTimeout(-1);
  }

  auto ReadNextWithTimeout(int timeout_ms) -> std::optional<std::string> {
    if (!WaitForData(timeout_ms)) {
      return {};
    }
    return ReadPacket();
  }

  auto ReadNextNonBlocking() -> std::optional<std::string> {
    return ReadNextWithTimeout(0);
  }

  void Stop() {
    // Increment the eventfd by writing 1. All subsequent calls to ReadNext will
    // return false.
    const uint64_t value = 1;
    const ssize_t ret = write(*stop_event_fd_, &value, sizeof(value));
    if (ret == -1) {
      ALOGE("Error writing to eventfd. errno: %d", errno);
    } else if (ret != sizeof(value)) {
      ALOGE("Wrote fewer bytes to eventfd than expected: %zd vs %zd", ret,
            sizeof(value));
    }
  }

 private:
  enum { kFdIdx = 0, kStopEventFdIdx, kNumFds };

  UEvent(UniqueFd &fd, UniqueFd &stop_event_fd)
      : fd_(std::move(fd)), stop_event_fd_(std::move(stop_event_fd)) {};

  auto ReadPacket() -> std::optional<std::string> {
    constexpr size_t kUEventBufferSize = 4096;
    char buffer[kUEventBufferSize];

    ssize_t ret = recv(*fd_, &buffer, sizeof(buffer), MSG_TRUNC);
    if (ret == 0) {
      return {};
    }

    if (ret < 0) {
      if (errno == ENOBUFS) {
        ALOGW("UEvent netlink socket overflow (ENOBUFS); synthesizing rescan");
        return std::string("DEVTYPE=drm_minor\nHOTPLUG=1\n");
      }
      ALOGE("Got error reading uevent: errno=%d", errno);
      return {};
    }

    if (static_cast<size_t>(ret) > sizeof(buffer)) {
      ALOGW("UEvent packet truncated (%zd > %zu bytes)", ret, sizeof(buffer));
      ret = static_cast<ssize_t>(sizeof(buffer));
    }

    for (ssize_t i = 0; i < ret; i++) {
      if (buffer[i] == '\0') {
        buffer[i] = '\n';
      }
    }

    return std::string(buffer, static_cast<size_t>(ret));
  }

  // Returns true if there is data to be read off of fd_.
  bool WaitForData(int timeout_ms) {
    struct pollfd poll_fds[kNumFds];
    poll_fds[kFdIdx].fd = *fd_;
    poll_fds[kFdIdx].events = POLLIN;
    poll_fds[kStopEventFdIdx].fd = *stop_event_fd_;
    poll_fds[kStopEventFdIdx].events = POLLIN;

    const int ret = poll(poll_fds, kNumFds, timeout_ms);
    if (ret == 0) {
      return false;
    }
    if (ret < 1) {
      ALOGE("Error polling uevent. errno: %d", errno);
      return false;
    }

    if ((poll_fds[kStopEventFdIdx].revents & POLLIN) != 0) {
      // Stop event has been signalled. Return without reading from the fd to
      // ensure that this fd stays in a readable state.
      ALOGI("Stop event signalled.");
      return false;
    }

    // Return true if there is data or a socket error (such as ENOBUFS) to
    // read so ReadPacket() can clear sk_err and synthesize a rescan.
    return (poll_fds[kFdIdx].revents & (POLLIN | POLLERR)) != 0;
  }

  UniqueFd fd_;
  UniqueFd stop_event_fd_;
};

}  // namespace android::drm_hwcomposer
