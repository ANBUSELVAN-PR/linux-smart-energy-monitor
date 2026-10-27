#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"

run_build_step() {
    local description=$1
    shift

    printf 'BUILD: %s\n' "${description}"
    if "$@"; then
        printf 'PASS: %s\n' "${description}"
    else
        local status=$?
        printf 'FAIL: %s (exit status %d)\n' "${description}" "${status}" >&2
        exit "${status}"
    fi
}

run_build_step \
    "kernel module" \
    make -C "${PROJECT_ROOT}/kernel"

run_build_step \
    "C++ monitoring application" \
    g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
    "${PROJECT_ROOT}/app/energy_monitor.cpp" \
    -o "${PROJECT_ROOT}/app/energy_monitor"

printf 'PASS: all builds completed\n'
