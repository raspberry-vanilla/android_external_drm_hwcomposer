#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts
# Bump the UBUNTU_AOSPLESS_TAG for changes in this file to take effect.

set -e

source "$(dirname "$0")/../shared.sh"

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
  zip
)

DEPS_FOR_TIDY=(
  clang-tidy-19
  libgmock-dev
  libgtest-dev
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

# Make Clang 19 the default
ln -sf /usr/bin/clang-19 /usr/bin/clang
ln -sf /usr/bin/clang++-19 /usr/bin/clang++
ln -sf /usr/bin/clang-tidy-19 /usr/bin/clang-tidy
ln -sf /usr/bin/clang-format-19 /usr/bin/clang-format

curl -o /usr/local/bin/repo https://storage.googleapis.com/git-repo-downloads/repo
chmod a+x /usr/local/bin/repo
fdo_log_section_end install_packages

fdo_log_section_start_collapsed repo_init "repo_init"
TOP="$(pwd)/aosp"
mkdir "${TOP}"
cd "${TOP}"

: "${ANDROID_BRANCH:?ANDROID_BRANCH is not set}"
: "${ANDROID_TARGET_RELEASE:?ANDROID_TARGET_RELEASE is not set}"

yes n | repo init \
  -u https://android.googlesource.com/platform/manifest \
  -b "${ANDROID_BRANCH}" \
  --depth=1
time safe_repo_sync
fdo_log_section_end repo_init

fdo_log_section_start_collapsed customize_repo "customize_repo"
DRMHWC_DIR="${TOP}/external/drm_hwcomposer"

rm "${DRMHWC_DIR}" -rf

git clone "${CI_REPOSITORY_URL}" "${DRMHWC_DIR}"
if [[ "${CI_PIPELINE_SOURCE}" == "merge_request_event" ]]; then
  git -C "${DRMHWC_DIR}" fetch origin "${CI_MERGE_REQUEST_REF_PATH}"
else
  git -C "${DRMHWC_DIR}" fetch origin "${CI_COMMIT_REF_NAME}"
fi
git -C "${DRMHWC_DIR}" checkout FETCH_HEAD

git clone https://github.com/GloDroid/aospext.git
# drm_hwcomposer dropped HWC2 support, so we don't build hwcomposer.drm.so anymore.
# Patch aospext to not expect it.
sed -i '/hwcomposer.drm.so/d' aospext/meson_drmhwcomposer.mk

cat >> "${TOP}/device/google/cuttlefish/shared/device.mk" <<EOF
BOARD_BUILD_AOSPEXT_DRMHWCOMPOSER := true
BOARD_DRMHWCOMPOSER_SRC_DIR := external/drm_hwcomposer
EOF

ALLOW_MK_x86_64="${TOP}/device/google/cuttlefish/vsoc_x86_64_only/slim/aosp_cf.mk"
cat >> "${ALLOW_MK_x86_64}" <<EOF
PRODUCT_ALLOWED_ANDROIDMK_FILES += aospext/Android.mk aospext/**/Android.mk
PRODUCT_SOONG_ONLY := false
EOF

ALLOW_MK_arm64="${TOP}/device/google/cuttlefish/vsoc_arm64_only/slim/aosp_cf.mk"
cat >> "${ALLOW_MK_arm64}" <<EOF
PRODUCT_ALLOWED_ANDROIDMK_FILES += aospext/Android.mk aospext/**/Android.mk
PRODUCT_SOONG_ONLY := false
EOF

fdo_log_section_end customize_repo

fdo_log_section_start_collapsed build_aospless_x86_64 "build_aospless_x86_64"
source build/envsetup.sh
cd "${TOP}/aospext"
export TARGET_BUILD_VARIANT=userdebug # needed for adb root and remount
export TARGET_PRODUCT=aosp_cf_x86_64_slim
export TARGET_RELEASE=${ANDROID_TARGET_RELEASE}

# Disable LLVM Link-Time-Optimization so that the aospless artifacts will
# have full object files for linking rather than raw bitcode
export DISABLE_LTO=true

lunch "${TARGET_PRODUCT}-${TARGET_RELEASE}-${TARGET_BUILD_VARIANT}"

mm
cd "${TOP}/out/target/product/vsoc_x86_64_only/obj/AOSPEXT/DRMHWCOMPOSER/"
make gen_aospless
tar --no-same-owner -xf aospless.tar.gz
# Rename and move the artifacts needed for subsequent jobs to the root directory
cp -r "${TOP}/out/target/product/vsoc_x86_64_only/obj/AOSPEXT/DRMHWCOMPOSER/aospless" \
  "/aospless_x86_64"
fdo_log_section_end build_aospless_x86_64


fdo_log_section_start_collapsed build_aospless_arm64 "build_aospless_arm64"
cd "${TOP}/aospext"
export TARGET_BUILD_VARIANT=userdebug # needed for adb root and remount
export TARGET_PRODUCT=aosp_cf_arm64_slim
export TARGET_RELEASE=${ANDROID_TARGET_RELEASE}
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
