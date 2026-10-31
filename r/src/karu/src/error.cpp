#include "error.hpp"

#include <algorithm>
#include <cstring>

namespace karu {
namespace {
constexpr std::size_t MESSAGE_MAX = 512;
thread_local char g_message[MESSAGE_MAX] = {'\0'};
} // namespace

void set_error(std::string_view message) noexcept {
    const std::size_t n = std::min(message.size(), MESSAGE_MAX - 1);
    std::memcpy(g_message, message.data(), n);
    g_message[n] = '\0';
}

void clear_error() noexcept {
    g_message[0] = '\0';
}

const char* last_error() noexcept {
    return g_message;
}

} // namespace karu
