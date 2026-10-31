#ifndef KARU_BATCH_HPP
#define KARU_BATCH_HPP

#include "../buffer.hpp"
#include "karu/karu.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace karu {

class Engine;

// Workers update these independently. They do not synchronize payload access.
struct BatchCounters {
    std::atomic<std::uint64_t> transfers{0};
    std::atomic<std::uint64_t> transfers_finished{0};
    std::atomic<std::uint64_t> requested_bytes{0};
    std::atomic<std::uint64_t> received_bytes{0};
    std::atomic<std::uint64_t> retries{0};
    std::atomic<std::uint64_t> throttled{0};
    std::atomic<std::uint64_t> new_connections{0};
    std::atomic<std::uint64_t> resumed{0};
    std::atomic<std::uint64_t> credential_refreshes{0};

    void add(std::atomic<std::uint64_t>& counter, std::uint64_t value) noexcept {
        counter.fetch_add(value, std::memory_order_relaxed);
    }
};

struct Completion {
    void* tag = nullptr;
    karu_status status = KARU_OK;
    std::uint64_t got = 0;
    void* buffer = nullptr;
    OwnedBuffer owned_buffer;
    std::string detail;

    [[nodiscard]] void* release_buffer() noexcept {
        return owned_buffer ? owned_buffer.release() : buffer;
    }
};

} // namespace karu

struct karu_batch {
    karu_batch();

    karu_batch(const karu_batch&) = delete;
    karu_batch& operator=(const karu_batch&) = delete;

    karu_status next(karu::Completion& out, int timeout_ms);
    void push(karu::Completion completion);
    void transfer_finished();
    [[nodiscard]] bool is_cancelled() const;
    // An inherited batch has no workers in the child; its mutex may be locked.
    [[nodiscard]] bool inherited() const;
    [[nodiscard]] std::string region_hint(std::string_view resource) const;
    void remember_region(std::string resource, std::string region);

    std::shared_ptr<karu::Engine> owner;
    const int owner_process;
    karu::BatchCounters counters;

    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<karu::Completion> ready;
    std::unordered_map<std::string, std::string> regions;
    std::size_t pending_parts = 0;
    std::size_t live_transfers = 0;
    std::atomic<bool> cancelled{false};
};

namespace karu {

using BatchCore = ::karu_batch;

} // namespace karu

#endif
