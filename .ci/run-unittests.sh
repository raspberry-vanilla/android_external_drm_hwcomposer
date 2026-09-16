#! /usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e
shopt -s expand_aliases

if [ -n "${FDO_CI_BASH_HELPERS}" ] && [ -f "${FDO_CI_BASH_HELPERS}" ]; then
  source "${FDO_CI_BASH_HELPERS}"
  _fdo_log_section_start_collapsed run_unittests "run_unittests"
fi

CI_PROJECT_DIR="${CI_PROJECT_DIR:-$(realpath "$(dirname "$0")/..")}"
RESULTS_DIR="${RESULTS_DIR:-${CI_PROJECT_DIR}/results}"

apt-get update -qq && apt-get install -y --no-install-recommends libdrm-dev liblz4-dev

mkdir -p "${RESULTS_DIR}"

AOSPLESS_DIR="${HOST_AOSPLESS_DIR:-/aospless_x86_64}"
if [ ! -d "${AOSPLESS_DIR}" ] && [ -d "${CI_PROJECT_DIR}/../aospless" ]; then
  AOSPLESS_DIR="${CI_PROJECT_DIR}/../aospless"
fi

# Populate packaged sources if running against a container image prior to rebuild
if [ -d "${AOSPLESS_DIR}" ] && [ ! -f "${AOSPLESS_DIR}/aosp_src/ColorSpace.cpp" ]; then
  mkdir -p "${AOSPLESS_DIR}/aosp_src"
  if [ -f "${CI_PROJECT_DIR}/../../frameworks/native/libs/ui/ColorSpace.cpp" ]; then
    cp "${CI_PROJECT_DIR}/../../frameworks/native/libs/ui/ColorSpace.cpp" "${AOSPLESS_DIR}/aosp_src/"
    cp "${CI_PROJECT_DIR}/../../system/core/libcutils/trace-host.cpp" "${AOSPLESS_DIR}/aosp_src/"
  else
    curl -s -f "https://android.googlesource.com/platform/frameworks/native/+/refs/heads/${ANDROID_BRANCH:-android17-release}/libs/ui/ColorSpace.cpp?format=TEXT" | base64 -d > "${AOSPLESS_DIR}/aosp_src/ColorSpace.cpp"
    curl -s -f "https://android.googlesource.com/platform/system/core/+/refs/heads/${ANDROID_BRANCH:-android17-release}/libcutils/trace-host.cpp?format=TEXT" | base64 -d > "${AOSPLESS_DIR}/aosp_src/trace-host.cpp"
  fi
fi

make -j"$(nproc)" -f "${CI_PROJECT_DIR}/.ci/Makefile.unittests" check \
  GTEST_ARGS="--gtest_output=xml:${RESULTS_DIR}/unittests.xml"

if [ -n "${FDO_CI_BASH_HELPERS}" ] && [ -f "${FDO_CI_BASH_HELPERS}" ]; then
  _fdo_log_section_end run_unittests
fi
