#include "batch.hpp"

#include "../platform.hpp"

#include <chrono>
#include <utility>

karu_batch::karu_batch() : owner_process(karu::os::pid()) {}

karu_status karu_batch::next(karu::Completion& out, int timeout_ms) {
    std::unique_lock lock(mutex);
    const auto can_return = [this] {
        return !ready.empty() || pending_parts == 0 || cancelled.load(std::memory_order_acquire);
    };

    if (timeout_ms < 0) {
        cv.wait(lock, can_return);
    } else if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), can_return)) {
        return KARU_TIMEOUT;
    }

    if (ready.empty())
        return KARU_END;
    out = std::move(ready.front());
    ready.pop_front();
    return KARU_OK;
}

void karu_batch::push(karu::Completion completion) {
    std::lock_guard lock(mutex);
    ready.push_back(std::move(completion));
    if (pending_parts > 0)
        --pending_parts;
    cv.notify_all();
}

void karu_batch::transfer_finished() {
    std::lock_guard lock(mutex);
    if (live_transfers > 0)
        --live_transfers;
    // Keep the notification inside the lock. A waiter that observes zero may
    // destroy the condition variable as soon as it reacquires the mutex.
    cv.notify_all();
}

bool karu_batch::is_cancelled() const {
    return cancelled.load(std::memory_order_acquire);
}

bool karu_batch::inherited() const {
    return owner_process != karu::os::pid();
}

std::string karu_batch::region_hint(std::string_view resource) const {
    std::lock_guard lock(mutex);
    const auto found = regions.find(std::string(resource));
    return found == regions.end() ? std::string{} : found->second;
}

void karu_batch::remember_region(std::string resource, std::string region) {
    std::lock_guard lock(mutex);
    regions.insert_or_assign(std::move(resource), std::move(region));
}
