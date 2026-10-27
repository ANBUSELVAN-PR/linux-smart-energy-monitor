#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/types.h>
#include <thread>
#include <time.h>
#include <unistd.h>

constexpr const char* DEVICE_PATH = "/dev/energy_monitor";
constexpr const char* CSV_PATH = "energy_readings.csv";
constexpr std::size_t BUFFER_SIZE = 256;
constexpr int POLL_TIMEOUT_MS = 5000;

static volatile sig_atomic_t stop_requested = 0;

struct Measurement {
    unsigned int sample;
    double voltage_v;
    double current_a;
    double power_w;
    double energy_wh;
};

static void handle_signal(int)
{
    stop_requested = 1;
}

static bool reset_read_offset(int fd)
{
    while (lseek(fd, 0, SEEK_SET) == static_cast<off_t>(-1)) {
        if (errno == EINTR && stop_requested)
            return false;

        if (errno == EINTR)
            continue;

        std::cerr << "Failed to reset device read offset: "
                  << std::strerror(errno) << '\n';
        return false;
    }

    return true;
}

static bool read_measurement(int fd, Measurement& measurement)
{
    char buffer[BUFFER_SIZE] = {};
    ssize_t bytes_read;

    if (!reset_read_offset(fd))
        return false;

    do {
        bytes_read = read(fd, buffer, sizeof(buffer) - 1);
    } while (bytes_read == -1 && errno == EINTR && !stop_requested);

    if (bytes_read < 0) {
        if (errno == EINTR && stop_requested)
            return false;

        std::cerr << "Read failed: " << std::strerror(errno) << '\n';
        return false;
    }

    if (bytes_read == 0) {
        std::cerr << "No data available from the driver.\n";
        return false;
    }

    buffer[bytes_read] = '\0';

    // Require exactly one complete record terminated by a newline.
    char *newline = std::strchr(buffer, '\n');
    if (newline == nullptr || newline[1] != '\0') {
        std::cerr << "Unexpected driver data: " << buffer << '\n';
        return false;
    }
    *newline = '\0';

    int consumed = 0;
    if (std::sscanf(buffer,
                    "sample=%u,voltage=%lf,current=%lf,power=%lf,energy=%lf%n",
                    &measurement.sample,
                    &measurement.voltage_v,
                    &measurement.current_a,
                    &measurement.power_w,
                    &measurement.energy_wh,
                    &consumed) != 5 ||
        consumed <= 0 || buffer[consumed] != '\0') {
        std::cerr << "Unexpected driver data: " << buffer << '\n';
        return false;
    }

    return true;
}

enum class PollResult {
    ready,
    timeout,
    stopped,
    error,
};

static PollResult wait_for_measurement(int fd)
{
    struct pollfd poll_fd = {};
    int result;

    poll_fd.fd = fd;
    poll_fd.events = POLLIN | POLLRDNORM;

    do {
        result = poll(&poll_fd, 1, POLL_TIMEOUT_MS);
    } while (result == -1 && errno == EINTR && !stop_requested);

    if (result == -1) {
        if (errno == EINTR && stop_requested)
            return PollResult::stopped;

        std::cerr << "poll() failed: " << std::strerror(errno) << '\n';
        return PollResult::error;
    }

    if (result == 0)
        return PollResult::timeout;

    if ((poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        std::cerr << "Device poll error; revents=" << poll_fd.revents << '\n';
        return PollResult::error;
    }

    if ((poll_fd.revents & (POLLIN | POLLRDNORM)) != 0)
        return PollResult::ready;

    std::cerr << "Unexpected device poll result; revents=" << poll_fd.revents << '\n';
    return PollResult::error;
}

static bool current_timestamp(std::string& timestamp)
{
    const std::time_t now = std::time(nullptr);
    struct tm local_time = {};

    if (now == static_cast<std::time_t>(-1) ||
        localtime_r(&now, &local_time) == nullptr) {
        std::cerr << "Failed to obtain the local timestamp.\n";
        return false;
    }

    char formatted_timestamp[32] = {};
    if (std::strftime(formatted_timestamp,
                      sizeof(formatted_timestamp),
                      "%Y-%m-%d %H:%M:%S",
                      &local_time) == 0) {
        std::cerr << "Failed to format the local timestamp.\n";
        return false;
    }

    timestamp = formatted_timestamp;
    return true;
}

int main()
{
    std::signal(SIGINT, handle_signal);

    const int fd = open(DEVICE_PATH, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        std::cerr << "Failed to open " << DEVICE_PATH
                  << ": " << std::strerror(errno) << '\n';
        return 1;
    }

    int exit_code = 0;
    bool have_last_sample = false;
    unsigned int last_sample = 0;
    std::ofstream csv;

    try {
        csv.exceptions(std::ios::failbit | std::ios::badbit);
        csv.open(CSV_PATH, std::ios::out | std::ios::app);

        if (csv.tellp() == std::streampos(0))
            csv << "timestamp,sample,voltage_v,current_a,power_w,energy_wh\n";

        std::cout << "Smart Energy Monitoring System (simulated readings)\n"
                  << "Logging to " << CSV_PATH << "; press Ctrl+C to stop.\n\n";

        while (!stop_requested) {
            const PollResult poll_result = wait_for_measurement(fd);
            if (poll_result == PollResult::stopped)
                break;

            if (poll_result == PollResult::timeout) {
                std::cerr << "Timed out waiting for a new measurement.\n";
                continue;
            }

            if (poll_result == PollResult::error) {
                exit_code = 1;
                break;
            }

            Measurement measurement = {};
            if (!read_measurement(fd, measurement)) {
                if (!stop_requested)
                    exit_code = 1;
                break;
            }

            if (have_last_sample && measurement.sample == last_sample) {
                /* Supports an older driver that reports constant readiness. */
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            last_sample = measurement.sample;
            have_last_sample = true;

            std::string timestamp;
            if (!current_timestamp(timestamp)) {
                exit_code = 1;
                break;
            }

            std::cout << '[' << timestamp << "] "
                      << "Voltage: " << std::fixed << std::setprecision(3)
                      << measurement.voltage_v << " V | Current: "
                      << measurement.current_a << " A | Power: "
                      << measurement.power_w << " W | Energy: "
                      << measurement.energy_wh << " Wh\n";

            csv << timestamp << ',' << measurement.sample << ','
                << std::fixed << std::setprecision(3)
                << measurement.voltage_v << ',' << measurement.current_a << ','
                << measurement.power_w << ',' << measurement.energy_wh << '\n';
            csv.flush();
        }
    } catch (const std::ios_base::failure& error) {
        std::cerr << "CSV write failed for " << CSV_PATH << ": "
                  << error.what() << '\n';
        exit_code = 1;
    }

    if (close(fd) == -1) {
        std::cerr << "Failed to close " << DEVICE_PATH
                  << ": " << std::strerror(errno) << '\n';
        exit_code = 1;
    }

    if (exit_code == 0)
        std::cout << "Monitoring stopped cleanly.\n";

    return exit_code;
}
