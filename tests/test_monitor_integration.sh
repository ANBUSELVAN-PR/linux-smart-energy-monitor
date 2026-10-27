#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PROJECT_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd -P)"
APP_PATH="${PROJECT_ROOT}/app/energy_monitor"
TEMP_DIR=""
APP_PID=""

cleanup() {
    local status=$?

    if [[ -n "${APP_PID}" ]] && kill -0 "${APP_PID}" 2>/dev/null; then
        kill -TERM "${APP_PID}" 2>/dev/null || true
        wait "${APP_PID}" 2>/dev/null || true
    fi

    if [[ -n "${TEMP_DIR}" ]]; then
        rm -rf -- "${TEMP_DIR}"
    fi

    exit "${status}"
}

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

trap cleanup EXIT
trap 'exit 130' INT TERM

[[ -x "${APP_PATH}" ]] || fail "application is missing or not executable: ${APP_PATH}"

TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/energy-monitor-integration.XXXXXX")"
LOG_PATH="${TEMP_DIR}/application.log"
CSV_PATH="${TEMP_DIR}/energy_readings.csv"

printf 'RUN: starting application in %s\n' "${TEMP_DIR}"
(
    cd "${TEMP_DIR}"
    exec "${APP_PATH}"
) >"${LOG_PATH}" 2>&1 &
APP_PID=$!

# Detect device-open or CSV-open failures before the planned observation period.
sleep 1
if ! kill -0 "${APP_PID}" 2>/dev/null; then
    set +e
    wait "${APP_PID}"
    app_status=$?
    set -e
    APP_PID=""
    printf '%s\n' '--- application log ---' >&2
    cat "${LOG_PATH}" >&2
    fail "application exited early with status ${app_status}"
fi

# The driver publishes about once per second; collect several event-driven rows.
sleep 4

if ! kill -0 "${APP_PID}" 2>/dev/null; then
    set +e
    wait "${APP_PID}"
    app_status=$?
    set -e
    APP_PID=""
    printf '%s\n' '--- application log ---' >&2
    cat "${LOG_PATH}" >&2
    fail "application exited before SIGINT with status ${app_status}"
fi

printf 'RUN: sending SIGINT to application\n'
kill -INT "${APP_PID}"

# Do not let a broken shutdown path leave a background application behind.
deadline=$((SECONDS + 5))
while kill -0 "${APP_PID}" 2>/dev/null; do
    if (( SECONDS >= deadline )); then
        kill -TERM "${APP_PID}" 2>/dev/null || true
        wait "${APP_PID}" 2>/dev/null || true
        APP_PID=""
        fail "application did not exit within 5 seconds after SIGINT"
    fi
    sleep 0.1
done

set +e
wait "${APP_PID}"
app_status=$?
set -e
APP_PID=""

if (( app_status != 0 )); then
    printf '%s\n' '--- application log ---' >&2
    cat "${LOG_PATH}" >&2
    fail "application exited with status ${app_status} after SIGINT"
fi

grep -Fqx 'Monitoring stopped cleanly.' "${LOG_PATH}" || {
    printf '%s\n' '--- application log ---' >&2
    cat "${LOG_PATH}" >&2
    fail 'clean-shutdown message was not present'
}

[[ -f "${CSV_PATH}" ]] || fail "CSV file was not created: ${CSV_PATH}"

python3 - "${CSV_PATH}" <<'PY'
import csv
import datetime
import decimal
import pathlib
import sys

csv_path = pathlib.Path(sys.argv[1])
expected_header = [
    "timestamp",
    "sample",
    "voltage_v",
    "current_a",
    "power_w",
    "energy_wh",
]

with csv_path.open(newline="", encoding="ascii") as stream:
    rows = list(csv.reader(stream))

if not rows:
    raise SystemExit("FAIL: CSV is empty")
if rows[0] != expected_header:
    raise SystemExit(f"FAIL: unexpected CSV header: {rows[0]!r}")

data_rows = rows[1:]
if len(data_rows) < 3:
    raise SystemExit(f"FAIL: expected at least 3 data rows, found {len(data_rows)}")

previous_sample = None
for row_number, row in enumerate(data_rows, start=2):
    if len(row) != len(expected_header):
        raise SystemExit(f"FAIL: row {row_number} has {len(row)} fields: {row!r}")

    try:
        datetime.datetime.strptime(row[0], "%Y-%m-%d %H:%M:%S")
        sample = int(row[1])
        if sample < 0:
            raise ValueError("negative sample")
        for value in row[2:]:
            decimal.Decimal(value)
    except (ValueError, decimal.InvalidOperation) as error:
        raise SystemExit(f"FAIL: invalid CSV row {row_number}: {row!r} ({error})")

    if previous_sample is not None and sample <= previous_sample:
        raise SystemExit(
            f"FAIL: sample IDs are not strictly increasing at row {row_number}: "
            f"{previous_sample} then {sample}"
        )
    previous_sample = sample

print(f"PASS: validated {len(data_rows)} CSV data rows with increasing sample IDs")
PY

printf 'PASS: application integration test completed\n'
