#!/usr/bin/env bash
# shellcheck disable=SC1091 # no need to follow references to other shell scripts

set -e

# shellcheck disable=SC2034 # exit code is used in the err trap
EXIT_CODE=1
VKMS_DIR="/config/vkms/my-vkms"

if [[ -z ${CI_PROJECT_DIR} ]]; then
    CI_PROJECT_DIR="$(dirname "${0}")/../.."
fi

source "${CI_PROJECT_DIR}/.ci/android/launch-cvd.sh"

function print_vkms_config() {
  _print_connector_type() {
    case "$1" in
      1)  echo "VGA" ;;
      10) echo "DP" ;;
      11) echo "HDMI-A" ;;
      12) echo "HDMI-B" ;;
      14) echo "eDP" ;;
      15) echo "VIRTUAL" ;;
      16) echo "DSI" ;;
      17) echo "DPI" ;;
      18) echo "WRITEBACK" ;;
      *)  echo "UNKNOWN($1)" ;;
    esac
  }
  _print_connector_status() {
    case "$1" in
      1) echo "connected" ;;
      2) echo "disconnected" ;;
      3) echo "unknown" ;;
      *) echo "UNKNOWN($1)" ;;
    esac
  }

  _print_plane_type() {
    case "$1" in
      0) echo "overlay" ;;
      1) echo "primary" ;;
      2) echo "cursor" ;;
      *) echo "UNKNOWN($1)" ;;
    esac
  }

  _to_boolean_string() {
    case "$1" in
      0) echo "false" ;;
      1) echo "true" ;;
      *) echo "UNKNOWN($1)" ;;
    esac
  }

  # shellcheck disable=SC2207 # it's ok to split on spaces since filenames won't have spaces
  local vkms_config_files=($(adb shell "find ${VKMS_DIR} \( -type f -a \( -name type -o -name status -o -name writeback -o -name enabled \) \) 2>/dev/null"))

  local full_path=""
  local base_name="" # (e.g., type or status)
  local numeric_value=""
  local human_readable_value=""

  for full_path in "${vkms_config_files[@]}"; do
    base_name="${full_path##*/}"
    numeric_value="$(adb shell "cat '$full_path' 2>/dev/null | tr -d '\r' | tr -d '[:space:]'")"
    human_readable_value="$numeric_value"

    case "$full_path" in
      */connectors/*)
        case "$base_name" in
          type)
            human_readable_value=$(_print_connector_type "$numeric_value") ;;
          status)
            human_readable_value=$(_print_connector_status "$numeric_value") ;;
        esac
        ;;
      */encoders/*)
        case "$base_name" in
        type)
          human_readable_value="VIRTUAL" ;;
        esac
        ;;
      */crtcs/*)
        case "$base_name" in
          writeback)
            human_readable_value=$(_to_boolean_string "$numeric_value") ;;
        esac
        ;;
      */planes/*)
        case "$base_name" in
          type)
            human_readable_value=$(_print_plane_type "$numeric_value") ;;
          enabled)
            human_readable_value=$(_to_boolean_string "$numeric_value") ;;
        esac
        ;;
      "${VKMS_DIR}/enabled")
          human_readable_value=$(_to_boolean_string "$numeric_value")
        ;;
    esac

    echo "$full_path: $human_readable_value"
  done
}

function stop_hwc() {
  adb shell setprop ctl.stop surfaceflinger
  adb shell setprop ctl.stop vendor.hwcomposer-3
  echo -n "hwcomposer-3: "
  adb shell getprop init.svc.vendor.hwcomposer-3
  echo -n "surfaceflinger: "
  adb shell getprop init.svc.surfaceflinger
}

function start_hwc() {
  adb shell setprop ctl.start vendor.hwcomposer-3
  adb shell setprop ctl.start surfaceflinger
  echo -n "hwcomposer-3: "
  adb shell getprop init.svc.vendor.hwcomposer-3
  echo -n "surfaceflinger: "
  adb shell getprop init.svc.surfaceflinger
}

function reload_vkms() {
  local enable_cursor=${1:-true}
  local enable_overlay=${2:-true}
  local enable_writeback=${3:-true}

  # validate true/false only
  case "$enable_cursor"    in true|false) ;; *) echo "arg1 enable_cursor must be true or false" >&2; exit 1 ;; esac
  case "$enable_overlay"   in true|false) ;; *) echo "arg2 enable_overlay must be true or false" >&2; exit 1 ;; esac
  case "$enable_writeback" in true|false) ;; *) echo "arg3 enable_writeback must be true or false" >&2; exit 1 ;; esac

  adb reboot
  adb wait-for-device devices
  adb root

  stop_hwc
  adb shell /vendor/bin/modprobe -d /vendor/lib/modules -r vkms || true
  adb logcat -b events -d | grep -iE 'rescue|watchdog|fatal' || true

  adb shell /vendor/bin/modprobe -d /vendor/lib/modules vkms \
    enable_cursor="${enable_cursor}" \
    enable_overlay="${enable_overlay}" \
    enable_writeback="${enable_writeback}" \
    create_default_dev=false

  adb shell lsmod | grep vkms
  adb shell ls /dev/dri/
  echo "Reloaded vkms module."

  start_hwc
  adb root

  unset CONNECTOR_TYPES
}

function configure_vkms()
{
  local config_command="--config"
  # local enable_writeback=0 # setup_vkms does this by default
  # local create_cursor_plane=1 # setup_vkms does this by default
  local overlay_planes=1 # create one overlay plane per pipeline

  # VGA=1, DP=10, HDMIA=11, HDMIB=12, eDP=14, VIRTUAL=15, DSI=16, DPI=17, WRITEBACK=18
  local connector_types=("eDP")
  if [ -n "${CONNECTOR_TYPES:-}" ]; then
    read -r -a connector_types <<<"${CONNECTOR_TYPES}"
  fi
  local type
  for type in "${connector_types[@]}"; do
    case "$type" in
      VGA|DP|HDMIA|HDMIB|eDP|VIRTUAL|DSI|DPI|WRITEBACK) ;;
      *)
        echo "connector_types contains invalid value '$type'" >&2
        exit 1
        ;;
    esac
    config_command+=" ${type},${overlay_planes}"
  done
  echo "Running config_command:" "${config_command[@]}"

  adb shell /data/local/tmp/setup_vkms_connectors_for_atest "${config_command}"

  echo -n "vendor.hwc.drm.device is set to: "
  adb shell getprop vendor.hwc.drm.device
  adb shell ls /sys/class/drm/card1
  print_vkms_config
}

function run_vts() {
  if [[ -z ${1-} ]]; then
    echo "run_vts: missing required argument <test_name>"
    return 1
  fi
  local test_name=$1
  local skip_file_name=${2:-vts-skips.txt}
  local results_log="vts_results_${test_name}"
  adb shell stop surfaceflinger
  adb logcat -c

  ADB_TEST_CMD="(/data/local/tmp/VtsHalGraphicsComposer3_TargetTest 2>&1 \
    | tee /data/local/tmp/${results_log})"

  set +e
  local TIMED_OUT=0
  # If drm-hwcomposer crashes, the test may timeout rather than fail
  timeout -k 30s 3m adb shell "${ADB_TEST_CMD}"
  TIMED_OUT=$?
  if [[ "$TIMED_OUT" -ne 0 ]]; then
      adb logcat -d | grep -E "hwc|VtsHalGraphicsComposer3_TargetTest|ActivityManager" > "${RESULTS_DIR}/vts_results_${test_name}.timeout"
      echo "${test_name} timed out."
  fi

  adb pull "/data/local/tmp/${results_log}" "${RESULTS_DIR}"
  [ -s "${RESULTS_DIR}/${results_log}" ]
  MISSING_RESULTS=$?
  if [ "${MISSING_RESULTS}" -ne 0 ]; then
    echo "Missing results."
    return "${MISSING_RESULTS}";
  fi

  mapfile -t expected_skips < <(
    sed -nE '/^(#|[[:space:]]*$)/!{ s/^[[:space:]]+//; s/[[:space:]]+$//; p }' \
      "${CI_PROJECT_DIR}/.ci/android/${skip_file_name}"
  )

  local actual_skips_file="vts_results_${test_name}_actual_skips.txt"
  sed -n '/\[ *SKIPPED *\].*listed below:/,$p' \
    "${RESULTS_DIR}/${results_log}" | sed '1d' > "${RESULTS_DIR}/${actual_skips_file}"

  mapfile -t actual_skips < <(
    sed -nE 's/^\[[[:space:]]*SKIPPED[[:space:]]*\][[:space:]]*(.*)$/\1/p' \
      "${RESULTS_DIR}/${actual_skips_file}"
  )

  UNEXPECTED_SKIPS=0
  if [[ "${expected_skips[*]}" != "${actual_skips[*]}" ]]; then
    UNEXPECTED_SKIPS=1
    echo "Unexpected changes to the skipped tests."
    unexpected_run=$(comm -23 <(printf '%s\n' "${expected_skips[@]}" | sort) \
        <(printf '%s\n' "${actual_skips[@]}"   | sort) || true)
    unexpected_skip=$(comm -13 <(printf '%s\n' "${expected_skips[@]}" | sort) \
        <(printf '%s\n' "${actual_skips[@]}"   | sort) || true)

    if [[ -n $unexpected_run ]]; then
      echo -e "\nTest(s) listed in ${skip_file_name}, were actually run: "
      printf '%s\n' "$unexpected_run"
    fi

    if [[ -n $unexpected_skip ]]; then
      echo -e "\nTest(s) not listed in ${skip_file_name}, were actually skipped:"
      printf '%s\n' "$unexpected_skip"
    fi
  fi

  # shellcheck disable=SC2251 # don't exit on failure
  ! grep -i "FAILED" "${RESULTS_DIR}/${results_log}"
  FAILED_TESTS=$?
  if [ "${FAILED_TESTS}" -ne 0 ]; then
    echo "Failed tests."
  fi

  [ "${TIMED_OUT}" = "0" ] && \
  [ "${FAILED_TESTS}" = "0" ] && \
  [ "${UNEXPECTED_SKIPS}" = "0" ]
  return $?
}

adb wait-for-device devices
adb root
adb push "/${BINARIES_DIR}/VtsHalGraphicsComposer3_TargetTest" /data/local/tmp/
adb push "/${BINARIES_DIR}/setup_vkms_connectors_for_atest" /data/local/tmp/
