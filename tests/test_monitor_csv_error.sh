#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
APP_PATH="${PROJECT_ROOT}/app/energy_monitor"
DEVICE_PATH="/dev/energy_monitor"
TEMP_DIR=""

cleanup() {
    local status=$?

    if [[ -n "${TEMP_DIR}" ]]; then
        rm -rf -- "${TEMP_DIR}"
    fi

    exit "${status}"
}

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

skip() {
    printf 'SKIP: %s\n' "$*" >&2
    exit 77
}

trap cleanup EXIT

[[ -x "${APP_PATH}" ]] || fail "application is missing or not executable: ${APP_PATH}"
[[ -e "${DEVICE_PATH}" ]] || skip "device is unavailable: ${DEVICE_PATH}"
[[ -r "${DEVICE_PATH}" ]] || skip \
    "device is not readable; use a session with the energy-monitor group active"
[[ -e /dev/full ]] || skip "/dev/full is unavailable"
command -v timeout >/dev/null || skip "timeout command is unavailable"

TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/energy-monitor-csv-error.XXXXXX")"
LOG_PATH="${TEMP_DIR}/application.log"
ln -s /dev/full "${TEMP_DIR}/energy_readings.csv"

printf 'RUN: exercising CSV failure through %s/energy_readings.csv\n' "${TEMP_DIR}"
set +e
(
    cd "${TEMP_DIR}"
    exec timeout --foreground --signal=TERM --kill-after=2s 10s "${APP_PATH}"
) >"${LOG_PATH}" 2>&1
app_status=$?
set -e

if (( app_status == 124 || app_status == 137 )); then
    printf '%s\n' '--- application log ---' >&2
    cat "${LOG_PATH}" >&2
    fail "application timed out instead of reporting a CSV failure"
fi

if (( app_status == 0 )); then
    printf '%s\n' '--- application log ---' >&2
    cat "${LOG_PATH}" >&2
    fail "application exited successfully despite /dev/full CSV target"
fi

if ! grep -Fq 'CSV write failed for energy_readings.csv:' "${LOG_PATH}"; then
    printf '%s\n' '--- application log ---' >&2
    cat "${LOG_PATH}" >&2
    fail "application did not report the expected CSV-related error"
fi

printf 'PASS: CSV write/flush failure was reported (application status %d)\n' \
    "${app_status}"
