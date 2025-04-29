#! /usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e

source "./.ci/setup-test-env.sh"

section_start build_drmhwc_arm64 "build_drmhwc_arm64"
set -x
ln -s "${CI_PROJECT_DIR}" "/aospless_arm64/src"
make -C /aospless_arm64 install
mkdir -p "${CI_PROJECT_DIR}/install/arm64"
cp -r /aospless_arm64/install/* "${CI_PROJECT_DIR}/install/arm64"
set +x
section_end build_drmhwc_arm64
