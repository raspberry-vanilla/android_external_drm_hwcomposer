#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e

EXIT_CODE=1

if [[ -z ${CI_PROJECT_DIR} ]]; then
    CI_PROJECT_DIR="$(dirname "${0}")/../.."
fi

source "${CI_PROJECT_DIR}/.ci/android/launch-cvd.sh"

fdo_log_section_start_collapsed run_vkms_hotplugs "run_vkms_hotplugs"
adb wait-for-device devices
adb root
adb push "/${BINARIES_DIR}/setup_vkms_connectors_for_atest" /data/local/tmp/setup_vkms_connectors_for_atest
adb push "/${BINARIES_DIR}/test_hotplugs" /data/local/tmp/test_hotplugs
adb push "/${BINARIES_DIR}/teardown_vkms" /data/local/tmp/teardown_vkms

adb shell logcat -c;

SKIP_FILE="${CI_PROJECT_DIR}/.ci/android/vkms-hotplugs-skips.txt"
EXCLUDE_FILTERS="$(grep -v -E "^(#|[[:space:]]*$)" "$SKIP_FILE" | paste -sd: -)"

# Run the entire command inside adb shell to capture stdout/err
ADB_TEST_CMD="(/data/local/tmp/test_hotplugs \
  --gtest_filter=-$EXCLUDE_FILTERS 2>&1 |\
   tee /data/local/tmp/vkms_hotplugs_results.txt)"

# If drm-hwcomposer crashes, the test may timeout rather than fail
set +e
timeout -k 30s 5m adb shell "${ADB_TEST_CMD}"
VKMS_TIMED_OUT=$?
adb shell logcat -d | grep -E "hwc|test_hotplugs" > "${RESULTS_DIR}/logcat_hotplugs.txt"
adb pull /data/local/tmp/vkms_hotplugs_results.txt "${RESULTS_DIR}"

! grep "FAILED" "${RESULTS_DIR}/vkms_hotplugs_results.txt"
VKMS_FAILED=$?

[ "${VKMS_TIMED_OUT}" = "0" ] && [ "${VKMS_FAILED}" = "0" ]
# shellcheck disable=SC2034
EXIT_CODE=$?
set -e

fdo_log_section_end run_vkms_hotplugs
