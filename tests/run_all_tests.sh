#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
DEVICE_PATH="/dev/energy_monitor"

run_test() {
    local name=$1
    shift

    printf "\n========== %s ==========\n" "${name}"

    if "$@"; then
        printf "PASS: %s\n" "${name}"
    else
        local status=$?

        if (( status == 77 )); then
            printf "SKIP: %s (exit status %d)\n" "${name}" "${status}" >&2
        else
            printf "FAIL: %s (exit status %d)\n" "${name}" "${status}" >&2
            exit "${status}"
        fi
    fi
}

if [[ ! -e "${DEVICE_PATH}" ]]; then
    printf "FAIL: %s does not exist; load the energy_monitor module before running tests.\n" \
        "${DEVICE_PATH}" >&2
    exit 1
fi

if [[ ! -r "${DEVICE_PATH}" ]]; then
    printf "FAIL: %s is not readable by the current user. " \
        "${DEVICE_PATH}" >&2
    printf "Use a new login session with the energy-monitor group active, then retry.\n" >&2
    exit 1
fi

run_test "build test" bash "${PROJECT_ROOT}/tests/test_build.sh"

run_test "device poll test" python3 "${PROJECT_ROOT}/tests/test_device_poll.py"

run_test "monitor integration test" bash "${PROJECT_ROOT}/tests/test_monitor_integration.sh"

run_test "monitor CSV error test" bash "${PROJECT_ROOT}/tests/test_monitor_csv_error.sh"

run_test "monitor input validation test" bash "${PROJECT_ROOT}/tests/test_monitor_input_validation.sh"

printf "\nPASS: all tests completed\n"
