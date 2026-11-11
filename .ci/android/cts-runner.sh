#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e

EXIT_CODE=1

if [[ -z ${CI_PROJECT_DIR} ]]; then
    CI_PROJECT_DIR="$(dirname "${0}")/../.."
fi

export NEED_ANDROID_CTS=1
source "${CI_PROJECT_DIR}/.ci/android/launch-cvd.sh"

fdo_log_section_start_collapsed run_android_cts "run_android_cts"

INCLUDE_FILE="${CI_PROJECT_DIR}/.ci/android/cts-includes.txt"
CTS_FILTER_ARGS=()
while IFS= read -r filter; do
  CTS_FILTER_ARGS+=(--include-filter "$filter")
done < <(grep -v -E "(^#|^[[:space:]]*$)" "$INCLUDE_FILE" | sed -e 's/[[:space:]]*$//')

EXCLUDE_FILE="${CI_PROJECT_DIR}/.ci/android/cts-excludes.txt"
if [ -e "$EXCLUDE_FILE" ]; then
  while IFS= read -r filter; do
    CTS_FILTER_ARGS+=(--exclude-filter "$filter")
  done < <(grep -v -E "(^#|^[[:space:]]*$)" "$EXCLUDE_FILE" | sed -e 's/[[:space:]]*$//')
fi

set +e
# Bound cts-tradefed to 25m (normal 387-test run is ~7m) so an init/shutdown
# deadlock (e.g. LowDiskSpaceException in CommandScheduler, job #115382266)
# cannot hold the Cuttlefish container open for 48m, and point java.io.tmpdir
# into CVD_TMP_DIR so Tradefed/OLC temp files stay out of container /tmp.
JAVA_TOOL_OPTIONS="-Djava.io.tmpdir=${CVD_TMP_DIR}" \
  timeout -k 30s 25m "${TOOLS_ROOT}/android-cts/tools/cts-tradefed" \
  run commandAndExit cts-dev \
  "${CTS_FILTER_ARGS[@]}" --log-level-display ASSERT
CTS_STATUS=$?
if [ "${CTS_STATUS}" -eq 124 ]; then
  EXIT_CODE=124
  exit 124
fi

cp -r "${TOOLS_ROOT}/android-cts/results/latest"/* "${RESULTS_DIR}" 2>/dev/null || true
cp -r "${TOOLS_ROOT}/android-cts/logs/latest"/* "${RESULTS_DIR}" 2>/dev/null || true

ACTUAL_ASSUMPTION_FAILURES_FILE="${RESULTS_DIR}/cts_results_actual_assumption_failures.txt"
awk '
  /<Module / {
    match($0, /name="[^"]+"/); mod = substr($0, RSTART + 6, RLENGTH - 7)
    match($0, /abi="[^"]+"/);  abi = substr($0, RSTART + 5, RLENGTH - 6)
  }
  /<TestCase / {
    match($0, /name="[^"]+"/); tc = substr($0, RSTART + 6, RLENGTH - 7)
  }
  /<Test / && /result="ASSUMPTION_FAILURE"/ {
    match($0, /name="[^"]+"/); test = substr($0, RSTART + 6, RLENGTH - 7)
    print abi, mod, tc "#" test
  }
' "${RESULTS_DIR}/test_result.xml" > "${ACTUAL_ASSUMPTION_FAILURES_FILE}"

check_expected_assumption_failures \
  "${CI_PROJECT_DIR}/.ci/android/cts-assumption-failures.txt" \
  "${ACTUAL_ASSUMPTION_FAILURES_FILE}"
UNEXPECTED_ASSUMPTION_FAILURES=$?

# Even if there are failed tests, eval will exit with 0, so check for failures manually.
SUMMARY_FILE="${RESULTS_DIR}"/invocation_summary.txt

# Parse a line like `1/2 modules completed` to check that all modules completed
COMPLETED_MODULES=$(sed -n -e '/modules completed/s/^\([0-9]\+\)\/\([0-9]\+\) .*$/\1/p' "${SUMMARY_FILE}")
AVAILABLE_MODULES=$(sed -n -e '/modules completed/s/^\([0-9]\+\)\/\([0-9]\+\) .*$/\2/p' "${SUMMARY_FILE}")
[ "${COMPLETED_MODULES}" = "${AVAILABLE_MODULES}" ]
# shellcheck disable=SC2319  # False-positive see https://github.com/koalaman/shellcheck/issues/2937#issuecomment-2660891195
MODULES_FAILED=$?

[ "$(grep "^FAILED" "${SUMMARY_FILE}" | tr -d ' ' | cut -d ':' -f 2)" = "0" ]
TESTS_FAILED=$?

[ "${MODULES_FAILED}" = "0" ] && \
[ "${TESTS_FAILED}" = "0" ] && \
[ "${UNEXPECTED_ASSUMPTION_FAILURES}" = "0" ]
# shellcheck disable=SC2034
EXIT_CODE=$?
set -e

fdo_log_section_end run_android_cts
