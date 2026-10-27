#! /usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -euo pipefail
shopt -s expand_aliases

if [ -n "${FDO_CI_BASH_HELPERS:-}" ] && [ -f "${FDO_CI_BASH_HELPERS}" ]; then
  source "${FDO_CI_BASH_HELPERS}"
  _fdo_log_section_start_collapsed run_unittests "run_unittests"
fi

CI_PROJECT_DIR="${CI_PROJECT_DIR:-$(realpath "$(dirname "$0")/..")}"
RESULTS_DIR="${RESULTS_DIR:-${CI_PROJECT_DIR}/results}"

mkdir -p "${RESULTS_DIR}"

make -j"$(nproc)" -f "${CI_PROJECT_DIR}/.ci/Makefile.unittests" check \
  GTEST_ARGS="--gtest_output=xml:${RESULTS_DIR}/unittests.xml"

if [ -n "${FDO_CI_BASH_HELPERS:-}" ] && [ -f "${FDO_CI_BASH_HELPERS}" ]; then
  _fdo_log_section_end run_unittests
fi
