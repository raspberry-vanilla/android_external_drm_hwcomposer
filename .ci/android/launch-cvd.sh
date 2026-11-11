#!/usr/bin/env bash
# shellcheck disable=SC1090 # no need to follow references to other shell scripts

set -e

EXIT_CODE=1

: "${CI_PROJECT_DIR:=/}"
CI_PROJECT_DIR="$(realpath "${CI_PROJECT_DIR}")"

source "${FDO_CI_BASH_HELPERS}"

: "${RESULTS_DIR:=${CI_PROJECT_DIR}/results}"
RESULTS_DIR="$(realpath -m "${RESULTS_DIR}")"
: "${CI_JOB_ID:=$$}"
: "${CUTTLEFISH_TARBALL:=cuttlefish.tar.xz}"
: "${ANDROID_TOOLS_TARBALL:=android_tools.tar.xz}"
: "${TOOLS_DIR:=android-tools}"
: "${BINARIES_DIR:=android-binaries}"

# Place unpacked Cuttlefish/CTS trees and VM disk images on the runner's
# /builds volume (keeping path length short for Cuttlefish UNIX domain sockets)
# rather than /, which lives in Podman's container RW overlay layer (#132).
if [ -z "${CVD_WORK_BASE:-}" ]; then
  BUILDS_MOUNT="$(df --output=target "${CI_PROJECT_DIR}" 2>/dev/null | tail -n 1 || true)"
  if [ -n "${BUILDS_MOUNT}" ] && [ "${BUILDS_MOUNT}" != "/" ] && [ "${#BUILDS_MOUNT}" -le 20 ]; then
    CVD_WORK_BASE="${BUILDS_MOUNT}"
  else
    CVD_WORK_BASE="/tmp"
  fi
fi
export CVD_WORK_DIR="${CVD_WORK_BASE}/.c${CI_JOB_ID}"
export CVD_DIR="${CVD_WORK_DIR}/cuttlefish"
export TOOLS_ROOT="${CVD_WORK_DIR}/${TOOLS_DIR}"
export CVD_TMP_DIR="${CVD_WORK_DIR}/tmp"

set -u

mkdir -p "${RESULTS_DIR}"

function my_atexit()
{
  # shellcheck disable=SC2317
  local status=$?
  # shellcheck disable=SC2317
  set +e
  # shellcheck disable=SC2317
  if [ "$status" -eq 124 ]; then
    EXIT_CODE=124
  fi
  # shellcheck disable=SC2317
  cp "${CVD_DIR}/cuttlefish/instances/cvd-1/logs/logcat" "${RESULTS_DIR}" 2>/dev/null || true
  # shellcheck disable=SC2317
  cp "${CVD_DIR}/cuttlefish/instances/cvd-1/kernel.log" "${RESULTS_DIR}" 2>/dev/null || true
  # shellcheck disable=SC2317
  cp "${CVD_DIR}/cuttlefish/instances/cvd-1/logs/launcher.log" "${RESULTS_DIR}" 2>/dev/null || true
  # Stop daemonized Cuttlefish processes before unlinking CVD_WORK_DIR so open
  # image file descriptors release allocated blocks immediately.
  # shellcheck disable=SC2317
  if [ -n "${CVD_DIR:-}" ] && [ -x "${CVD_DIR}/bin/stop_cvd" ]; then
    HOME="${CVD_DIR}" timeout -k 5s 20s "${CVD_DIR}/bin/stop_cvd" >/dev/null 2>&1 || true
  fi
  # shellcheck disable=SC2317
  if [ -n "${CVD_WORK_DIR:-}" ]; then
    rm -rf --preserve-root "${CVD_WORK_DIR}" || true
  fi
  # shellcheck disable=SC2317
  exit "$EXIT_CODE"
}

