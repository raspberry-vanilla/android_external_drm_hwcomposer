#!/usr/bin/env bash
# shellcheck disable=SC1090,SC1091 # no need to follow references to other shell scripts
# For changes in this file to take effect, bump both:
# DEBIAN_CUTTLEFISH_TAG and UBUNTU_ANDROID_TAG
set -e

function get_repo() {
  local repo_dir="$1"
  local repo_url="$2"
  if [[ -z "${repo_url}" ]]; then
    repo_url="${CI_REPOSITORY_URL}"
  fi

  local ref="$3"
  if [[ -z $ref ]]; then
    if [[ "${CI_PIPELINE_SOURCE}" == "merge_request_event" ]]; then
      ref="${CI_MERGE_REQUEST_REF_PATH}"
    else
      ref="${CI_COMMIT_REF_NAME}"
    fi
  fi

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

source "$(dirname "$0")/../shared.sh"


function my_atexit()
{
  local exit_status=$?
  set +e
  # This directory survives outside of the container, so use it for job artifacts
  mkdir -p "/cache/${CI_PROJECT_PATH}"
  gzip -c "/cuttlefish.log.txt" > "/cache/${CI_PROJECT_PATH}/cuttlefish.log.txt.gz" || true
  cp "/${CUTTLEFISH_TARBALL}" "/cache/${CI_PROJECT_PATH}/${CUTTLEFISH_TARBALL}" || true

  apt-get purge -y "${EPHEMERAL_DEPS[@]}"
  apt-get autoremove -y
  apt-get clean

  # clean up the container to avoid storing > 200GB
  rm --preserve-root "${TOP}" -rf

  # also remove uncompressed CUTTLEFISH_DIR to reduce container size by ~ 3GB
  rm --preserve-root "${CUTTLEFISH_DIR}" -rf
  rm --preserve-root /root/\.* -rf
  rm --preserve-root /tmp/* -rf
  exit $exit_status
}

trap my_atexit EXIT
trap 'exit 2' HUP INT PIPE TERM

# remove logs from previous builds
rm "/cache/${CI_PROJECT_PATH}/cuttlefish.log.txt" || true
rm "/cache/${CI_PROJECT_PATH}/${CUTTLEFISH_TARBALL}" || true

source "${FDO_CI_BASH_HELPERS}"

EPHEMERAL_DEPS=(
  binutils
  bison
  curl
  flex
  git
  glslang-tools
  gpg
  gpg-agent
  libncurses-dev
  meson
  ninja-build
  openssl
  openssh-client
  openssh-server
  patch
  perl
  pkg-config
  pipx
  python3
  python3-mako
  python3-yaml
  rsync
  ssh
  time
  unzip
  vim
  wget
  xz-utils
  zip
  zstd
)

DEPS=(
  ca-certificates
  sudo
)

export DEBIAN_FRONTEND=noninteractive

fdo_log_section_start_collapsed install_packages "install_packages"
apt-get update
apt-get upgrade -y
apt-get install -y --no-install-recommends "${EPHEMERAL_DEPS[@]}"
apt-get install -y --no-install-recommends "${DEPS[@]}"
curl -o /usr/local/bin/repo https://storage.googleapis.com/git-repo-downloads/repo
chmod a+x /usr/local/bin/repo
fdo_log_section_end install_packages

fdo_log_section_start_collapsed repo_init "repo_init"

# avoid accidentally reusing the .repo from previous builds
rm /cache/.repo -rf

TOP="$(pwd)/aosp"
mkdir "${TOP}"
cd "${TOP}"

: "${ANDROID_BRANCH:?ANDROID_BRANCH is not set}"
: "${ANDROID_TARGET_RELEASE:?ANDROID_TARGET_RELEASE is not set}"

# prevent interactive colour diffs question
yes n | repo init \
  -u https://android.googlesource.com/platform/manifest \
  -b "${ANDROID_BRANCH}" \
  --depth=1

 # Don't increase parallel jobs or they will be denied
time safe_repo_sync
fdo_log_section_end repo_init

fdo_log_section_start_collapsed customize_repo "customize_repo"
DRMHWC_DIR="${TOP}/external/drm_hwcomposer"
get_repo "${DRMHWC_DIR}"

# Ensure that __ANDROID_API__ is defined as ANDROID_SDK_VERSION
: "${ANDROID_SDK_VERSION:?ANDROID_SDK_VERSION is not set}"
sed -i "/cc_defaults[[:space:]]*{/a\    min_sdk_version: \"${ANDROID_SDK_VERSION}\"," "${DRMHWC_DIR}/Android.bp"

# Keep mesa's Android.bp files for other devices that reference them, but,
# since the blueprint files don't support building lavapipe and llvmpipe for cuttlefish,
# continue to use Android.mk for these drivers
MESA3D_DIR="${TOP}/external/mesa3d"
pushd "${MESA3D_DIR}"
git remote add upstream https://gitlab.freedesktop.org/mesa/mesa.git
git fetch upstream

# Fetch upstream mesa at the commit which uses the static LLVM library available from AOSP.
# When this commit is included in the next mesa release, then just use the released version.
MESA3D_LLVM_COMMIT="9029c8b1e37"
git restore --source="$MESA3D_LLVM_COMMIT" android/
popd

# Remove references to AOSP's vulkan.lvp to avoid conflicts with Mesa make files
rm "${MESA3D_DIR}/src/gallium/targets/lavapipe/Android.bp"
CUTTLEFISH_DEVICE_DIR="${TOP}/device/google/cuttlefish"
sed -i '/"vulkan\.lvp"/d' "${CUTTLEFISH_DEVICE_DIR}/build/Android.bp"

ALLOW_MESA_DIR="${TOP}/vendor/google/build/androidmk"
mkdir -p "${ALLOW_MESA_DIR}"
echo 'external/mesa3d/android/Android.mk' > "${ALLOW_MESA_DIR}/allowlist.txt"

ALLOW_MESA_PRODUCT="${TOP}/device/google/cuttlefish/vsoc_x86_64/phone/aosp_cf.mk"
cat >> "${ALLOW_MESA_PRODUCT}" <<EOF
PRODUCT_ALLOWED_ANDROIDMK_FILES += external/mesa3d/android/Android.mk
PRODUCT_SOONG_ONLY := false
EOF

cat >> "${CUTTLEFISH_DEVICE_DIR}/shared/virgl/BoardConfig.mk" <<EOF
  BOARD_MESA3D_USES_MESON_BUILD := true
  BOARD_MESA3D_GALLIUM_DRIVERS := llvmpipe
  BOARD_MESA3D_VULKAN_DRIVERS := swrast
EOF

sed -i '$d' "${CUTTLEFISH_DEVICE_DIR}/shared/virgl/device_vendor.mk"
cat >> "${CUTTLEFISH_DEVICE_DIR}/shared/virgl/device_vendor.mk" <<EOF
  PRODUCT_PACKAGES += \\
    libEGL_mesa \\
    libGLESv1_CM_mesa \\
    libGLESv2_mesa \\
    libgallium_dri \\
    vulkan.lvp
EOF

CROSVM_FILE="${CUTTLEFISH_DEVICE_DIR}/host/libs/vm_manager/crosvm_manager.cpp"
sed -i 's/\("androidboot.hardware.egl", \)"angle"/\1"mesa"/' "${CROSVM_FILE}"
sed -i 's/\("androidboot.hardware.vulkan", \)"pastel"/\1"lvp"/' "${CROSVM_FILE}"
sed -i '/"lvp"/a \        {"androidboot.hardware.hwcomposer.mode", "client"},' "${CROSVM_FILE}"

# Don't block vkms
BLOCKLIST="${TOP}/device/google/cuttlefish/shared/modules.blocklist"
sed -i '/vkms/d' "$BLOCKLIST"

fdo_log_section_end customize_repo

fdo_log_section_start_collapsed build_cuttlefish "build_cuttlefish"
source build/envsetup.sh
export TARGET_BUILD_VARIANT=userdebug # needed for adb root and remount
export TARGET_PRODUCT=aosp_cf_x86_64_phone
export TARGET_RELEASE=${ANDROID_TARGET_RELEASE}

# Trusty attempts to mount a fresh /proc to run nsjail,
# but ci-templates uses buildah which already mounts /null on /proc
# so this conflict causes trusty to fail
export RELEASE_AVF_ENABLE_EARLY_VM=false
export TRUSTY_SYSTEM_VM=disabled

lunch "${TARGET_PRODUCT}-${TARGET_RELEASE}-${TARGET_BUILD_VARIANT}"

echo "NOTE: AOSP compilation output is being filtered to echo only errors, failures, and milestone progress lines (every 1000th step)."
echo "This prevents the multi-million line build log from exceeding GitLab CI's job log size limit while keeping progress visible."
echo "The complete, unfiltered build log is saved to /cuttlefish.log.txt and will be uploaded as an artifact on failure."
time make -j"${FDO_CI_CONCURRENT:-4}" 2>&1 | tee "/cuttlefish.log.txt" | \
  awk '/error:/ || /FAILED:/ || /ERROR:/ || /fatal:/ || (/\[/ && ++count % 1000 == 0) { print }'

echo "Build of ${TARGET_PRODUCT}-${TARGET_RELEASE}-${TARGET_BUILD_VARIANT} complete."

fdo_log_section_end build_cuttlefish

fdo_log_section_start_collapsed get_cuttlefish_images "get_cuttlefish_images"

CUTTLEFISH_DIR="/cuttlefish"
mkdir -p "${CUTTLEFISH_DIR}"
cd "${TOP}/out/target/product/vsoc_x86_64"

PHONE_FILES=(
  boot.img
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
tar -xvf "${TOP}/out/host/linux-x86/cvd-host_package.tar.gz" -C "${CUTTLEFISH_DIR}"

BOOTLOADER_DIR="${TOP}/out/soong/.intermediates/device/google/cuttlefish_prebuilts/bootloader"
cp -r "${BOOTLOADER_DIR}/bootloader_crosvm_x86_64/linux_glibc_common/bootloader.crosvm" \
  "${CUTTLEFISH_DIR}/bootloader"

# Get keys and certificates for signing future apex packages
cp "${TOP}/hardware/interfaces/apexkey/com.android.hardware.x509.pem" "${CUTTLEFISH_DIR}"
cp "${TOP}/hardware/interfaces/apexkey/com.android.hardware.pk8" "${CUTTLEFISH_DIR}"
cp "${TOP}/hardware/interfaces/apexkey/com.android.hardware.pem" "${CUTTLEFISH_DIR}"
cp "${TOP}/hardware/interfaces/apexkey/com.android.hardware.avbpubkey" "${CUTTLEFISH_DIR}"

: "${CUTTLEFISH_TARBALL:?CUTTLEFISH_TARBALL is not set}"

tar -cf - "${CUTTLEFISH_DIR}" | xz --best -e -T"${FDO_CI_CONCURRENT:-4}" > "/${CUTTLEFISH_TARBALL}" || true
fdo_log_section_end get_cuttlefish_images
