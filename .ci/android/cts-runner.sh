#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e

EXIT_CODE=1

if [[ -z ${CI_PROJECT_DIR} ]]; then
    CI_PROJECT_DIR="$(dirname "${0}")/../.."
fi

source "${CI_PROJECT_DIR}/.ci/android/launch-cvd.sh"

fdo_log_section_start_collapsed run_android_cts "run_android_cts"

INCLUDE_FILE="${CI_PROJECT_DIR}/.ci/android/cts-includes.txt"
# shellcheck disable=SC2086 # keep word splitting
INCLUDE_FILTERS="$(grep -v -E "(^#|^[[:space:]]*$)" "$INCLUDE_FILE" | sed -e 's/\s*$//g' -e 's/.*/--include-filter "\0" /g')"

SKIP_FILE="${CI_PROJECT_DIR}/.ci/android/cts-skips.txt"
EXCLUDE_FILTERS=""
if [ -e "$SKIP_FILE" ]; then
  EXCLUDE_FILTERS="$(grep -v -E "(^#|^[[:space:]]*$)" "$SKIP_FILE" | sed -e 's/\s*$//g' -e 's/.*/--exclude-filter "\0" /g')"
fi

set +e
# shellcheck disable=SC2086 # keep word splitting
eval /android-tools/android-cts/tools/cts-tradefed run commandAndExit cts-dev \
  $INCLUDE_FILTERS $EXCLUDE_FILTERS --log-level-display ASSERT

cp -r "/android-tools/android-cts/results/latest"/* "${RESULTS_DIR}"
cp -r "/android-tools/android-cts/logs/latest"/* "${RESULTS_DIR}"

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

[ "${MODULES_FAILED}" = "0" ] && [ "${TESTS_FAILED}" = "0" ]
# shellcheck disable=SC2034
EXIT_CODE=$?
set -e

fdo_log_section_end run_android_cts
