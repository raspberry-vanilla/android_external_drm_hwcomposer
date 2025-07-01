#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts
# Bump the UBUNTU_AOSPLESS_TAG for changes in this file to take effect.

set -e

source "${FDO_CI_BASH_HELPERS}"

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

fdo_log_section_start_collapsed install_packages "install_packages"
apt-get update
apt-get upgrade -y
apt-get install -y --no-remove --no-install-recommends "${DEPS[@]}"
apt-get install -y --no-remove --no-install-recommends "${DEPS_FOR_AOSP[@]}"
apt-get install -y --no-remove --no-install-recommends "${DEPS_FOR_BUILD[@]}"
apt-get install -y --no-remove --no-install-recommends "${DEPS_FOR_TIDY[@]}"
apt-get install -y --no-remove --no-install-recommends "${DEPS_FOR_CHECK[@]}"

curl -o /usr/local/bin/repo https://storage.googleapis.com/git-repo-downloads/repo
chmod a+x /usr/local/bin/repo
fdo_log_section_end install_packages

fdo_log_section_start_collapsed repo_init "repo_init"
TOP="$(pwd)/aosp"
mkdir "${TOP}"
cd "${TOP}"

: "${ANDROID_BRANCH:?ANDROID_BRANCH is not set}"

yes n | repo init \
  -u https://android.googlesource.com/platform/manifest \
  -b "${ANDROID_BRANCH}" \
  --depth=1
time repo sync --fail-fast --no-tags -j2

rm external/drm_hwcomposer -rf

git clone "${CI_REPOSITORY_URL}" external/drm_hwcomposer
if [[ "${CI_PIPELINE_SOURCE}" == "merge_request_event" ]]; then
  git -C external/drm_hwcomposer fetch origin "${CI_MERGE_REQUEST_REF_PATH}"
else
  git -C external/drm_hwcomposer fetch origin "${CI_COMMIT_REF_NAME}"
fi
git -C external/drm_hwcomposer checkout FETCH_HEAD

sed -i "/'-DUSE_IMAPPER4_METADATA_API'/a\    '-D__ANDROID_API__=${ANDROID_SDK_VERSION}'," external/drm_hwcomposer/meson.build

rm external/libdisplay_info -rf
git clone --depth=1 https://android.googlesource.com/platform/external/libdisplay-info/ external/libdisplay_info

git clone https://github.com/GloDroid/aospext.git

cat >> "${TOP}/device/google/cuttlefish/shared/device.mk" <<EOF
BOARD_BUILD_AOSPEXT_DRMHWCOMPOSER := true
BOARD_DRMHWCOMPOSER_SRC_DIR := external/drm_hwcomposer
EOF
source build/envsetup.sh
fdo_log_section_end repo_init

fdo_log_section_start_collapsed build_aospless_x86_64 "build_aospless_x86_64"
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
fdo_log_section_end build_aospless_x86_64


fdo_log_section_start_collapsed build_aospless_arm64 "build_aospless_arm64"
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
fdo_log_section_end build_aospless_arm64

# clean up
rm "${TOP}" -rf
