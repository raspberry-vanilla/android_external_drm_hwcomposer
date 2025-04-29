#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e

EXIT_CODE=1

if [[ -z ${CI_PROJECT_DIR} ]]; then
    CI_PROJECT_DIR="$(dirname "${0}")/../.."
fi

source "${CI_PROJECT_DIR}/.ci/android/launch-cvd.sh"

section_start run_android_cts "run_android_cts"
set -x

INCLUDE_FILE="${CI_PROJECT_DIR}/.ci/android/cts-includes.txt"
# shellcheck disable=SC2086 # keep word splitting
INCLUDE_FILTERS="$(grep -v -E "(^#|^[[:space:]]*$)" "$INCLUDE_FILE" | sed -e 's/\s*$//g' -e 's/.*/--include-filter "\0" /g')"

# shellcheck disable=SC2086 # keep word splitting
eval /android-tools/android-cts/tools/cts-tradefed run commandAndExit cts-dev \
  $INCLUDE_FILTERS --log-level-display ASSERT

# Even if there are failed tests, eval will exit with 0, so check for failures manually.
[ "$(grep "^FAILED" /android-tools/android-cts/results/latest/invocation_summary.txt | tr -d ' ' | cut -d ':' -f 2)" = "0" ]
export EXIT_CODE=$?
section_end run_android_cts

cp -r "/android-tools/android-cts/results/latest"/* "${RESULTS_DIR}"
cp -r "/android-tools/android-cts/logs/latest"/* "${RESULTS_DIR}"