# safe_adb [-t <timeout_seconds>] [-d <delay_seconds>] <adb_args...>
function safe_adb() {
  local timeout=30
  local delay=1

  # Parse optional timeout/delay flags
  while [[ $# -gt 0 ]]; do
    case "$1" in
      -t) timeout="$2"; shift 2 ;;
      -d) delay="$2"; shift 2 ;;
      --) shift; break ;;
      *) break ;;
    esac
  done

  local start_time
  start_time=$(date +%s)

  while true; do
    if [ "${1:-}" = "root" ]; then
      local root_out
      root_out=$(adb root 2>&1)
      if [ $? -eq 0 ] || [[ "$root_out" =~ "already running as root" ]]; then
        adb wait-for-device
        return 0
      fi
    else
      if adb "$@"; then
        return 0
      fi
    fi

    local current_time
    current_time=$(date +%s)
    if (( current_time - start_time >= timeout )); then
      echo "ERROR: 'adb $*' failed after ${timeout}s" >&2
      return 1
    fi

    echo "WARNING: 'adb $*' failed, retrying in ${delay}s (elapsed: $((current_time - start_time))s / ${timeout}s)..." >&2
    sleep "$delay"
    adb wait-for-device || true
  done
}

export -f safe_adb

# check_expected_assumption_failures <expected_file> <actual_file>
function check_expected_assumption_failures() {
  local expected_file="$1"
  local actual_file="$2"
  local expected_file_name="${expected_file##*/}"

  mapfile -t expected_failures < <(
    if [ -f "${expected_file}" ]; then
      sed -nE '/^(#|[[:space:]]*$)/!{ s/^[[:space:]]+//; s/[[:space:]]+$//; p }' \
        "${expected_file}" | sort
    fi
  )

  mapfile -t actual_failures < <(
    if [ -f "${actual_file}" ]; then
      sed -nE '/^(#|[[:space:]]*$)/!{ s/^[[:space:]]+//; s/[[:space:]]+$//; p }' \
        "${actual_file}" | sort
    fi
  )

  if [[ "${expected_failures[*]:-}" != "${actual_failures[*]:-}" ]]; then
    echo "Unexpected changes to the assumption-failed tests."
    local unexpected_run unexpected_assumption_failure
    unexpected_run=$(comm -23 \
        <((( ${#expected_failures[@]} )) && printf '%s\n' "${expected_failures[@]}") \
        <((( ${#actual_failures[@]} )) && printf '%s\n' "${actual_failures[@]}") || true)
    unexpected_assumption_failure=$(comm -13 \
        <((( ${#expected_failures[@]} )) && printf '%s\n' "${expected_failures[@]}") \
        <((( ${#actual_failures[@]} )) && printf '%s\n' "${actual_failures[@]}") || true)

    if [[ -n $unexpected_run ]]; then
      echo -e "\nTest(s) listed in ${expected_file_name}, were actually run: "
      printf '%s\n' "$unexpected_run"
    fi

    if [[ -n $unexpected_assumption_failure ]]; then
      echo -e "\nTest(s) not listed in ${expected_file_name}, had assumption failures:"
      printf '%s\n' "$unexpected_assumption_failure"
    fi
    return 1
  fi

  return 0
}

export -f check_expected_assumption_failures

trap my_atexit EXIT
trap 'exit 2' HUP INT PIPE TERM

find "${CVD_WORK_BASE}" -maxdepth 1 -name '.c[0-9]*' -mmin +120 -exec rm -rf {} + 2>/dev/null || true
rm -rf --preserve-root "${CVD_WORK_DIR}"
mkdir -p "${CVD_WORK_DIR}" "${CVD_TMP_DIR}"
export TMPDIR="${CVD_TMP_DIR}"

fdo_log_section_start_collapsed get_cuttlefish "get_cuttlefish"
tar xf "/${CUTTLEFISH_TARBALL}" -C "${CVD_WORK_DIR}"
fdo_log_section_end get_cuttlefish

fdo_log_section_start_collapsed get_tools "get_tools"
if [ "${NEED_ANDROID_CTS:-0}" = "1" ]; then
  tar xf "/${ANDROID_TOOLS_TARBALL}" -C "${CVD_WORK_DIR}" \
    --exclude="${TOOLS_DIR}/cuttlefish-*.deb"
else
  # Non-CTS VTS/VKMS jobs only need build-tools and JDK to repack and sign the
  # test APEX; skip decompressing the ~650 MB CTS testcases payload.
  tar xf "/${ANDROID_TOOLS_TARBALL}" -C "${CVD_WORK_DIR}" \
    "${TOOLS_DIR}/build-tools" \
    "${TOOLS_DIR}/android-cts/jdk"
fi
cp "/${BINARIES_DIR}/apexer" "${TOOLS_ROOT}/build-tools/"
fdo_log_section_end get_tools

export PATH="${CVD_DIR}/bin:${TOOLS_ROOT}/android-cts/jdk/bin:${TOOLS_ROOT}/build-tools:${PATH}"

[ -e /dev/kvm ] || echo "Warning: /dev/kvm is not available"

fdo_log_section_start_collapsed launch_cvd "launch_cvd"
pushd "${CVD_DIR}"

VSOCK_BASE=10000 # greater than all the default vsock ports
VSOCK_CID=$((VSOCK_BASE + (CI_JOB_ID & 0xfff)))

ulimit -S unlimited
# Cap blank userdata (8 GiB) and sdcard (2 GiB) images instead of 64 GiB each;
# userdata size also bounds Cuttlefish's composite disk qcow2 overlay.img,
# shrinking apparent per-VM image size from ~193 GB to ~21 GB. Disable unused
# Wi-Fi (OpenWRT VM + ap_overlay.img) and modem simulator daemons.
HOME="${CVD_DIR}" timeout -k 30s 15m launch_cvd \
  -daemon \
  -verbosity=VERBOSE \
  -file_verbosity=VERBOSE \
  -enable_bootanimation=false \
  -guest_enforce_security=false \
  -report_anonymous_usage_stats=no \
  -gpu_mode="guest_swiftshader" \
  -hwcomposer="drm" \
  -memory_mb 8192 \
  -blank_sdcard_image_mb 2048 \
  -data_policy=always_create \
  -blank_data_image_mb 8192 \
  -enable_audio=false \
  -enable_wifi=false \
  -enable_modem_simulator=false \
  -enable-sandbox=false \
  -vsock_guest_cid=$VSOCK_CID \
  -cpus="${FDO_CI_CONCURRENT:-4}" \
  -extra_bootconfig_args="androidboot.vendor.apex.com.android.hardware.graphics.composer=com.android.hardware.graphics.composer.drm_hwcomposer"

# Wait for SurfaceFlinger, PackageManagerService, and sys.boot_completed=1 so
# apexd is ready to accept --force-non-staged APEX installs without racing
# package_native startup.
timeout -k 10s 5m bash -c '
  while [ "$(adb shell dumpsys -l 2>/dev/null | grep SurfaceFlinger)" = "" ]; do sleep 1; done
  while [ "$(adb shell dumpsys -l 2>/dev/null | grep -E "^  package[[:space:]]*$")" = "" ]; do sleep 1; done
  until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d "\r")" = "1" ]; do sleep 1; done
'
adb shell dumpsys SurfaceFlinger | grep GLES

adb logcat -d | grep -i vkms
echo "Running ro.build.version.sdk: $(adb shell getprop ro.build.version.sdk)"
echo "Running ro.build.version.release: $(adb shell getprop ro.build.version.release)"
popd
fdo_log_section_end launch_cvd

fdo_log_section_start_collapsed push_new_apex "push_new_apex"
pushd "${CVD_WORK_DIR}"

mkdir -p "${CVD_WORK_DIR}/old_apex"
safe_adb root
safe_adb pull /vendor/apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex "${CVD_WORK_DIR}/old_apex"

# Unzip it to get apex_build_info.pb which has all the build parameters normally passed to apexer
unzip "${CVD_WORK_DIR}/old_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex" -d "${CVD_WORK_DIR}/old_apex"

# Extract all the apex pieces that can be reused
deapexer \
  --fsckerofs_path "${CVD_DIR}/bin/fsck.erofs" \
  --debugfs_path "${CVD_DIR}/bin/debugfs_static" \
  extract \
  "${CVD_WORK_DIR}/old_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex" \
  "${CVD_WORK_DIR}/old_apex"

# Dynamically locate the HWC3 service binary built earlier in the pipeline
# (installed under bin/hw/ or vendor/bin/hw/ depending on container setup)
HWC3_BINARY="$(find "${CI_PROJECT_DIR}/install/x86_64" -name "android.hardware.composer.hwc3-service.drm" | head -n 1)"
mkdir -p "${CVD_WORK_DIR}/new_image/bin/hw"
cp "${HWC3_BINARY}" "${CVD_WORK_DIR}/new_image/bin/hw"

cp -r "${CVD_WORK_DIR}/old_apex/etc" "${CVD_WORK_DIR}/new_image/etc"
cp -r "${CVD_WORK_DIR}/old_apex/lib64" "${CVD_WORK_DIR}/new_image/lib64"

# Overlay updated VINTF manifest and shared libraries from build artifacts
find "${CVD_WORK_DIR}/new_image/etc" -name "hwc3-drm.xml" -exec cp "${CI_PROJECT_DIR}/hwc3/hwc3-drm.xml" {} \;
find "${CI_PROJECT_DIR}/install/x86_64" -name "*.so" -exec cp {} "${CVD_WORK_DIR}/new_image/lib64/" \; 2>/dev/null || true

mkdir -p "${CVD_WORK_DIR}/new_apex"
cp "${CVD_WORK_DIR}/old_apex/apex_manifest.pb" "${CVD_WORK_DIR}/new_apex/"
cp "${CVD_WORK_DIR}/old_apex/apex_build_info.pb" "${CVD_WORK_DIR}/new_apex/"

mkdir -p "${CVD_WORK_DIR}/prebuilts/sdk/current/public"
cp "/${BINARIES_DIR}/android.jar" "${CVD_WORK_DIR}/prebuilts/sdk/current/public/"

apexer \
  --build_info "${CVD_WORK_DIR}/new_apex/apex_build_info.pb" \
  --apexer_tool_path "${PATH}" \
  --manifest "${CVD_WORK_DIR}/new_apex/apex_manifest.pb" \
  --force \
  --key "${CVD_DIR}/com.android.hardware.pem" \
  --pubkey "${CVD_DIR}/com.android.hardware.avbpubkey" \
  "${CVD_WORK_DIR}/new_image" \
  "${CVD_WORK_DIR}/new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex"

java \
  -Djava.library.path="${CVD_DIR}/lib64" \
  -jar "${CVD_DIR}/framework/signapk.jar" \
  -a 4096 --align-file-size \
  "${CVD_DIR}/com.android.hardware.x509.pem" \
  "${CVD_DIR}/com.android.hardware.pk8" \
  "${CVD_WORK_DIR}/new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex" \
  "${CVD_WORK_DIR}/new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.signed.apex"

mv "${CVD_WORK_DIR}/new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex" \
  "${CVD_WORK_DIR}/new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex_unsigned"

mv "${CVD_WORK_DIR}/new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.signed.apex" \
  "${CVD_WORK_DIR}/new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex"

safe_adb -t 90 install --force-non-staged "${CVD_WORK_DIR}/new_apex/com.android.hardware.graphics.composer.drm_hwcomposer.apex"
popd

# Reboot and wait for drmhwc to start up again with new apex. Bound
# wait-for-device with a 3-minute timeout so a wedged Cuttlefish vsock/adbd
# fails fast and triggers automatic job retry instead of stalling for 60m.
adb logcat -c
adb reboot
sleep 2
timeout -k 10s 3m adb wait-for-device
adb devices
fdo_log_section_end push_new_apex

# Define helper to wait for critical services with a timeout
function wait_for_services() {
  # If this service is missing, cts-tradefed will fail device pretests
  while [ "$(adb shell dumpsys -l 2>/dev/null | grep window)" = "" ] ; do sleep 1; done
  echo "window ok"

  while [ "$(adb shell dumpsys -l 2>/dev/null | grep lock_settings)" = "" ] ; do sleep 1; done
  echo "lock_settings ok"

  while [ "$(adb shell dumpsys -l 2>/dev/null | grep display)" = "" ] ; do sleep 1; done
  echo "display ok"

  while [ "$(adb shell dumpsys -l 2>/dev/null | grep input)" = "" ] ; do sleep 1; done
  echo "input ok"

  while [ "$(adb shell dumpsys -l 2>/dev/null | grep logcat)" = "" ] ; do sleep 1; done
  echo "logcat ok"

  # package manager and boot completion are needed before CTS/VTS can install APKs
  while [ "$(adb shell dumpsys -l 2>/dev/null | grep -E '^  package[[:space:]]*$')" = "" ]; do sleep 1; done
  echo "package ok"

  until [ "$(adb shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]; do sleep 1; done
  echo "boot_completed ok"
}

export -f wait_for_services
timeout -k 30s 15m bash -c wait_for_services

# Look for other missing services
adb shell dumpsys > /dev/null
