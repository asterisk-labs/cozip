#ifndef KARU_ENGINE_HPP
#define KARU_ENGINE_HPP

#include "../config.hpp"
#include "../request_builder.hpp"
#include "batch.hpp"
#include "curl_types.hpp"
#include "transfer.hpp"
#include "transport.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace karu {

struct Retry {
    std::chrono::steady_clock::time_point due;
    std::unique_ptr<Transfer> transfer;
};

struct CurlShareState {
    std::array<std::mutex, CURL_LOCK_DATA_LAST> locks;
};

// Owned by the I/O thread. Other threads may call curl_multi_wakeup or set
// cancellation_pending; transfers arrive through the engine's HTTP queue.
struct IoLoop {
    std::uint64_t bit = 0;
    std::size_t limit = 0;
    Multi multi;
    std::thread thread;
    std::vector<Easy> easy_pool;
    std::deque<std::unique_ptr<Transfer>> pending;
    std::unordered_map<Transfer*, std::unique_ptr<Transfer>> active;
    std::deque<Retry> retries;
    std::atomic<bool> cancellation_pending{false};
};

class Engine {
  public:
    explicit Engine(ConfigSnapshot config);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    [[nodiscard]] std::unique_ptr<BatchCore> submit(std::span<const Request> requests,
                                                    const ClientOptions& plan_options,
                                                    karu_status& status);

    void cancel(BatchCore& batch);
    karu_status size_of(const Locator& locator, std::uint64_t& size);
    karu_status object_info(const Locator& locator, std::uint64_t& size, std::string& etag);
    // Stores the first head.size() and the last tail.size() visible bytes, or
    // all of them when the object is shorter.
    karu_status read_ends(const Locator& locator, std::span<std::byte> head,
                          std::span<std::byte> tail, std::uint64_t& size, std::string& etag);

  private:
    void io_loop(IoLoop& loop);
    void file_loop();
    void credential_loop();
    void start_transfer(IoLoop& loop, std::unique_ptr<Transfer> transfer);
    void finish_transfer(std::unique_ptr<Transfer> transfer, karu_status status,
                         std::string detail = {});
    void deliver(Transfer& transfer, karu_status status, const std::string& detail);
    void discard_transfer(std::unique_ptr<Transfer> transfer) noexcept;
    void discard_cancelled_http(IoLoop& loop);
    void queue_http(std::unique_ptr<Transfer> transfer);
    void take_http(IoLoop& loop);
    static void recycle_handle(IoLoop& loop, Transfer& transfer) noexcept;
    void wake_loops() noexcept;
    void wake_idle_loops(std::size_t count) noexcept;
    void stop_workers() noexcept;
    [[nodiscard]] Easy take_size_handle();
    void return_size_handle(Easy handle) noexcept;
    [[nodiscard]] std::expected<transport::RangeReply, transport::Failure>
    get_range(const Locator& locator, const transport::ByteRange& range,
              std::span<std::byte> destination, std::string_view if_match);

    std::atomic<bool> stop_{false};
    std::vector<std::thread> file_workers_;
    std::vector<std::thread> credential_workers_;

    RequestBuilder request_builder_;
    ClientOptions options_;
    CurlShareState share_state_;
    Share share_;
    std::vector<std::unique_ptr<IoLoop>> loops_;
    // Synchronous probes need handles separate from the I/O loops.
    std::mutex size_pool_mutex_;
    std::vector<Easy> size_pool_;

    // Separate waits keep file work from waking credential workers and vice versa.
    std::mutex queue_mutex_;
    std::condition_variable file_cv_;
    std::condition_variable credential_cv_;
    std::deque<std::unique_ptr<Transfer>> file_queue_;
    std::deque<std::unique_ptr<Transfer>> credential_queue_;

    // Shared by all loops; each takes only as many transfers as it can run.
    std::mutex http_mutex_;
    std::deque<std::unique_ptr<Transfer>> http_queue_;
    std::atomic<std::size_t> http_backlog_{0};
    // Bits mark loops about to poll with spare capacity. Producers clear a
    // bit before waking that loop; see wake_idle_loops() and io_loop().
    std::atomic<std::uint64_t> idle_loops_{0};
};

} // namespace karu

#endif
