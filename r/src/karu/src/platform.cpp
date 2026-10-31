#include "platform.hpp"

#include <algorithm>
#include <cerrno>
#include <limits>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace karu::os {

int pid() {
#ifdef _WIN32
    return ::_getpid();
#else
    return static_cast<int>(::getpid());
#endif
}

std::time_t timegm(std::tm& tm) {
#ifdef _WIN32
    return ::_mkgmtime(&tm);
#else
    return ::timegm(&tm);
#endif
}

std::filesystem::path path_from_utf8(std::string_view utf8) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

File::~File() {
    if (fd_ < 0)
        return;
#ifdef _WIN32
    ::_close(fd_);
#else
    ::close(fd_);
#endif
}

std::error_code File::open(std::string_view utf8_path) {
    if (fd_ >= 0)
        return std::make_error_code(std::errc::device_or_resource_busy);
    const std::filesystem::path path = path_from_utf8(utf8_path);
#ifdef _WIN32
    fd_ = ::_wopen(path.c_str(), _O_RDONLY | _O_BINARY);
#else
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    if (fd_ < 0)
        return {errno, std::generic_category()};
    return {};
}

std::expected<std::size_t, std::error_code> File::read_at(void* buf, std::size_t n,
                                                          std::uint64_t offset) {
    if (fd_ < 0)
        return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
#ifdef _WIN32
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<long long>::max())) {
        return std::unexpected(std::make_error_code(std::errc::value_too_large));
    }
    if (::_lseeki64(fd_, static_cast<long long>(offset), SEEK_SET) < 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    const auto want = static_cast<unsigned int>(std::min<std::size_t>(n, 1u << 30));
    const int got = ::_read(fd_, buf, want);
    if (got < 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    return static_cast<std::size_t>(got);
#else
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        return std::unexpected(std::make_error_code(std::errc::value_too_large));
    }
    n = std::min(n, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    for (;;) {
        const ssize_t got = ::pread(fd_, buf, n, static_cast<off_t>(offset));
        if (got >= 0)
            return static_cast<std::size_t>(got);
        if (errno == EINTR)
            continue;
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
#endif
}

} // namespace karu::os
