#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e

EXIT_CODE=1

if [[ -z ${CI_PROJECT_DIR} ]]; then
    CI_PROJECT_DIR="$(dirname "${0}")/../.."
fi

source "${CI_PROJECT_DIR}/.ci/android/launch-cvd.sh"

fdo_log_section_start_collapsed run_android_vts "run_android_vts"
adb wait-for-device devices
adb root
adb push /VtsHalGraphicsComposer3_TargetTest /data/local/tmp/
adb shell stop surfaceflinger

SKIP_FILE="${CI_PROJECT_DIR}/.ci/android/vts-skips.txt"
EXCLUDE_FILTERS="$(grep -v -E "^(#|[[:space:]]*$)" "$SKIP_FILE" | paste -sd: -)"

# Run the entire command inside adb shell to capture stdout/err
adb shell "(/data/local/tmp/VtsHalGraphicsComposer3_TargetTest \
  --gtest_filter=-$EXCLUDE_FILTERS 2>&1 | tee /data/local/tmp/vts_results.txt)"

adb pull /data/local/tmp/vts_results.txt "${RESULTS_DIR}"

! grep "FAILED" "${RESULTS_DIR}/vts_results.txt"
export EXIT_CODE=$?

fdo_log_section_end run_android_vts
