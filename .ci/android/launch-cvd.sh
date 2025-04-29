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
cp -r "/cuttlefish/cvd-host_package/." /cuttlefish

export PATH=/cuttlefish/bin:/android-tools/android-cts/jdk/bin/:/android-tools/build-tools:$PATH

[ -e /dev/kvm ] || echo "Warning: /dev/kvm is not available"
trap my_atexit EXIT
trap 'exit 2' HUP INT PIPE TERM

section_start launch_cvd "launch_cvd"
cd /cuttlefish

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

section_end launch_cvd

section_start push_drm_hwc "push_drm_hwc"
set -x

adb wait-for-device root
adb remount /vendor
adb reboot
adb wait-for-device root
adb remount /vendor

# stop ranchu that comes with the cuttlefish image
adb shell stop vendor.hwcomposer-3
adb shell umount /apex/com.android.hardware.graphics.composer.ranchu
adb shell umount /apex/com.android.hardware.graphics.composer.ranchu@1
adb shell umount /bootstrap-apex/com.android.hardware.graphics.composer.ranchu@1
adb shell umount /bootstrap-apex/com.android.hardware.graphics.composer.ranchu

# These artifacts are built earlier in the pipeline
adb push "${CI_PROJECT_DIR}/install/x86_64/vendor/bin/hw/android.hardware.composer.hwc3-service.drm" \
  "/vendor/bin/hw/android.hardware.composer.hwc3-service.drm"
adb push "${CI_PROJECT_DIR}/install/x86_64/vendor/etc/init/hwc3-drm.rc" \
  "/vendor/etc/init/hwc3-drm.rc"
adb push "${CI_PROJECT_DIR}/install/x86_64/vendor/etc/vintf/manifest/hwc3-drm.xml" \
  "/vendor/etc/vintf/manifest/hwc3-drm.xml"
adb push "${CI_PROJECT_DIR}/install/x86_64/vendor/lib64/hw/hwcomposer.drm.so" \
  "/vendor/lib64/hw/hwcomposer.drm.so"

# Start drmhwc
adb shell LD_LIBRARY_PATH=/system/lib64:/apex/com.android.hardware.graphics.composer@1/lib64 \
  /vendor/bin/hw/android.hardware.composer.hwc3-service.drm &
adb logcat -d | grep -i hwc

set +x

# If these service is missing, cts-tradefed will fail device pretests
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

section_end push_drm_hwc
