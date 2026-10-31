#ifndef KARU_BUFFER_HPP
#define KARU_BUFFER_HPP

#include <cstdlib>
#include <memory>

namespace karu {

struct FreeBuffer {
    void operator()(void* ptr) const noexcept { std::free(ptr); }
};

using OwnedBuffer = std::unique_ptr<void, FreeBuffer>;

} // namespace karu

#endif
