#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e

EXIT_CODE=1

source "${CI_PROJECT_DIR}/.ci/android/vts-runner.sh"

fdo_log_section_start_collapsed CrosvmDisplay "CrosvmDisplay"
set +e
run_vts "crosvm"
# shellcheck disable=SC2034
export EXIT_CODE=$?
fdo_log_section_end CrosvmDisplay
