#!/usr/bin/env bash
# no need to follow references to other shell scripts
# shellcheck disable=SC1090
# shellcheck disable=SC1091
# Bump the UBUNTU_ANDROID_TAG for changes in this file to take effect.

set -e

function my_atexit()
{
  local exit_status=$?
  set +e
  apt-get purge -y "${EPHEMERAL_DEPS[@]}"
  apt-get autoremove -y
  apt-get clean
  exit $exit_status
}

trap my_atexit EXIT
trap 'exit 2' HUP INT PIPE TERM

source "${FDO_CI_BASH_HELPERS}"

DEPS=(
  ca-certificates
  git
  unzip
  xz-utils
)

EPHEMERAL_DEPS=(
  skopeo
  umoci
)

export DEBIAN_FRONTEND=noninteractive

fdo_log_section_start_collapsed install_packages "install_packages"
apt-get update
apt-get upgrade -y
apt-get install -y --no-remove --no-install-recommends "${DEPS[@]}"
apt-get install -y --no-remove --no-install-recommends "${EPHEMERAL_DEPS[@]}"
fdo_log_section_end install_packages

TMP_DIR=/tmp/my_oci
fdo_log_section_start_collapsed get_cuttlefish "get_cuttlefish"
mkdir -p "${TMP_DIR}"
skopeo copy --retry-times 3 \
  --src-creds "${CI_REGISTRY_USER}:${CI_JOB_TOKEN}" \
  "docker://${CI_REGISTRY_IMAGE}/${DEBIAN_CUTTLEFISH_IMAGE}:${DEBIAN_CUTTLEFISH_TAG}--${CI_TEMPLATES_COMMIT}" \
  "oci:${TMP_DIR}/cuttlefish-oci:cf"
umoci unpack --image "${TMP_DIR}/cuttlefish-oci:cf" "${TMP_DIR}"
cp -a "${TMP_DIR}/rootfs/${CUTTLEFISH_TARBALL}"  /"${CUTTLEFISH_TARBALL}"
rm --preserve-root "${TMP_DIR}" -rf
fdo_log_section_end get_cuttlefish

fdo_log_section_start_collapsed get_binaries "get_binaries"
mkdir -p "${TMP_DIR}"
skopeo copy --retry-times 3 \
  --src-creds "${CI_REGISTRY_USER}:${CI_JOB_TOKEN}" \
  "docker://${CI_REGISTRY_IMAGE}/${UBUNTU_ANDROID_BINARIES_IMAGE}:${UBUNTU_ANDROID_BINARIES_TAG}--${CI_TEMPLATES_COMMIT}" \
  "oci:${TMP_DIR}/binaries-oci:cf"
umoci unpack --image "${TMP_DIR}/binaries-oci:cf" "${TMP_DIR}"
cp -a "${TMP_DIR}/rootfs/${BINARIES_DIR}"  /"${BINARIES_DIR}"
rm --preserve-root "${TMP_DIR}" -rf
fdo_log_section_end get_binaries

fdo_log_section_start_collapsed get_tools "get_tools"
mkdir -p "${TMP_DIR}"
skopeo copy --retry-times 3 \
  --src-creds "${CI_REGISTRY_USER}:${CI_JOB_TOKEN}" \
  "docker://${CI_REGISTRY_IMAGE}/${UBUNTU_ANDROID_TOOLS_IMAGE}:${UBUNTU_ANDROID_TOOLS_TAG}--${CI_TEMPLATES_COMMIT}" \
  "oci:${TMP_DIR}/tools-oci:cf"
umoci unpack --image "${TMP_DIR}/tools-oci:cf" "${TMP_DIR}"
cp -a "${TMP_DIR}/rootfs/${ANDROID_TOOLS_TARBALL}"  /"${ANDROID_TOOLS_TARBALL}"
rm --preserve-root "${TMP_DIR}" -rf
fdo_log_section_end get_tools
