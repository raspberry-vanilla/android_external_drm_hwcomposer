#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts
# Bump the UBUNTU_ANDROID_TAG for changes in this file to take effect.

set -e

source "./.ci/setup-test-env.sh"

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
  rsync
  ssh
  unzip
)

export DEBIAN_FRONTEND=noninteractive
ln -fs /usr/share/zoneinfo/UTC /etc/localtime # suppress tzdata prompt

section_start install_packages "install_packages"
apt-get update
apt-get upgrade -y
apt-get install -y --no-remove --no-install-recommends "${DEPS[@]}"
apt-get install -y --no-remove --no-install-recommends "${DEPS_FOR_AOSP[@]}"

curl -o /usr/local/bin/repo https://storage.googleapis.com/git-repo-downloads/repo
chmod a+x /usr/local/bin/repo
section_end install_packages

# Build and install Debian package for cuttlefish
section_start get_cuttlefish_packages "get_cuttlefish_packages"
set -x
ANDROID_CUTTLEFISH_VERSION=v1.5.0
mkdir /android-cuttlefish
pushd /android-cuttlefish
git init
git remote add origin https://github.com/google/android-cuttlefish.git
git fetch --depth 1 origin "$ANDROID_CUTTLEFISH_VERSION"
git checkout FETCH_HEAD
/android-cuttlefish/tools/buildutils/build_packages.sh
apt-get install -y --allow-downgrades ./cuttlefish-base_*.deb ./cuttlefish-user_*.deb
popd
rm -rf /android-cuttlefish
set +x
section_end get_cuttlefish_packages

# Download Android CTS
section_start get_android_cts "get_android_cts"
set -x
ANDROID_CTS_VERSION="${ANDROID_VERSION}_r3"
mkdir /android-tools
cd /android-tools
curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
  -o "android-cts-${ANDROID_CTS_VERSION}-linux_x86-${ANDROID_CTS_DEVICE_ARCH}.zip" \
  "https://dl.google.com/dl/android/cts/android-cts-${ANDROID_CTS_VERSION}-linux_x86-${ANDROID_CTS_DEVICE_ARCH}.zip"
unzip -q ./*.zip
rm ./*.zip

# Keep only the interesting tests to save space
# shellcheck disable=SC2086 # keep word splitting
ANDROID_CTS_MODULES_KEEP_EXPRESSION=$(printf "%s|" $ANDROID_CTS_MODULES | sed -e 's/|$//g')
find android-cts/testcases/ -mindepth 1 -type d | grep -v -E "$ANDROID_CTS_MODULES_KEEP_EXPRESSION" | xargs rm -rf
set +x
section_end get_android_cts

section_start get_build-tools "get_build-tools"
set -x
curl -L --retry 4 -f --retry-all-errors --retry-delay 60 \
  -o "build-tools_r${ANDROID_SDK_VERSION}-linux.zip" \
  "https://dl.google.com/android/repository/build-tools_r${ANDROID_SDK_VERSION}_linux.zip"
unzip "build-tools_r${ANDROID_SDK_VERSION}-linux.zip"
rm ./*.zip
mv "android-$ANDROID_VERSION" build-tools #rename the directory
set +x
section_end get_build-tools "get_build-tools"

section_start repo_init "repo_init"
TOP="/aosp"
mkdir "${TOP}"
cd "${TOP}"

: "${ANDROID_BRANCH:?ANDROID_BRANCH is not set}"

# prevent interactive colour diffs question
yes n | repo init \
  -u https://android.googlesource.com/platform/manifest \
  -b "${ANDROID_BRANCH}" \
  --depth=1

 # Don't increase parallel jobs or they will be denied
time repo sync --fail-fast --no-tags -j2
section_end repo_init

section_start build_vts "build_vts"
source "${TOP}/build/envsetup.sh"
export TARGET_BUILD_VARIANT=userdebug
export TARGET_PRODUCT=aosp_cf_x86_64_slim
export TARGET_RELEASE=trunk_staging
lunch "${TARGET_PRODUCT}-${TARGET_RELEASE}-${TARGET_BUILD_VARIANT}"
time m VtsHalGraphicsComposer3_TargetTest > "/vts.log.txt" 2>&1
cp "${TOP}/out/target/product/vsoc_x86_64_only/data/nativetest64/VtsHalGraphicsComposer3_TargetTest/VtsHalGraphicsComposer3_TargetTest" \
  "/VtsHalGraphicsComposer3_TargetTest"
set +x
section_end build_vts

section_start build_apexer "build_apexer"
m apexer-host
cp "${TOP}/out/host/linux-x86/bin/apexer" "/android-tools/build-tools/apexer"
section_end build_apexer

cp "${TOP}/prebuilts/sdk/current/public/android.jar" "/android.jar"

# clean up
rm "/root/.cache" -rf
rm "${TOP}" -rf
