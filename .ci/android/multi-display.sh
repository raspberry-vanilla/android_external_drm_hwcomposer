#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e

EXIT_CODE=1

source "${CI_PROJECT_DIR}/.ci/android/vts-runner.sh"

fdo_log_section_start_collapsed three_displays "three_displays"
reload_vkms
export CONNECTOR_TYPES="eDP DP HDMIA"
configure_vkms
set +e
run_vts "three-displays" "multi-display-skips.txt"
# shellcheck disable=SC2034
export EXIT_CODE=$?
fdo_log_section_end three_displays
