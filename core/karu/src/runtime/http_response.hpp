#ifndef KARU_HTTP_RESPONSE_HPP
#define KARU_HTTP_RESPONSE_HPP

#include "transport.hpp"

#include <chrono>
#include <ctime>
#include <random>
#include <string>
#include <string_view>

namespace karu::transport::detail {

[[nodiscard]] std::string summarize(std::string_view body);
[[nodiscard]] karu_status status_for_http(long status) noexcept;
[[nodiscard]] bool transient(CURLcode code, long status) noexcept;
[[nodiscard]] std::string s3_region(std::string_view body);
[[nodiscard]] bool credentials_expired(Backend backend, long status,
                                       std::string_view body) noexcept;
[[nodiscard]] bool credentials_expired(const Transfer& transfer) noexcept;
[[nodiscard]] int retry_after_seconds(std::string_view value, std::time_t now) noexcept;
[[nodiscard]] std::chrono::milliseconds retry_delay(int attempt, int retry_after,
                                                    std::mt19937& random);
// Whether the headers report a transformation of the stored bytes.
[[nodiscard]] bool transformed(const EncodingHeaders& encoding) noexcept;
[[nodiscard]] std::string transformation_detail(std::string_view url,
                                                const EncodingHeaders& encoding);

} // namespace karu::transport::detail

#endif
