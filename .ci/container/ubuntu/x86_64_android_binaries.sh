#!/usr/bin/env bash
# no need to follow references to other shell scripts
# shellcheck disable=SC1090
# shellcheck disable=SC1091
# For changes in this file to take effect, bump both:
# UBUNTU_ANDROID_BINARIES_TAG and UBUNTU_ANDROID_TAG

set -e

function my_atexit()
{
  local exit_status=$?
  set +e
  apt-get purge -y "${DEPS_FOR_AOSP[@]}"
  apt-get autoremove -y
  apt-get clean
  rm --preserve-root "${BINARIES_DIR}" -rf
  rm --preserve-root /root/.cache/ -rf
  rm --preserve-root /root/.npm/ -rf
  rm --preserve-root /tmp/* -rf
  rm --preserve-root /usr/local/bin/repo
  rm --preserve-root "${TOP}" -rf
  exit $exit_status
}

trap my_atexit EXIT
trap 'exit 2' HUP INT PIPE TERM

source "$(dirname "$0")/../shared.sh"

source "${FDO_CI_BASH_HELPERS}"

DEPS=(
  ca-certificates
  curl
  git
  sudo
  wget
  xz-utils
)

DEPS_FOR_AOSP=(
  gpg
  gpg-agent
  python3
  rsync
  ssh
  unzip
)

export DEBIAN_FRONTEND=noninteractive
ln -fs /usr/share/zoneinfo/UTC /etc/localtime # suppress tzdata prompt

fdo_log_section_start_collapsed install_packages "install_packages"
apt-get update
apt-get upgrade -y
apt-get install -y --no-remove --no-install-recommends "${DEPS[@]}"
apt-get install -y --no-remove --no-install-recommends "${DEPS_FOR_AOSP[@]}"

curl -o /usr/local/bin/repo https://storage.googleapis.com/git-repo-downloads/repo
chmod a+x /usr/local/bin/repo

fdo_log_section_end install_packages

fdo_log_section_start_collapsed repo_init "repo_init"
TOP="/aosp"
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

source "${TOP}/build/envsetup.sh"
export TARGET_BUILD_VARIANT=userdebug
export TARGET_PRODUCT=aosp_cf_x86_64_slim
export TARGET_RELEASE=${ANDROID_TARGET_RELEASE}
lunch "${TARGET_PRODUCT}-${TARGET_RELEASE}-${TARGET_BUILD_VARIANT}"

: "${BINARIES_DIR:?BINARIES_DIR is not set}"
mkdir "/${BINARIES_DIR}"

fdo_log_section_start_collapsed build_vts "build_vts"
m VtsHalGraphicsComposer3_TargetTest
cp "${TOP}/out/target/product/vsoc_x86_64_only/data/nativetest64/VtsHalGraphicsComposer3_TargetTest/VtsHalGraphicsComposer3_TargetTest" \
  "/${BINARIES_DIR}/VtsHalGraphicsComposer3_TargetTest"
fdo_log_section_end build_vts

fdo_log_section_start_collapsed build_apexer "build_apexer"
m apexer-host
cp "${TOP}/out/host/linux-x86/bin/apexer" "/${BINARIES_DIR}/apexer"
fdo_log_section_end build_apexer

cp "${TOP}/prebuilts/sdk/current/public/android.jar" "/${BINARIES_DIR}/android.jar"

fdo_log_section_start_collapsed build_vkms_tests "build_vkms_tests"
m setup_vkms_connectors_for_atest;
cp "${TOP}/out/target/product/vsoc_x86_64_only/system/bin/setup_vkms_connectors_for_atest" \
  "/${BINARIES_DIR}/setup_vkms_connectors_for_atest"

m teardown_vkms;
cp "${TOP}/out/target/product/vsoc_x86_64_only/system/bin/teardown_vkms" \
  "/${BINARIES_DIR}/teardown_vkms"

m VkmsTestHwcHotplugs
cp "${TOP}/out/target/product/vsoc_x86_64_only/data/nativetest64/VkmsTestHwcHotplugs/VkmsTestHwcHotplugs" \
  "/${BINARIES_DIR}/test_hotplugs"
fdo_log_section_end build_vkms_tests
