#! /usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e

source "./.ci/setup-test-env.sh"

section_start build_drmhwc_x86_64 "build_drmhwc_x86_64"
set -x
sed -i "/'-DUSE_IMAPPER4_METADATA_API'/a\    '-D__ANDROID_API__=${ANDROID_SDK_VERSION}'," \
    "${CI_PROJECT_DIR}/meson.build"

ln -s "${CI_PROJECT_DIR}" "/aospless_x86_64/src"
make -C /aospless_x86_64 install
mkdir -p "${CI_PROJECT_DIR}/install/x86_64"
cp -r /aospless_x86_64/install/* "${CI_PROJECT_DIR}/install/x86_64"
set +x
section_end build_drmhwc_arm64
