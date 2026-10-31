// Per-thread error text. The status code is the return value; the message is
// a side channel valid until the next karu call on the same thread.
#ifndef KARU_ERROR_HPP
#define KARU_ERROR_HPP

#include <string_view>

namespace karu {

void set_error(std::string_view message) noexcept;

void clear_error() noexcept;
const char* last_error() noexcept;

} // namespace karu

#endif
