#ifndef KARU_LOCATOR_HPP
#define KARU_LOCATOR_HPP

#include "uri.hpp"

struct karu_locator {
    karu::Resolved resolved;
};

namespace karu {

using Locator = ::karu_locator;

} // namespace karu

#endif
