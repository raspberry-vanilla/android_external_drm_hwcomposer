#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts
# Bump the DEBIAN_CUTTLEFISH_TAG for changes in this file to take effect.
set -e

function get_repo() {
  local repo_dir="$1"
  local repo_url="$2"
  local ref="$3"

  echo "Fetching ${repo_url} at ref ${ref} into ${repo_dir}"
  rm --preserve-root "${repo_dir}" -rf
  mkdir -p "${repo_dir}"
  pushd "${repo_dir}"
  git init
  git remote add origin "${repo_url}"
  git fetch --depth=1 origin "${ref}"
  git checkout FETCH_HEAD
  popd
}


function my_atexit()
{
  # This directory survives outside of the container, so use it for job artifacts
  mkdir -p "/cache/${CI_PROJECT_PATH}"
  gzip -c "/cuttlefish.log.txt" > "/cache/${CI_PROJECT_PATH}/cuttlefish.log.txt.gz" || true
  cp "/${CUTTLEFISH_TARBALL}" "/cache/${CI_PROJECT_PATH}/${CUTTLEFISH_TARBALL}" || true

  apt remove -y "${EPHEMERAL_DEPS[@]}"

  # clean up the container to avoid storing > 200GB
  rm --preserve-root "${TOP}" -rf

  # also remove uncompressed CUTTLEFISH_DIR to reduce container size by ~ 3GB
  rm --preserve-root "${CUTTLEFISH_DIR}" -rf
}

trap my_atexit EXIT
trap 'exit 2' HUP INT PIPE TERM

# remove logs from previous builds
rm "/cache/${CI_PROJECT_PATH}/cuttlefish.log.txt" || true
rm "/cache/${CI_PROJECT_PATH}/${CUTTLEFISH_TARBALL}" || true

source "./.ci/setup-test-env.sh"

EPHEMERAL_DEPS=(
  binutils
  bison
  flex
  glslang-tools
  libncurses5
  ninja-build
  pkg-config
  pipx
  python3
  python3-mako
  python3-yaml
  rsync
  ssh
  time
  unzip
  wget
  xz-utils
  zip
  zstd
)

DEPS=(
  ca-certificates
  curl
  git
  gpg
  gpg-agent
  sudo
  vim
)

export DEBIAN_FRONTEND=noninteractive

section_start install_packages "install_packages"
set -x
apt-get update
apt-get upgrade -y
apt-get install -y --no-install-recommends "${EPHEMERAL_DEPS[@]}"
apt-get install -y --no-install-recommends "${DEPS[@]}"
curl -o /usr/local/bin/repo https://storage.googleapis.com/git-repo-downloads/repo
chmod a+x /usr/local/bin/repo
set +x
section_end install_packages

section_start repo_init "repo_init"

# avoid accidentally reusing the .repo from previous builds
rm /cache/.repo -rf

TOP="$(pwd)/aosp"
mkdir "${TOP}"
cd "${TOP}"

: "${ANDROID_BRANCH:?ANDROID_BRANCH is not set}"

# prevent interactive colour diffs question
yes n | repo init \
  -u https://android.googlesource.com/platform/manifest \
  -b "${ANDROID_BRANCH}" \
  --depth=1

 # Don't increase parallel jobs or they will be denied
time repo sync --fail-fast --no-tags -j4
section_end repo_init

section_start customize_repo "customize_repo"

MESA3D_DIR="${TOP}/external/mesa3d"
MESA3D_URL="https://gitlab.freedesktop.org/mesa/mesa.git"
MESA3D_REF=mesa-25.1.2
get_repo "${MESA3D_DIR}" "${MESA3D_URL}" "${MESA3D_REF}"

LLVM_PROJECT_DIR="${TOP}/external/llvm-project"
LLVM_PROJECT_URL="https://github.com/maurossi/llvm-project"
LLVM_PROJECT_REF=release_18.x
get_repo "${LLVM_PROJECT_DIR}" "${LLVM_PROJECT_URL}" "${LLVM_PROJECT_REF}"

LIBDISPLAY_DIR="${TOP}/external/libdisplay_info"
LIBDISPLAY_URL="https://android.googlesource.com/platform/external/libdisplay-info"
LIBDISPLAY_REF=sdk-release
get_repo "${LIBDISPLAY_DIR}" "${LIBDISPLAY_URL}" "${LIBDISPLAY_REF}"

DRMHWC_DIR="${TOP}/external/drm_hwcomposer"
DRMHWC_URL="https://gitlab.freedesktop.org/drm-hwcomposer/drm-hwcomposer.git"
DRMHWC_REF=main
get_repo "${DRMHWC_DIR}" "${DRMHWC_URL}" "${DRMHWC_REF}"

# Ensure that __ANDROID_API__ is defined as ANDROID_SDK_VERSION
sed -i "/cc_defaults[[:space:]]*{/a\    min_sdk_version: \"${ANDROID_SDK_VERSION}\"," "${DRMHWC_DIR}/Android.bp"

