#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e

EXIT_CODE=1

source "${CI_PROJECT_DIR}/.ci/android/vts-runner.sh"

fdo_log_section_start_collapsed headless "headless"
reload_vkms
configure_vkms
stop_hwc
adb shell "echo 0 > ${VKMS_DIR}/enabled"
adb shell "echo 2 > ${VKMS_DIR}/connectors/CONNECTOR_0/status"   # disconnected=2
adb shell "echo 1 > ${VKMS_DIR}/enabled"
print_vkms_config
start_hwc
set +e
run_vts "headless"
# shellcheck disable=SC2034
export EXIT_CODE=$?
fdo_log_section_end headless
