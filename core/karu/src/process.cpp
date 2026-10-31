#include "process.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace karu::os {
namespace {

#ifdef _WIN32

class Handle {
  public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    ~Handle() {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : value_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            Handle replacement(other.release());
            std::swap(value_, replacement.value_);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(value_, INVALID_HANDLE_VALUE); }
    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        Handle replacement(value);
        std::swap(value_, replacement.value_);
    }

  private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

std::error_code windows_error(DWORD value = GetLastError()) {
    return {static_cast<int>(value), std::system_category()};
}

std::expected<std::wstring, std::error_code> wide(std::string_view text) {
    if (text.empty())
        return std::wstring{};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return std::unexpected(std::make_error_code(std::errc::value_too_large));
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0)
        return std::unexpected(windows_error());
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size) != size) {
        return std::unexpected(windows_error());
    }
    return result;
}

void terminate_process(HANDLE process, HANDLE job) noexcept {
    if (job == nullptr || !TerminateJobObject(job, 1))
        TerminateProcess(process, 1);
    WaitForSingleObject(process, 5'000);
}

#else

void wait_for_child(pid_t child, int& status) noexcept {
    while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
    }
}

void terminate_process(pid_t child, int& status) noexcept {
    // The child creates its own process group before invoking the shell. Kill
    // the group so a credential helper cannot leave descendants behind.
    ::kill(-child, SIGKILL);
    ::kill(child, SIGKILL);
    wait_for_child(child, status);
}

std::error_code setup_spawn(posix_spawn_file_actions_t& actions,
                            posix_spawnattr_t& attributes, int read_descriptor,
                            int write_descriptor) {
    int status = posix_spawn_file_actions_init(&actions);
    if (status != 0)
        return {status, std::generic_category()};

    status = posix_spawnattr_init(&attributes);
    if (status != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return {status, std::generic_category()};
    }

    const auto add_action = [&](int result) {
        if (status == 0 && result != 0)
            status = result;
    };
    add_action(posix_spawn_file_actions_adddup2(&actions, write_descriptor, STDOUT_FILENO));
    add_action(posix_spawn_file_actions_addclose(&actions, read_descriptor));
    if (write_descriptor != STDOUT_FILENO)
        add_action(posix_spawn_file_actions_addclose(&actions, write_descriptor));
    add_action(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP));
    add_action(posix_spawnattr_setpgroup(&attributes, 0));

    if (status != 0) {
        posix_spawnattr_destroy(&attributes);
        posix_spawn_file_actions_destroy(&actions);
        return {status, std::generic_category()};
    }
    return {};
}

#endif

} // namespace

std::expected<CommandResult, std::error_code>
run_command(std::string_view command, std::size_t output_limit, std::chrono::milliseconds timeout) {
    if (command.empty() || command.find('\0') != std::string_view::npos)
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));

    const bool have_timeout = timeout > std::chrono::milliseconds::zero();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    CommandResult result;

#ifdef _WIN32
    auto command_wide = wide(command);
    if (!command_wide)
        return std::unexpected(command_wide.error());

    std::array<wchar_t, MAX_PATH> shell_buffer{};
    const DWORD shell_size = GetEnvironmentVariableW(L"COMSPEC", shell_buffer.data(),
                                                     static_cast<DWORD>(shell_buffer.size()));
    const std::wstring shell = shell_size > 0 && shell_size < shell_buffer.size()
                                   ? std::wstring(shell_buffer.data(), shell_size)
                                   : L"C:\\Windows\\System32\\cmd.exe";
    std::wstring command_line = L"\"" + shell + L"\" /D /S /C \"" + *command_wide + L"\"";

    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE raw_read = INVALID_HANDLE_VALUE;
    HANDLE raw_write = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&raw_read, &raw_write, &security, 0))
        return std::unexpected(windows_error());
    Handle read_pipe(raw_read);
    Handle write_pipe(raw_write);
    if (!SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0))
        return std::unexpected(windows_error());

    STARTUPINFOW startup{};
    startup.cb = static_cast<DWORD>(sizeof(startup));
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe.get();
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process_info{};
    const DWORD flags = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED;
    if (!CreateProcessW(shell.c_str(), command_line.data(), nullptr, nullptr, TRUE, flags, nullptr,
                        nullptr, &startup, &process_info)) {
        return std::unexpected(windows_error());
    }
    Handle process(process_info.hProcess);
    Handle thread(process_info.hThread);

    // Best effort: a job lets timeout cleanup include helpers spawned by the
    // command. The fallback still terminates the direct child.
    Handle job(CreateJobObjectW(nullptr, nullptr));
    bool assigned_to_job = false;
    if (job.get() != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits,
                                    static_cast<DWORD>(sizeof(limits))) &&
            AssignProcessToJobObject(job.get(), process.get())) {
            assigned_to_job = true;
        }
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        const std::error_code error = windows_error();
        terminate_process(process.get(), assigned_to_job ? job.get() : nullptr);
        return std::unexpected(error);
    }
    thread.reset();
    write_pipe.reset();

    bool pipe_closed = false;
    bool process_finished = false;
    while (!pipe_closed || !process_finished) {
        DWORD available = 0;
        if (!pipe_closed &&
            !PeekNamedPipe(read_pipe.get(), nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) {
                pipe_closed = true;
            } else {
                terminate_process(process.get(), assigned_to_job ? job.get() : nullptr);
                return std::unexpected(windows_error(error));
            }
        }
        while (available > 0) {
            std::array<char, 4096> buffer{};
            DWORD read = 0;
            const DWORD wanted = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
            if (!ReadFile(read_pipe.get(), buffer.data(), wanted, &read, nullptr)) {
                const DWORD error = GetLastError();
                if (error == ERROR_BROKEN_PIPE) {
                    pipe_closed = true;
                    break;
                }
                terminate_process(process.get(), assigned_to_job ? job.get() : nullptr);
                return std::unexpected(windows_error(error));
            }
            if (read > output_limit - std::min(output_limit, result.output.size())) {
                result.output_limit_exceeded = true;
                terminate_process(process.get(), assigned_to_job ? job.get() : nullptr);
                return result;
            }
            result.output.append(buffer.data(), read);
            available -= std::min(available, read);
        }

        process_finished = WaitForSingleObject(process.get(), 0) == WAIT_OBJECT_0;
        if (have_timeout && std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            terminate_process(process.get(), assigned_to_job ? job.get() : nullptr);
            return result;
        }
        if (!pipe_closed || !process_finished)
            Sleep(10);
    }

    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.get(), &exit_code))
        return std::unexpected(windows_error());
    result.exit_code = static_cast<int>(exit_code);
