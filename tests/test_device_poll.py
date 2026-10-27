#!/usr/bin/env python3
"""Integration test for /dev/energy_monitor poll and read semantics."""

import os
import re
import select
import sys
import time


DEVICE_PATH = "/dev/energy_monitor"
READ_SIZE = 256
INITIAL_TIMEOUT_MS = 3000
POST_READ_TIMEOUT_MS = 200
NEXT_SAMPLE_TIMEOUT_MS = 3000

RECORD_PATTERN = re.compile(
    r"sample=(?P<sample>\d+),voltage=\d+,current=\d+\.\d{3},"
    r"power=\d+\.\d{3},energy=\d+\.\d{3}\n?"
)

READ_EVENTS = select.POLLIN | select.POLLRDNORM
ERROR_EVENTS = select.POLLERR | select.POLLHUP | select.POLLNVAL


class TestFailure(Exception):
    """A device behavior assertion failed."""


def read_sample(fd):
    """Read one complete current record and return its sample identifier."""
    try:
        # simple_read_from_buffer advances this per-file offset after every read.
        os.lseek(fd, 0, os.SEEK_SET)
        record = os.read(fd, READ_SIZE)
    except OSError as error:
        raise TestFailure(f"read or seek failed: {error}") from error

    if not record:
        raise TestFailure("read returned EOF instead of a measurement record")

    try:
        text = record.decode("ascii")
    except UnicodeDecodeError as error:
        raise TestFailure(f"record is not ASCII: {record!r}") from error

    match = RECORD_PATTERN.fullmatch(text)
    if not match:
        raise TestFailure(f"malformed measurement record: {text!r}")

    return int(match.group("sample")), text.rstrip()


def wait_for_readable(poller, timeout_ms, description):
    """Wait for exactly the driver's readable event and reject error events."""
    events = poller.poll(timeout_ms)
    if not events:
        raise TestFailure(f"timed out after {timeout_ms} ms waiting for {description}")

    if len(events) != 1:
        raise TestFailure(f"unexpected poll result count for {description}: {events!r}")

    _, revents = events[0]
    if revents & ERROR_EVENTS:
        raise TestFailure(
            f"unexpected device error event for {description}: revents=0x{revents:x}"
        )

    unexpected_events = revents & ~(READ_EVENTS | ERROR_EVENTS)
    if unexpected_events:
        raise TestFailure(
            f"unexpected poll flags for {description}: revents=0x{revents:x}"
        )

    if not (revents & READ_EVENTS):
        raise TestFailure(
            f"unexpected non-readable poll event for {description}: revents=0x{revents:x}"
        )


def main():
    fd = None
    status = 0

    try:
        try:
            fd = os.open(DEVICE_PATH, os.O_RDONLY | os.O_CLOEXEC)
        except FileNotFoundError as error:
            raise TestFailure(f"device is missing: {DEVICE_PATH}") from error
        except PermissionError as error:
            raise TestFailure(f"permission denied opening {DEVICE_PATH}: {error}") from error
        except OSError as error:
            raise TestFailure(f"failed to open {DEVICE_PATH}: {error}") from error

        poller = select.poll()
        poller.register(fd, READ_EVENTS)

        # A fresh open has one unread current sample and must become readable.
        initial_wait_started = time.monotonic()
        wait_for_readable(poller, INITIAL_TIMEOUT_MS, "the initial sample")
        initial_wait_seconds = time.monotonic() - initial_wait_started
        first_sample, first_record = read_sample(fd)

        # A consumed sample must not remain ready unless a newer update raced in.
        immediate_events = poller.poll(POST_READ_TIMEOUT_MS)
        if immediate_events:
            if len(immediate_events) != 1:
                raise TestFailure(
                    f"unexpected immediate poll result count: {immediate_events!r}"
                )

            _, revents = immediate_events[0]
            if revents & ERROR_EVENTS:
                raise TestFailure(f"device error after consuming sample: revents=0x{revents:x}")
            unexpected_events = revents & ~(READ_EVENTS | ERROR_EVENTS)
            if unexpected_events:
                raise TestFailure(
                    f"unexpected poll flags after consuming sample: revents=0x{revents:x}"
                )
            if not (revents & READ_EVENTS):
                raise TestFailure(
                    f"unexpected event after consuming sample: revents=0x{revents:x}"
                )

            second_sample, second_record = read_sample(fd)
            # Readiness immediately after consumption is valid only for a new sample.
            if second_sample <= first_sample:
                raise TestFailure(
                    "consumed sample remained falsely ready: "
                    f"first={first_sample}, immediate={second_sample}"
                )
        else:
            # No immediate event is expected; wait for the next delayed-work update.
            wait_for_readable(poller, NEXT_SAMPLE_TIMEOUT_MS, "a later sample")
            second_sample, second_record = read_sample(fd)

        # The later publication must advance the simulated driver's sample counter.
        if second_sample <= first_sample:
            raise TestFailure(
                f"sample did not advance: first={first_sample}, second={second_sample}"
            )

        print("PASS: poll/read semantics verified")
        print(f"  initial readiness delay: {initial_wait_seconds:.3f} s")
        print(f"  first:  {first_record}")
        print(f"  second: {second_record}")
    except TestFailure as error:
        print(f"FAIL: {error}", file=sys.stderr)
        status = 1
    except KeyboardInterrupt:
        print("FAIL: interrupted", file=sys.stderr)
        status = 1
    finally:
        if fd is not None:
            try:
                os.close(fd)
            except OSError as error:
                print(f"FAIL: failed to close {DEVICE_PATH}: {error}", file=sys.stderr)
                status = 1

    return status


if __name__ == "__main__":
    sys.exit(main())
