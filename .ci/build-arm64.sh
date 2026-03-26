#! /usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e

source "${FDO_CI_BASH_HELPERS}"

fdo_log_section_start_collapsed build_drmhwc_arm64 "build_drmhwc_arm64"

ln -s "${CI_PROJECT_DIR}" "/aospless_arm64/src"
make -C /aospless_arm64 install STD=gnu++23
mkdir -p "${CI_PROJECT_DIR}/install/arm64"
cp -r /aospless_arm64/install/* "${CI_PROJECT_DIR}/install/arm64"
fdo_log_section_end build_drmhwc_arm64