#else
    std::string command_copy(command);
    int descriptors[2]{};
    if (::pipe(descriptors) != 0)
        return std::unexpected(std::error_code(errno, std::generic_category()));
    const int read_descriptor = descriptors[0];
    const int write_descriptor = descriptors[1];

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (const std::error_code error =
            setup_spawn(actions, attributes, read_descriptor, write_descriptor)) {
        ::close(read_descriptor);
        ::close(write_descriptor);
        return std::unexpected(error);
    }

    std::array<char, 3> shell_name{'s', 'h', '\0'};
    std::array<char, 3> option{'-', 'c', '\0'};
    char* arguments[]{shell_name.data(), option.data(), command_copy.data(), nullptr};
    pid_t child = -1;
    const int spawn_status =
        posix_spawn(&child, "/bin/sh", &actions, &attributes, arguments, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    ::close(write_descriptor);
    if (spawn_status != 0) {
        ::close(read_descriptor);
        return std::unexpected(std::error_code(spawn_status, std::generic_category()));
    }

    const int flags = ::fcntl(read_descriptor, F_GETFL, 0);
    if (flags < 0 || ::fcntl(read_descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        const std::error_code error(errno, std::generic_category());
        int status = 0;
        terminate_process(child, status);
        ::close(read_descriptor);
        return std::unexpected(error);
    }

    int status = 0;
    bool pipe_closed = false;
    bool child_finished = false;
    while (!pipe_closed || !child_finished) {
        while (!pipe_closed) {
            std::array<char, 4096> buffer{};
            const ssize_t read = ::read(read_descriptor, buffer.data(), buffer.size());
            if (read > 0) {
                const std::size_t count = static_cast<std::size_t>(read);
                if (count > output_limit - std::min(output_limit, result.output.size())) {
                    result.output_limit_exceeded = true;
                    terminate_process(child, status);
                    ::close(read_descriptor);
                    return result;
                }
                result.output.append(buffer.data(), count);
                continue;
            }
            if (read == 0) {
                pipe_closed = true;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                const std::error_code error(errno, std::generic_category());
                terminate_process(child, status);
                ::close(read_descriptor);
                return std::unexpected(error);
            }
            break;
        }

        if (!child_finished) {
            const pid_t waited = ::waitpid(child, &status, WNOHANG);
            if (waited == child) {
                child_finished = true;
            } else if (waited < 0 && errno != EINTR) {
                const std::error_code error(errno, std::generic_category());
                ::kill(-child, SIGKILL);
                ::kill(child, SIGKILL);
                ::close(read_descriptor);
                return std::unexpected(error);
            }
        }
        if (have_timeout && std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            if (!child_finished)
                terminate_process(child, status);
            else
                ::kill(-child, SIGKILL);
            ::close(read_descriptor);
            return result;
        }
        if (!pipe_closed || !child_finished) {
            const auto remaining = have_timeout
                                       ? std::chrono::duration_cast<std::chrono::milliseconds>(
                                             deadline - std::chrono::steady_clock::now())
                                       : std::chrono::milliseconds(50);
            const int wait_ms =
                static_cast<int>(std::clamp<std::int64_t>(remaining.count(), 1, 50));
            pollfd descriptor{read_descriptor, POLLIN | POLLHUP, 0};
            ::poll(&descriptor, 1, wait_ms);
        }
    }
    ::close(read_descriptor);
    if (WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.exit_code = 128 + WTERMSIG(status);
#endif

    return result;
}

} // namespace karu::os
