# Linux-Based Smart Energy Monitoring System

A Linux systems-programming project combining a Linux kernel character-device driver with a C++17 user-space monitoring application.

**Important:** This project currently uses simulated readings. It does not measure electricity from physical sensors.

## Features

- Linux kernel character-device driver.
- Simulated voltage, current, power, and energy readings.
- C++17 monitoring application.
- Device polling using `poll()`.
- Kernel synchronization using mutexes and wait queues.
- Periodic updates using a kernel workqueue.
- Timestamped CSV logging.
- Input validation and error handling.
- Graceful shutdown using Ctrl+C.
- Automated build, integration, polling, and fault-handling tests.

## Architecture

<img width="1312" height="1199" alt="image" src="https://github.com/user-attachments/assets/4986687e-9a0f-4150-bcb6-aa88ac9a6164" />


## Requirements

- Linux operating system.
- Kernel headers matching the running kernel.
- GCC/G++, GNU Make, and Python 3.
- Permission to build and load a kernel module.

On Linux Mint or Ubuntu, install the build tools with:

```bash
sudo apt update
sudo apt install build-essential linux-headers-$(uname -r) python3
```

## Build

From the project root:

```bash
make -C kernel

g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
    app/energy_monitor.cpp -o app/energy_monitor
```

## Load the kernel module

If the module is not already loaded:

```bash
sudo insmod kernel/energy_monitor.ko
```

Check the device:

```bash
ls -l /dev/energy_monitor
```

Do not load the module a second time if it is already running.

## Run the application

Run from the project root:

```bash
./app/energy_monitor
```

Press **Ctrl+C** to stop.

Readings are saved to `energy_readings.csv` in the current working directory.

The device must be readable by the current user. Use appropriate device-group permissions; do not make the device world-writable.

## Run the automated tests

```bash
./tests/run_all_tests.sh
```

The test suite covers:

1. Kernel-module and C++ application builds.
2. Device polling and read behavior.
3. Application integration and CSV logging.
4. CSV write-failure handling.
5. Valid, incomplete, malformed, and trailing-garbage records.

Some tests require the kernel module to be loaded and `/dev/energy_monitor` to be readable. Input-validation tests use an `LD_PRELOAD` shim to supply deterministic fake records.

## Project structure

```text
smart-energy-monitor/
├── app/
│   └── energy_monitor.cpp
├── kernel/
│   ├── energy_monitor.c
│   └── Makefile
├── tests/
│   ├── faults/
│   │   └── energy_monitor_preload.c
│   ├── run_all_tests.sh
│   ├── test_build.sh
│   ├── test_device_poll.py
│   ├── test_monitor_csv_error.sh
│   ├── test_monitor_input_validation.sh
│   └── test_monitor_integration.sh
├── docs/
├── .gitignore
└── README.md
```

## Limitations and future improvements

- Readings are simulated; no physical sensors are connected.
- Physical sensor integration and calibration remain future work.
- Hardware-specific validation is needed before real electrical monitoring.

## License

No license has been selected yet. Choose an appropriate license before redistributing the project.
