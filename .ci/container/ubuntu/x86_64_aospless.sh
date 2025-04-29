#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts
# Bump the UBUNTU_AOSPLESS_TAG for changes in this file to take effect.

set -e

source "./.ci/setup-test-env.sh"

DEPS=(
  ca-certificates
  git
  wget
  xz-utils
)

DEPS_FOR_AOSP=(
  curl
  gpg
  gpg-agent
  ssh
)

DEPS_FOR_BUILD=(
  clang
  clang-19
  git
  lld
  llvm
  make
  meson
  pkg-config
  rsync
)

DEPS_FOR_TIDY=(
  clang-tidy-19
)

DEPS_FOR_CHECK=(
  blueprint-tools
  clang-format-19
)

export DEBIAN_FRONTEND=noninteractive

section_start install_packages "install_packages"
set -x
apt-get update
apt-get upgrade -y
apt-get install -y --no-remove --no-install-recommends "${DEPS[@]}"
apt-get install -y --no-remove --no-install-recommends "${DEPS_FOR_AOSP[@]}"
apt-get install -y --no-remove --no-install-recommends "${DEPS_FOR_BUILD[@]}"
apt-get install -y --no-remove --no-install-recommends "${DEPS_FOR_TIDY[@]}"
apt-get install -y --no-remove --no-install-recommends "${DEPS_FOR_CHECK[@]}"

curl -o /usr/local/bin/repo https://storage.googleapis.com/git-repo-downloads/repo
chmod a+x /usr/local/bin/repo
set +x
section_end install_packages

section_start repo_init "repo_init"
set -x
TOP="$(pwd)/aosp" # $CI_PROJECT_DIR is unavailable in FDO_DISTRIBUTION_EXEC
mkdir "${TOP}"
cd "${TOP}"

: "${ANDROID_BRANCH:?ANDROID_BRANCH is not set}"

yes n | repo init \
  -u https://android.googlesource.com/platform/manifest \
  -b "${ANDROID_BRANCH}" \
  --depth=1
time repo sync --fail-fast --no-tags -j2

rm external/drm_hwcomposer -rf
git clone --depth=1 --branch "${CI_COMMIT_REF_NAME}" "${CI_PROJECT_URL}" external/drm_hwcomposer
git -C external/drm_hwcomposer checkout "${CI_COMMIT_SHA}"

rm external/libdisplay_info -rf
git clone --depth=1 https://android.googlesource.com/platform/external/libdisplay-info/ external/libdisplay_info

git clone https://github.com/GloDroid/aospext.git
cat >> "${TOP}/device/google/cuttlefish/shared/device.mk" <<EOF
BOARD_BUILD_AOSPEXT_DRMHWCOMPOSER := true
BOARD_DRMHWCOMPOSER_SRC_DIR := external/drm_hwcomposer
EOF
set +x
source build/envsetup.sh
section_end repo_init

section_start build_aospless_x86_64 "build_aospless_x86_64"
set -x
cd "${TOP}/aospext"
export TARGET_BUILD_VARIANT=userdebug # needed for adb root and remount
export TARGET_PRODUCT=aosp_cf_x86_64_slim
export TARGET_RELEASE=trunk_staging
lunch "${TARGET_PRODUCT}-${TARGET_RELEASE}-${TARGET_BUILD_VARIANT}"
mm
cd "${TOP}/out/target/product/vsoc_x86_64_only/obj/AOSPEXT/DRMHWCOMPOSER/"
make gen_aospless
tar --no-same-owner -xf aospless.tar.gz
# Rename and move the artifacts needed for subsequent jobs to the root directory
cp -r "./aospless" "/aospless_x86_64"
set +x
section_end build_aospless_x86_64


section_start build_aospless_arm64 "build_aospless_arm64"
set -x
cd "${TOP}/aospext"
export TARGET_BUILD_VARIANT=userdebug # needed for adb root and remount
export TARGET_PRODUCT=aosp_cf_arm64_slim
export TARGET_RELEASE=trunk_staging
lunch "${TARGET_PRODUCT}-${TARGET_RELEASE}-${TARGET_BUILD_VARIANT}"
mm
cd "${TOP}/out/target/product/vsoc_arm64_only/obj/AOSPEXT/DRMHWCOMPOSER/"
make gen_aospless
tar --no-same-owner -xf aospless.tar.gz
# Rename and move the artifacts needed for subsequent jobs to the root directory
cp -r "./aospless" "/aospless_arm64"
set +x
section_end build_aospless_arm64

# clean up
rm "${TOP}" -rf
