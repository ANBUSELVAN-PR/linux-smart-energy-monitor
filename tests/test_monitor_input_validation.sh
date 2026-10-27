#!/usr/bin/env bash

# Automated input-validation tests for the C++ energy monitoring application.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
APP_PATH="${PROJECT_ROOT}/app/energy_monitor"
SHIM_SRC="${SCRIPT_DIR}/faults/energy_monitor_preload.c"

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/energy-monitor-validation.XXXXXX")"
SHIM_SO="${TMP_ROOT}/libenergy_monitor_preload.so"
FAILED=0

cleanup() {
    if [[ "$FAILED" -eq 0 ]]; then
        rm -rf "$TMP_ROOT"
    else
        echo "DEBUG: test files preserved at ${TMP_ROOT}" >&2
    fi
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    FAILED=1
}

if [[ ! -x "$APP_PATH" ]]; then
    echo "FAIL: application missing or not executable: $APP_PATH" >&2
    exit 1
fi

if ! gcc -Wall -Wextra -Werror -shared -fPIC \
    -o "$SHIM_SO" "$SHIM_SRC"; then
    echo "FAIL: shim compilation failed" >&2
    exit 1
fi

run_scenario() {
    local scenario="$1"
    local workdir="${TMP_ROOT}/${scenario}"
    local app_status
    local expected_status

    mkdir -p "$workdir"

    echo
    echo "========== ${scenario} input =========="

    if (
        cd "$workdir"
        export ENERGY_MONITOR_FAKE_DATA="$scenario"
        export LD_PRELOAD="$SHIM_SO"

        if timeout --foreground --signal=TERM --kill-after=2s 5s \
            "$APP_PATH" >app.log 2>&1; then
            exit 0
        else
            exit "$?"
        fi
    ); then
        app_status=0
    else
        app_status=$?
    fi

    cat "$workdir/app.log"

    case "$scenario" in
        valid)
            expected_status=0

            if [[ "$app_status" -ne "$expected_status" ]]; then
                fail "valid input exited with status ${app_status}, expected 0"
            elif ! grep -Fq "Voltage: 230.000 V" "$workdir/app.log"; then
                fail "valid input did not display the measurement"
            elif ! grep -Fq "Monitoring stopped cleanly." "$workdir/app.log"; then
                fail "valid input did not stop cleanly"
            elif [[ ! -f "$workdir/energy_readings.csv" ]]; then
                fail "valid input did not create the CSV file"
            elif [[ "$(head -n 1 "$workdir/energy_readings.csv")" != \
                "timestamp,sample,voltage_v,current_a,power_w,energy_wh" ]]; then
                fail "CSV header is incorrect"
            elif [[ "$(wc -l < "$workdir/energy_readings.csv")" -ne 2 ]]; then
                fail "expected one CSV data row plus the header"
            elif ! grep -Eq \
                '^[^,]+,1,230\.000,5\.000,1150\.000,100\.000$' \
                "$workdir/energy_readings.csv"; then
                fail "CSV data row does not match the valid measurement"
            else
                echo "PASS: valid input displayed, logged, and shut down correctly"
            fi
            ;;

        incomplete|malformed|trailing)
            expected_status=1

            if [[ "$app_status" -ne "$expected_status" ]]; then
                fail "${scenario} input exited with status ${app_status}, expected 1"
            elif ! grep -Fq "Unexpected driver data:" "$workdir/app.log"; then
                fail "${scenario} input was not rejected with the expected error"
            elif grep -Fq "Voltage:" "$workdir/app.log"; then
                fail "${scenario} input was incorrectly accepted as a measurement"
            else
                echo "PASS: ${scenario} input rejected correctly"
            fi
            ;;
    esac
}

for scenario in valid incomplete malformed trailing; do
    run_scenario "$scenario"
done

echo
echo "========== Summary =========="

if [[ "$FAILED" -ne 0 ]]; then
    echo "FAIL: input-validation tests failed"
    exit 1
fi

echo "PASS: all four input-validation scenarios"
exit 0
