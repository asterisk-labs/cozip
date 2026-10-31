// The few places where POSIX and Windows differ.
#ifndef KARU_PLATFORM_HPP
#define KARU_PLATFORM_HPP

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <expected>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace karu::os {

int pid();

// UTC broken-down time to Unix time.
std::time_t timegm(std::tm& tm);

// karu's paths are UTF-8 everywhere; Windows wants them wide.
std::filesystem::path path_from_utf8(std::string_view utf8);

// A file open for positional reads. One per transfer, so an implementation
// may move the file pointer.
class File {
  public:
    File() = default;
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    std::error_code open(std::string_view utf8_path);
    // Bytes read; zero at end of file.
    std::expected<std::size_t, std::error_code> read_at(void* buf, std::size_t n,
                                                        std::uint64_t offset);

  private:
    int fd_ = -1;
};

} // namespace karu::os

#endif
