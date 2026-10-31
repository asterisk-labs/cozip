#ifndef KARU_PROCESS_HPP
#define KARU_PROCESS_HPP

#include <chrono>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>

namespace karu::os {

struct CommandResult {
    std::string output;
    int exit_code = -1;
    bool timed_out = false;
    bool output_limit_exceeded = false;
};

// Runs a command through the platform shell and captures stdout. A zero
// timeout deliberately disables the deadline.
[[nodiscard]] std::expected<CommandResult, std::error_code>
run_command(std::string_view command, std::size_t output_limit, std::chrono::milliseconds timeout);

} // namespace karu::os

#endif