# Build tools are restricted to approved locations in aosp
# https://android.googlesource.com/platform/build/+/main/Changes.md#PATH_Tools
# Don't use TEMPORARY_DISABLE_PATH_RESTRICTIONS=true as it is no longer available
pipx install meson
mv "${HOME}/.local/bin/meson" /usr/bin

CROSVM_FILE="${TOP}/device/google/cuttlefish/host/libs/vm_manager/crosvm_manager.cpp"
sed -i 's/\("androidboot.hardware.egl", \)"angle"/\1"mesa"/' "${CROSVM_FILE}"
sed -i 's/\("androidboot.hardware.vulkan", \)"pastel"/\1"lvp"/' "${CROSVM_FILE}"
sed -i '/"lvp"/a \        {"androidboot.hardware.hwcomposer.mode", "client"},' "${CROSVM_FILE}"

cat >> "${TOP}/device/google/cuttlefish/shared/config/init.vendor.rc" <<EOF
on early-init
   setprop ro.gfx.angle.supported false
   setprop mesa.libgl.always.software true
   setprop mesa.android.no.kms.swrast true
   setprop debug.hwui.renderer opengl
   setprop debug.sf.disable_hwc_vds 1
EOF

cat >> "${TOP}/device/google/cuttlefish/shared/virgl/BoardConfig.mk" <<EOF
BOARD_MESA3D_USES_MESON_BUILD := true
BOARD_MESA3D_GALLIUM_DRIVERS := llvmpipe
BOARD_MESA3D_VULKAN_DRIVERS := swrast
BUILD_BROKEN_PLUGIN_VALIDATION := soong-llvm18
EOF

sed -i '$d' "${TOP}/device/google/cuttlefish/shared/virgl/device_vendor.mk"
cat >>"${TOP}/device/google/cuttlefish/shared/virgl/device_vendor.mk" <<EOF
PRODUCT_PACKAGES += \\
  libEGL_mesa \\
  libGLESv1_CM_mesa \\
  libGLESv2_mesa \\
  libgallium_dri \\
  libglapi \\
  vulkan.lvp
EOF

# Build drm_hwcomposer apex package
sed -i 's/ranchu/drm_hwcomposer/' \
  "${TOP}/device/google/cuttlefish/shared/graphics/device_vendor.mk"

section_end customize_repo

section_start build_cuttlefish "build_cuttlefish"
source build/envsetup.sh
export TARGET_BUILD_VARIANT=userdebug # needed for adb root and remount
export TARGET_PRODUCT=aosp_cf_x86_64_phone
export TARGET_RELEASE=trunk_staging
lunch "${TARGET_PRODUCT}-${TARGET_RELEASE}-${TARGET_BUILD_VARIANT}"

time make -j"${FDO_CI_CONCURRENT:-4}" > "/cuttlefish.log.txt" 2>&1 # Silent or job logs will exceed limit
echo "Build of ${TARGET_PRODUCT}-${TARGET_RELEASE}-${TARGET_BUILD_VARIANT} complete."

section_end build_cuttlefish

section_start get_cuttlefish_images "get_cuttlefish_images"
set -x

CUTTLEFISH_DIR="/cuttlefish"
mkdir -p "${CUTTLEFISH_DIR}"
cd "${TOP}/out/target/product/vsoc_x86_64"

PHONE_FILES=(
  boot.img
  bootloader
  fastboot-info.txt
  init_boot.img
  android-info.txt
  vendor_boot.img
  userdata.img
  vbmeta_system_dlkm.img
  vbmeta_system.img
  vbmeta_vendor_dlkm.img
  vbmeta.img
  super.img
);

for file in "${PHONE_FILES[@]}"; do cp -v "$file" "${CUTTLEFISH_DIR}/"; done;
cp -r  "${TOP}/out/host/linux-x86/cvd-host_package/." "${CUTTLEFISH_DIR}"

# Get keys and certificates for signing future apex packages
cp "${TOP}/hardware/interfaces/apexkey/com.android.hardware.x509.pem" "${CUTTLEFISH_DIR}"
cp "${TOP}/hardware/interfaces/apexkey/com.android.hardware.pk8" "${CUTTLEFISH_DIR}"
cp "${TOP}/hardware/interfaces/apexkey/com.android.hardware.pem" "${CUTTLEFISH_DIR}"
cp "${TOP}/hardware/interfaces/apexkey/com.android.hardware.avbpubkey" "${CUTTLEFISH_DIR}"

: "${CUTTLEFISH_TARBALL:?CUTTLEFISH_TARBALL is not set}"

tar -cf - "${CUTTLEFISH_DIR}" | xz --best -e -T"${FDO_CI_CONCURRENT:-4}" > "/${CUTTLEFISH_TARBALL}" || true
section_end get_cuttlefish_images
