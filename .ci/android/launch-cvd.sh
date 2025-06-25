#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e

EXIT_CODE=1

: "${CI_PROJECT_DIR:=/}"

source "${CI_PROJECT_DIR}/.ci/setup-test-env.sh"

: "${RESULTS_DIR:=./results}"

set -u

mkdir -p "${RESULTS_DIR}"

function my_atexit()
{
  # shellcheck disable=SC2317
  cp /cuttlefish/cuttlefish/instances/cvd-1/logs/logcat "${RESULTS_DIR}" || true
  # shellcheck disable=SC2317
  cp /cuttlefish/cuttlefish/instances/cvd-1/kernel.log "${RESULTS_DIR}" || true
  # shellcheck disable=SC2317
  cp /cuttlefish/cuttlefish/instances/cvd-1/logs/launcher.log "${RESULTS_DIR}" || true
  exit $EXIT_CODE
}

# Cuttlefish is an artifact built earlier in the pipeline
tar xf "${CI_PROJECT_DIR}/${CUTTLEFISH_TARBALL}" -C /

export PATH=/cuttlefish/bin:/android-tools/android-cts/jdk/bin/:/android-tools/build-tools:$PATH

[ -e /dev/kvm ] || echo "Warning: /dev/kvm is not available"
trap my_atexit EXIT
trap 'exit 2' HUP INT PIPE TERM

section_start launch_cvd "launch_cvd"
pushd /cuttlefish

VSOCK_BASE=10000 # greater than all the default vsock ports
VSOCK_CID=$((VSOCK_BASE + (CI_JOB_ID & 0xfff)))

ulimit -S -n 1048576
HOME=/cuttlefish launch_cvd \
  -daemon \
  -verbosity=VERBOSE \
  -file_verbosity=VERBOSE \
  -use_overlay=false \
  -enable_bootanimation=false \
  -guest_enforce_security=false \
  -report_anonymous_usage_stats=no \
  -gpu_mode="guest_swiftshader" \
  -hwcomposer="drm" \
  -memory_mb 32768 \
  -blank_sdcard_image_mb 65536 \
  -data_policy=always_create \
  -blank_data_image_mb 65536 \
  -enable_audio=false \
  -enable-sandbox=false \
  -enable_modem_simulator=false \
  -vsock_guest_cid=$VSOCK_CID \
  -cpus="${FDO_CI_CONCURRENT:-4}"

while [ "$(adb shell dumpsys -l | grep SurfaceFlinger)" = "" ] ; do sleep 1; done
adb shell dumpsys SurfaceFlinger | grep GLES

popd
section_end launch_cvd

section_start push_new_apex "push_new_apex"
set -x

mkdir /old_apex
adb wait-for-device root
adb pull /vendor/apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex /old_apex

# Unzip it to get apex_build_info.pb which has all the build parameters normally passed to apexer
unzip /old_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex -d /old_apex

# Extract all the apex pieces that can be reused
deapexer \
  --fsckerofs_path /cuttlefish/bin/fsck.erofs \
  --debugfs_path /cuttlefish/bin/debugfs_static \
  extract \
  /old_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex \
  /old_apex

mkdir -p /new_image/bin/hw
# This binary is built earlier in the pipeline
cp "${CI_PROJECT_DIR}/install/x86_64/vendor/bin/hw/android.hardware.composer.hwc3-service.drm" \
  "/new_image/bin/hw"

cp -r /old_apex/etc /new_image/etc
cp -r /old_apex/lib64 /new_image/lib64

mkdir /new_apex
cp /old_apex/apex_manifest.pb /new_apex/
cp /old_apex/apex_build_info.pb /new_apex/

mkdir -p $PWD/prebuilts/sdk/current/public/
cp /android.jar $PWD/prebuilts/sdk/current/public/

apexer \
  --build_info /new_apex/apex_build_info.pb \
  --apexer_tool_path $PATH \
  --manifest /new_apex/apex_manifest.pb \
  --force \
  --key /cuttlefish/com.android.hardware.pem \
  --pubkey /cuttlefish/com.android.hardware.avbpubkey \
  /new_image \
  /new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex

java \
 -Djava.library.path=/cuttlefish/lib64 \
  -jar /cuttlefish/framework/signapk.jar \
  -a 4096 \
  /cuttlefish/com.android.hardware.x509.pem \
  /cuttlefish/com.android.hardware.pk8 \
  /new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex \
  /new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.signed.apex

mv /new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex \
  /new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex_unsigned

mv /new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.signed.apex \
  /new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex

adb install --force-non-staged /new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex

adb logcat -d | grep -i hwc

set +x

# If this service is missing, cts-tradefed will fail device pretests
while [ "$(adb shell dumpsys -l | grep window)" = "" ] ; do sleep 1; done
echo "window ok"

while [ "$(adb shell dumpsys -l | grep lock_settings)" = "" ] ; do sleep 1; done
echo "lock_settings ok"

while [ "$(adb shell dumpsys -l | grep display)" = "" ] ; do sleep 1; done
echo "display ok"

while [ "$(adb shell dumpsys -l | grep input)" = "" ] ; do sleep 1; done
echo "input ok"

while [ "$(adb shell dumpsys -l | grep logcat)" = "" ] ; do sleep 1; done
echo "logcat ok"

# Look for other missing services
adb shell dumpsys > /dev/null

section_end push_new_apex
