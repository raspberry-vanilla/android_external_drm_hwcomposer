#!/usr/bin/env bash
# shellcheck disable=SC1090 # no need to follow references to other shell scripts
# For changes in this file to take effect, bump both:
# UBUNTU_ANDROID_TOOLS_TAG and UBUNTU_ANDROID_TAG


set -e

function my_atexit()
{
  set +e
  apt-get purge -y "${EPHEMERAL_DEPS[@]}"
  apt-get autoremove -y
  apt-get clean
  rm --preserve-root "${TOOLS_DIR}" -rf
  rm --preserve-root /root/.cache/ -rf
  rm --preserve-root /root/.npm/ -rf
  rm --preserve-root /tmp/* -rf
}

trap my_atexit EXIT
trap 'exit 2' HUP INT PIPE TERM

source "${FDO_CI_BASH_HELPERS}"

DEPS=(
  ca-certificates
  curl
  git
  sudo
  wget
  xz-utils
)

EPHEMERAL_DEPS=(
  bazel
  binutils
  curl
  emacsen-common
  gcc
  gettext
  git
  gnupg
  golang
  libfakeroot
  m4
  make
  man-db
  perl
  python3
  openssl
)

export DEBIAN_FRONTEND=noninteractive
ln -fs /usr/share/zoneinfo/UTC /etc/localtime # suppress tzdata prompt

fdo_log_section_start_collapsed install_packages "install_packages"
apt-get update
apt-get upgrade -y
apt-get install -y --no-remove --no-install-recommends "${DEPS[@]}"
fdo_log_section_end install_packages

: "${TOOLS_DIR:?TOOLS_DIR is not set}"
mkdir "${TOOLS_DIR}"

# Build debian packages for cuttlefish
fdo_log_section_start_collapsed get_cuttlefish_packages "get_cuttlefish_packages"
ANDROID_CUTTLEFISH_VERSION=v1.17.0
CUT_DIR="/android-cuttlefish"
mkdir "${CUT_DIR}"
pushd "${CUT_DIR}"
git init
git remote add origin https://github.com/google/android-cuttlefish.git
git fetch --depth 1 origin "${ANDROID_CUTTLEFISH_VERSION}"
git checkout FETCH_HEAD
${CUT_DIR}/tools/buildutils/build_packages.sh
popd
cp -a "${CUT_DIR}"/cuttlefish-base_*.deb "${TOOLS_DIR}"
cp -a "${CUT_DIR}"/cuttlefish-user_*.deb "${TOOLS_DIR}"
rm "${CUT_DIR}" -rf
fdo_log_section_end get_cuttlefish_packages

# Download Android CTS https://source.android.com/docs/compatibility/cts/downloads
fdo_log_section_start_collapsed get_android_cts "get_android_cts"
CTS_DIR="/cts"
mkdir "${CTS_DIR}"
ANDROID_CTS_VERSION="${ANDROID_VERSION}_r2"
curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
  -o "${CTS_DIR}/cts.zip" \
  "https://dl.google.com/dl/android/cts/android-cts-${ANDROID_CTS_VERSION}-linux_x86-x86.zip"
unzip -q "${CTS_DIR}/cts.zip" -d "${CTS_DIR}"
rm "${CTS_DIR}/cts.zip"

# Keep only the interesting tests to save space
# shellcheck disable=SC2086 # keep word splitting
ANDROID_CTS_MODULES_KEEP_EXPRESSION=$(printf "%s|" $ANDROID_CTS_MODULES | sed -e 's/|$//g')
find "${CTS_DIR}"/android-cts/testcases/ -mindepth 1 -type d | grep -v -E "$ANDROID_CTS_MODULES_KEEP_EXPRESSION" | xargs rm -rf
mv "${CTS_DIR}"/android-cts "${TOOLS_DIR}"
rm "${CTS_DIR}" -rf
fdo_log_section_end get_android_cts

fdo_log_section_start_collapsed get_build-tools "get_build-tools"
BUILD_TOOLS_DIR="/build-tools"
mkdir "${BUILD_TOOLS_DIR}"
curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
  -o "${BUILD_TOOLS_DIR}/build-tools.zip" \
  "https://dl.google.com/android/repository/build-tools_r${ANDROID_SDK_VERSION}_linux.zip"
unzip "${BUILD_TOOLS_DIR}/build-tools.zip" -d "${BUILD_TOOLS_DIR}"
rm "${BUILD_TOOLS_DIR}/build-tools.zip"
mv "${BUILD_TOOLS_DIR}/android-$ANDROID_VERSION" "${TOOLS_DIR}"/build-tools #rename the directory
rm "${BUILD_TOOLS_DIR}" -rf
fdo_log_section_end get_build-tools "get_build-tools"

: "${ANDROID_TOOLS_TARBALL:?ANDROID_TOOLS_TARBALL is not set}"

time tar -cf - "${TOOLS_DIR}" | xz --best -e -T"${FDO_CI_CONCURRENT:-4}" > "/${ANDROID_TOOLS_TARBALL}"
