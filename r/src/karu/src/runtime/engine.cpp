#include "engine.hpp"

#include "../backends/contract.hpp"
#include "../error.hpp"
#include "../platform.hpp"
#include "../text.hpp"
#include "http_response.hpp"
#include "planner.hpp"
#include "transport.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <future>
#include <random>
#include <stdexcept>
#include <utility>

namespace karu {
namespace {

using SteadyClock = std::chrono::steady_clock;

constexpr int kFileWorkers = 4;
constexpr int kCredentialWorkers = 4;

CURLcode initialize_curl() {
    static std::once_flag initialized;
    static CURLcode result = CURLE_FAILED_INIT;
    std::call_once(initialized, [] { result = curl_global_init(CURL_GLOBAL_DEFAULT); });
    return result;
}

void share_lock(CURL*, curl_lock_data data, curl_lock_access, void* user_data) {
    auto& state = *static_cast<CurlShareState*>(user_data);
    state.locks[static_cast<std::size_t>(data) % state.locks.size()].lock();
}

void share_unlock(CURL*, curl_lock_data data, void* user_data) {
    auto& state = *static_cast<CurlShareState*>(user_data);
    state.locks[static_cast<std::size_t>(data) % state.locks.size()].unlock();
}

void require_share_option(CURLSH* share, CURLSHoption option, curl_lock_function callback) {
    const CURLSHcode result = curl_share_setopt(share, option, callback);
    if (result != CURLSHE_OK)
        throw std::runtime_error(curl_share_strerror(result));
}

void require_share_option(CURLSH* share, CURLSHoption option, curl_unlock_function callback) {
    const CURLSHcode result = curl_share_setopt(share, option, callback);
    if (result != CURLSHE_OK)
        throw std::runtime_error(curl_share_strerror(result));
}

void require_shared_data(CURLSH* share, curl_lock_data data) {
    const CURLSHcode result = curl_share_setopt(share, CURLSHOPT_SHARE, data);
    if (result != CURLSHE_OK)
        throw std::runtime_error(curl_share_strerror(result));
}

template <typename Value> void require_multi_option(CURLM* multi, CURLMoption option, Value value) {
    const CURLMcode result = curl_multi_setopt(multi, option, value);
    if (result != CURLM_OK)
        throw std::runtime_error(curl_multi_strerror(result));
}

std::mt19937 retry_generator(const void* identity) {
    const auto ticks = static_cast<std::uint64_t>(SteadyClock::now().time_since_epoch().count());
    const auto address = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(identity));
    std::seed_seq seed{static_cast<std::uint32_t>(os::pid()), static_cast<std::uint32_t>(ticks),
                       static_cast<std::uint32_t>(ticks >> 32), static_cast<std::uint32_t>(address),
                       static_cast<std::uint32_t>(address >> 32)};
    return std::mt19937(seed);
}

std::string timeout_detail(const Transfer& transfer,
                           std::string_view reason = "request timeout exceeded") {
    const std::string& uri = transfer.request_url.empty() ? transfer.locator->resolved.canonical_uri
                                                          : transfer.request_url;
    return concat(redact_url(uri), ": ", reason);
}

std::string shown_uri(const Resolved& resolved) {
    return resolved.backend == Backend::Http ? redact_url(resolved.canonical_uri)
                                             : resolved.canonical_uri;
}

std::uint64_t visible_size(const Resolved& resolved, std::uint64_t total) noexcept {
    if (resolved.window_length != TO_END)
        return resolved.window_length;
    return total > resolved.window_offset ? total - resolved.window_offset : 0;
}

std::size_t at_most(std::size_t length, std::uint64_t limit) noexcept {
    return static_cast<std::size_t>(std::min<std::uint64_t>(length, limit));
}

karu_status short_read(const Resolved& resolved, std::uint64_t first, std::uint64_t length,
                       std::uint64_t got) {
    set_error(concat(shown_uri(resolved), ": object ended at byte ", first + got,
                     " while reading [", first, ", +", length, ")"));
    return KARU_ERR_RANGE;
}

karu_status read_file(os::File& file, const Resolved& resolved, std::uint64_t first,
                      std::span<std::byte> destination) {
    std::size_t done = 0;
    while (done < destination.size()) {
        const auto read =
            file.read_at(destination.data() + done, destination.size() - done, first + done);
        if (!read) {
            set_error(concat(resolved.target, ": ", read.error().message()));
            return KARU_ERR_IO;
        }
        if (*read == 0)
            return short_read(resolved, first, destination.size(), done);
        done += *read;
    }
    return KARU_OK;
}

karu_status read_file_ends(const Resolved& resolved, std::span<std::byte> head,
                           std::span<std::byte> tail, std::uint64_t& size) {
    std::error_code error;
    const auto total = std::filesystem::file_size(os::path_from_utf8(resolved.target), error);
    if (error) {
        set_error(concat(resolved.target, ": ", error.message()));
        return KARU_ERR_IO;
    }
    os::File file;
    if (const std::error_code opened = file.open(resolved.target)) {
        set_error(concat(resolved.target, ": ", opened.message()));
        return KARU_ERR_IO;
    }
    const std::uint64_t visible = visible_size(resolved, total);
    const std::size_t tail_length = at_most(tail.size(), visible);
    karu_status status = read_file(file, resolved, resolved.window_offset,
                                   head.first(at_most(head.size(), visible)));
    if (status == KARU_OK) {
        status = read_file(file, resolved, resolved.window_offset + visible - tail_length,
                           tail.first(tail_length));
    }
    if (status == KARU_OK)
        size = visible;
    return status;
}

// Replies to separate requests must describe one version of the object.
bool same_version(const transport::RangeReply& left, const transport::RangeReply& right) noexcept {
    return left.total == right.total &&
           (left.etag.empty() || right.etag.empty() || left.etag == right.etag);
}

} // namespace

Engine::Engine(ConfigSnapshot config)
    : request_builder_(std::move(config)), options_(request_builder_.config().client_options()) {
    const CURLcode initialized = initialize_curl();
    if (initialized != CURLE_OK) {
        throw std::runtime_error(curl_easy_strerror(initialized));
    }

    share_.reset(curl_share_init());
    if (!share_)
        throw std::bad_alloc();

    require_share_option(share_.get(), CURLSHOPT_LOCKFUNC, share_lock);
    require_share_option(share_.get(), CURLSHOPT_UNLOCKFUNC, share_unlock);
    if (const CURLSHcode result =
            curl_share_setopt(share_.get(), CURLSHOPT_USERDATA, &share_state_);
        result != CURLSHE_OK) {
        throw std::runtime_error(curl_share_strerror(result));
    }
    require_shared_data(share_.get(), CURL_LOCK_DATA_DNS);
    require_shared_data(share_.get(), CURL_LOCK_DATA_SSL_SESSION);
    size_pool_.reserve(static_cast<std::size_t>(options_.concurrency));

    // Split the client-wide limit, with at least one transfer slot per loop.
    const auto concurrency = static_cast<std::size_t>(options_.concurrency);
    const std::size_t count = std::clamp<std::size_t>(static_cast<std::size_t>(options_.io_threads),
                                                      1, std::min<std::size_t>(concurrency, 64));
    loops_.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto loop = std::make_unique<IoLoop>();
        loop->bit = std::uint64_t{1} << index;
        loop->limit = concurrency / count + (index < concurrency % count ? 1 : 0);
        loop->multi.reset(curl_multi_init());
        if (!loop->multi)
            throw std::bad_alloc();
        require_multi_option(loop->multi.get(), CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
        require_multi_option(loop->multi.get(), CURLMOPT_MAX_TOTAL_CONNECTIONS, 0L);
        require_multi_option(loop->multi.get(), CURLMOPT_MAX_HOST_CONNECTIONS, 0L);
        require_multi_option(loop->multi.get(), CURLMOPT_MAXCONNECTS,
                             static_cast<long>(loop->limit));
        loop->easy_pool.reserve(loop->limit);
        loop->active.reserve(loop->limit);
        loops_.push_back(std::move(loop));
    }

    try {
        for (auto& loop : loops_) {
            IoLoop* owned = loop.get();
            loop->thread = std::thread([this, owned] { io_loop(*owned); });
        }
        file_workers_.reserve(kFileWorkers);
        for (int index = 0; index < kFileWorkers; ++index) {
            file_workers_.emplace_back([this] { file_loop(); });
        }
        credential_workers_.reserve(kCredentialWorkers);
        for (int index = 0; index < kCredentialWorkers; ++index) {
            credential_workers_.emplace_back([this] { credential_loop(); });
        }
    } catch (...) {
        stop_workers();
        throw;
    }
}

Engine::~Engine() {
    stop_workers();

    for (auto& transfer : http_queue_) {
        discard_transfer(std::move(transfer));
    }
    for (auto& transfer : file_queue_) {
        discard_transfer(std::move(transfer));
    }
    for (auto& transfer : credential_queue_) {
        discard_transfer(std::move(transfer));
    }
}

void Engine::stop_workers() noexcept {
    {
        // Publish shutdown while holding the mutex used by file workers for
        // their wait predicate. Without this pairing, a notify can land
        // between the predicate check and the worker actually sleeping.
        std::lock_guard lock(queue_mutex_);
        stop_.store(true, std::memory_order_release);
    }
    file_cv_.notify_all();
    credential_cv_.notify_all();
    wake_loops();
    for (auto& loop : loops_) {
        if (loop->thread.joinable())
            loop->thread.join();
    }
    for (std::thread& worker : file_workers_) {
        if (worker.joinable())
            worker.join();
    }
    for (std::thread& worker : credential_workers_) {
        if (worker.joinable())
            worker.join();
    }
}

std::unique_ptr<BatchCore> Engine::submit(std::span<const Request> requests,
                                          const ClientOptions& plan_options, karu_status& status) {
    auto batch = std::make_unique<BatchCore>();
    if (requests.empty()) {
        status = KARU_OK;
        return batch;
    }

    TransferPlan plan = plan_transfers(*batch, requests, plan_options, status);
    if (status != KARU_OK)
        return batch;

    {
        std::lock_guard lock(batch->mutex);
        std::uint64_t requested = 0;
        for (const auto& transfer : plan.transfers) {
            batch->pending_parts += transfer->parts.size();
            for (const Part& part : transfer->parts)
                requested += part.length;
        }
        batch->pending_parts += plan.immediate.size();
        batch->live_transfers = plan.transfers.size();
        batch->counters.add(batch->counters.transfers, plan.transfers.size());
        batch->counters.add(batch->counters.requested_bytes, requested);
    }
    for (Completion& completion : plan.immediate) {
        batch->push(std::move(completion));
    }

    std::deque<std::unique_ptr<Transfer>> ready;
    std::size_t files = 0;
    std::size_t lookups = 0;
    {
        std::lock_guard lock(queue_mutex_);
        for (auto& transfer : plan.transfers) {
            if (transfer->locator->resolved.backend == Backend::File) {
                file_queue_.push_back(std::move(transfer));
                ++files;
            } else if (backends::cloud_provider(transfer->locator->resolved.backend) != nullptr) {
                credential_queue_.push_back(std::move(transfer));
                ++lookups;
            } else {
                ready.push_back(std::move(transfer));
            }
        }
    }
    // Wake only the workers that can take this batch's queued transfers.
    const auto notify = [](std::condition_variable& cv, std::size_t count, int workers) {
        if (count >= static_cast<std::size_t>(workers)) {
            cv.notify_all();
            return;
        }
        for (std::size_t index = 0; index < count; ++index)
            cv.notify_one();
    };
    notify(file_cv_, files, kFileWorkers);
    notify(credential_cv_, lookups, kCredentialWorkers);
    if (!ready.empty()) {
        const std::size_t count = ready.size();
        {
            std::lock_guard lock(http_mutex_);
            for (auto& transfer : ready)
                http_queue_.push_back(std::move(transfer));
            http_backlog_.store(http_queue_.size(), std::memory_order_seq_cst);
        }
        wake_idle_loops(count);
    }
    return batch;
}

void Engine::wake_loops() noexcept {
    for (auto& loop : loops_) {
        if (loop->multi)
            curl_multi_wakeup(loop->multi.get());
    }
}

void Engine::wake_idle_loops(std::size_t count) noexcept {
    // Prefer the lowest idle loop to reuse connections for serial reads.
    // Clear its bit first so concurrent producers choose different loops.
    std::uint64_t idle = idle_loops_.load(std::memory_order_seq_cst);
    while (count > 0 && idle != 0) {
        const int index = std::countr_zero(idle);
        const std::uint64_t bit = std::uint64_t{1} << index;
        idle &= ~bit;
        if ((idle_loops_.fetch_and(~bit, std::memory_order_seq_cst) & bit) != 0) {
            curl_multi_wakeup(loops_[static_cast<std::size_t>(index)]->multi.get());
            --count;
        }
    }
}

void Engine::queue_http(std::unique_ptr<Transfer> transfer) {
    {
        std::lock_guard lock(http_mutex_);
        if (stop_.load(std::memory_order_acquire)) {
            discard_transfer(std::move(transfer));
            return;
        }
        http_queue_.push_back(std::move(transfer));
        http_backlog_.store(http_queue_.size(), std::memory_order_seq_cst);
    }
    wake_idle_loops(1);
}

void Engine::take_http(IoLoop& loop) {
    if (http_backlog_.load(std::memory_order_acquire) == 0)
        return;
    std::lock_guard lock(http_mutex_);
    while (loop.active.size() + loop.pending.size() < loop.limit && !http_queue_.empty()) {
        loop.pending.push_back(std::move(http_queue_.front()));
        http_queue_.pop_front();
    }
    http_backlog_.store(http_queue_.size(), std::memory_order_seq_cst);
}

void Engine::recycle_handle(IoLoop& loop, Transfer& transfer) noexcept {
    if (!transfer.easy)
        return;
    curl_easy_reset(transfer.easy.get());
    try {
        loop.easy_pool.push_back(std::move(transfer.easy));
    } catch (...) {
        // The handle is dropped with the transfer instead of pooled.
    }
}

void Engine::deliver(Transfer& transfer, karu_status status, const std::string& detail) {
    for (Part& part : transfer.parts) {
        Completion completion{
            .tag = part.tag,
            .status = status,
            .buffer = part.buffer,
            .owned_buffer = std::move(part.owned_buffer),
            .detail = detail,
        };
        if (status == KARU_OK) {
            const std::uint64_t part_end = part.relative_offset + part.length;
            if (part_end > transfer.received) {
                completion.status = KARU_ERR_RANGE;
                completion.got = transfer.received > part.relative_offset
                                     ? transfer.received - part.relative_offset
                                     : 0;
                completion.detail =
                    concat(shown_uri(transfer.locator->resolved), ": object ended at byte ",
                           transfer.offset + transfer.received, " while reading [",
                           transfer.offset + part.relative_offset, ", +", part.length, ")");
            } else {
                completion.got = part.length;
            }
            if (completion.got > 0 && !transfer.scratch.empty()) {
                std::memcpy(part.buffer, transfer.scratch.data() + part.relative_offset,
                            static_cast<std::size_t>(completion.got));
            }
        }
        transfer.batch->push(std::move(completion));
    }
}

void Engine::finish_transfer(std::unique_ptr<Transfer> transfer, karu_status status,
                             std::string detail) {
    BatchCore* batch = transfer->batch;
    // Publish completions before dropping the last live transfer. A waiter may
    // destroy the batch as soon as transfer_finished() reaches zero.
    // The consumer must see the final count when it receives the last result.
    batch->counters.add(batch->counters.transfers_finished, 1);
    deliver(*transfer, status, detail);
    transfer.reset();
    batch->transfer_finished();
}

void Engine::start_transfer(IoLoop& loop, std::unique_ptr<Transfer> transfer) {
    try {
        transfer->deadline.start(options_.request_timeout_seconds);
        if (transfer->deadline.expired()) {
            const std::string detail = timeout_detail(*transfer);
            finish_transfer(std::move(transfer), KARU_TIMEOUT, detail);
            return;
        }
        if (!transport::ensure_sink(*transfer)) {
            finish_transfer(std::move(transfer), KARU_ERR_NOMEM,
                            "out of memory allocating a coalesced transfer buffer");
            return;
        }

        if (transfer->region_hint.empty()) {
            transfer->region_hint =
                transfer->batch->region_hint(transfer->locator->resolved.canonical_uri);
        }
        const bool cloud = backends::cloud_provider(transfer->locator->resolved.backend) != nullptr;
        if (cloud && !transfer->credentials_ready) {
            finish_transfer(std::move(transfer), KARU_ERR_INVALID,
                            "internal error: cloud request has no resolved credentials");
            return;
        }
        const std::uint64_t first = transfer->offset + transfer->resumed;
        const std::uint64_t length = transfer->length - transfer->resumed;
        const std::string_view pin = transport::version_pin(*transfer);
        auto request = cloud
                           ? request_builder_.materialize(*transfer->locator, transfer->credentials,
                                                          first, length, transfer->region_hint, pin)
                           : request_builder_.prepare(*transfer->locator, first, length,
                                                      transfer->region_hint, pin);
        if (!request) {
            finish_transfer(std::move(transfer), request.error().status,
                            std::move(request.error().message));
            return;
        }
        if (transfer->deadline.expired()) {
            const std::string detail = timeout_detail(*transfer);
            finish_transfer(std::move(transfer), KARU_TIMEOUT, detail);
            return;
        }
        transfer->request_url = std::move(request->url);
        transfer->request_range = std::move(request->range);
        transfer->request_headers = std::move(request->headers);
        transfer->http = std::make_unique<HttpRequestOptions>(std::move(request->http));
        if (!request->routing_region.empty())
            transfer->region_hint = std::move(request->routing_region);

        if (loop.easy_pool.empty()) {
            transfer->easy.reset(curl_easy_init());
        } else {
            transfer->easy = std::move(loop.easy_pool.back());
            loop.easy_pool.pop_back();
        }
        if (!transfer->easy) {
            finish_transfer(std::move(transfer), KARU_ERR_NOMEM, "curl_easy_init: out of memory");
            return;
        }
        transfer->http_buffers = std::make_unique<HttpBuffers>();

        if (auto configured = transport::configure(*transfer, share_.get(), options_);
            !configured) {
            finish_transfer(std::move(transfer), KARU_ERR_NETWORK,
                            "could not configure HTTP request: " + configured.error());
            return;
        }

        CURL* easy = transfer->easy.get();
        const CURLMcode added = curl_multi_add_handle(loop.multi.get(), easy);
        if (added != CURLM_OK) {
            finish_transfer(std::move(transfer), KARU_ERR_NETWORK,
                            concat("curl_multi_add_handle: ", curl_multi_strerror(added)));
            return;
        }
        Transfer* key = transfer.get();
        auto [entry, inserted] = loop.active.try_emplace(key);
        if (!inserted) {
            curl_multi_remove_handle(loop.multi.get(), transfer->easy.get());
            finish_transfer(std::move(transfer), KARU_ERR_INVALID,
                            "internal error registering HTTP transfer");
            return;
        }
        entry->second = std::move(transfer);
    } catch (const std::bad_alloc&) {
        if (transfer) {
            if (transfer->easy)
                curl_multi_remove_handle(loop.multi.get(), transfer->easy.get());
            finish_transfer(std::move(transfer), KARU_ERR_NOMEM, "out of memory preparing request");
        }
    } catch (const std::exception& error) {
        if (transfer) {
            if (transfer->easy)
                curl_multi_remove_handle(loop.multi.get(), transfer->easy.get());
            finish_transfer(std::move(transfer), KARU_ERR_INVALID,
                            concat("could not prepare request: ", error.what()));
        }
    } catch (...) {
        if (transfer) {
            if (transfer->easy)
                curl_multi_remove_handle(loop.multi.get(), transfer->easy.get());
            finish_transfer(std::move(transfer), KARU_ERR_INVALID,
                            "could not prepare request: unknown C++ exception");
        }
    }
}

void Engine::discard_transfer(std::unique_ptr<Transfer> transfer) noexcept {
    BatchCore* batch = transfer->batch;
    transfer.reset();
    batch->transfer_finished();
}

void Engine::discard_cancelled_http(IoLoop& loop) {
    for (auto iterator = loop.pending.begin(); iterator != loop.pending.end();) {
        if (!(*iterator)->batch->is_cancelled()) {
            ++iterator;
            continue;
        }
        auto transfer = std::move(*iterator);
        iterator = loop.pending.erase(iterator);
        discard_transfer(std::move(transfer));
    }

    for (auto iterator = loop.retries.begin(); iterator != loop.retries.end();) {
        if (!iterator->transfer->batch->is_cancelled()) {
            ++iterator;
            continue;
        }
        auto transfer = std::move(iterator->transfer);
        iterator = loop.retries.erase(iterator);
        discard_transfer(std::move(transfer));
    }

    for (auto iterator = loop.active.begin(); iterator != loop.active.end();) {
        if (!iterator->second->batch->is_cancelled()) {
            ++iterator;
            continue;
        }
        curl_multi_remove_handle(loop.multi.get(), iterator->second->easy.get());
        auto transfer = std::move(iterator->second);
        iterator = loop.active.erase(iterator);
        discard_transfer(std::move(transfer));
    }
}

void Engine::io_loop(IoLoop& loop) {
    std::mt19937 random = retry_generator(&loop);
    CURLM* const multi = loop.multi.get();

    while (!stop_.load(std::memory_order_acquire)) {
        take_http(loop);
        if (loop.cancellation_pending.exchange(false, std::memory_order_acq_rel))
            discard_cancelled_http(loop);

        const auto now = SteadyClock::now();
        for (auto iterator = loop.retries.begin(); iterator != loop.retries.end();) {
            if (iterator->due > now) {
                ++iterator;
                continue;
            }
            loop.pending.push_back(std::move(iterator->transfer));
            iterator = loop.retries.erase(iterator);
        }

        while (loop.active.size() < loop.limit && !loop.pending.empty()) {
            auto transfer = std::move(loop.pending.front());
            loop.pending.pop_front();
            if (transfer->batch->is_cancelled()) {
                discard_transfer(std::move(transfer));
                continue;
            }
            start_transfer(loop, std::move(transfer));
        }

        int running = 0;
        const CURLMcode performed = curl_multi_perform(multi, &running);
        if (performed != CURLM_OK) {
            for (auto iterator = loop.active.begin(); iterator != loop.active.end();) {
                curl_multi_remove_handle(multi, iterator->second->easy.get());
                auto transfer = std::move(iterator->second);
                iterator = loop.active.erase(iterator);
                finish_transfer(std::move(transfer), KARU_ERR_NETWORK,
                                concat("curl_multi_perform: ", curl_multi_strerror(performed)));
            }
        }

        int remaining_messages = 0;
        while (CURLMsg* message = curl_multi_info_read(multi, &remaining_messages)) {
            if (message->msg != CURLMSG_DONE)
                continue;
            CURL* const easy = message->easy_handle;
            const CURLcode result = message->data.result;
            Transfer* key = nullptr;
            curl_easy_getinfo(easy, CURLINFO_PRIVATE, &key);
            auto node = loop.active.extract(key);
            if (node.empty())
                continue;
            curl_multi_remove_handle(multi, easy);
            std::unique_ptr<Transfer> transfer = std::move(node.mapped());

            if (transfer->http_status == 0) {
                curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &transfer->http_status);
            }
            {
                BatchCounters& counters = transfer->batch->counters;
                long connects = 0;
                if (curl_easy_getinfo(easy, CURLINFO_NUM_CONNECTS, &connects) == CURLE_OK &&
                    connects > 0) {
                    counters.add(counters.new_connections, static_cast<std::uint64_t>(connects));
                }
                if (transfer->http_status == 429 || transfer->http_status == 503)
                    counters.add(counters.throttled, 1);
            }
            if (transport::succeeded(*transfer, result)) {
                recycle_handle(loop, *transfer);
                finish_transfer(std::move(transfer), KARU_OK);
                continue;
            }

            if (transfer->deadline.expired()) {
                const std::string detail = timeout_detail(*transfer);
                recycle_handle(loop, *transfer);
                finish_transfer(std::move(transfer), KARU_TIMEOUT, detail);
                continue;
            }

            if (transfer->response_region.empty() &&
                transfer->locator->resolved.backend == Backend::S3) {
                transfer->response_region = transport::detail::s3_region(std::string_view(
                    transfer->http_buffers->error_body.data(), transfer->error_body_size));
            }
            if (!transfer->region_retried && !transfer->response_region.empty() &&
                transfer->response_region != transfer->region_hint &&
                transfer->locator->resolved.backend == Backend::S3) {
                // The region is part of SigV4, so rebuild and sign the request.
                transfer->region_hint = transfer->response_region;
                transfer->batch->remember_region(transfer->locator->resolved.canonical_uri,
                                                 transfer->response_region);
                transfer->region_retried = true;
                recycle_handle(loop, *transfer);
                loop.pending.push_front(std::move(transfer));
                continue;
            }

            if (!transfer->credentials_retried &&
                transport::detail::credentials_expired(*transfer)) {
                // Authentication failures get one fresh lookup outside the
                // ordinary retry budget.
                request_builder_.invalidate_credentials(*transfer->locator);
                transfer->batch->counters.add(transfer->batch->counters.credential_refreshes, 1);
                transfer->credentials_retried = true;
                transfer->credentials = {};
                transfer->credentials_ready = false;
                recycle_handle(loop, *transfer);
                {
                    std::lock_guard lock(queue_mutex_);
                    credential_queue_.push_front(std::move(transfer));
                }
                credential_cv_.notify_one();
                continue;
            }

            if (transport::unreachable(result))
                ++transfer->unreachable_attempts;
            if (transfer->attempt + 1 < options_.max_attempts &&
                transfer->unreachable_attempts < transport::kUnreachableAttempts &&
                transport::retryable(*transfer, result)) {
                ++transfer->attempt;
                BatchCounters& counters = transfer->batch->counters;
                counters.add(counters.retries, 1);
                const std::uint64_t kept = transfer->resumed;
                transport::plan_resume(*transfer);
                if (transfer->resumed > kept)
                    counters.add(counters.resumed, 1);
                const auto delay = transport::detail::retry_delay(transfer->attempt,
                                                                  transfer->retry_after, random);
                if (!transfer->deadline.can_wait_for(delay)) {
                    const std::string detail = timeout_detail(
                        *transfer, "request timeout leaves no time for another attempt");
                    recycle_handle(loop, *transfer);
                    finish_transfer(std::move(transfer), KARU_TIMEOUT, detail);
                    continue;
                }
                recycle_handle(loop, *transfer);
                loop.retries.push_back(Retry{SteadyClock::now() + delay, std::move(transfer)});
                continue;
            }

            transport::Failure failed = transport::failure(*transfer, result);
            recycle_handle(loop, *transfer);
            finish_transfer(std::move(transfer), failed.status, std::move(failed.detail));
        }

        // Skip polling when queued work can start now.
        const bool room = loop.active.size() < loop.limit;
        int timeout_ms = 50;
        if (room && (!loop.pending.empty() || http_backlog_.load(std::memory_order_acquire) != 0)) {
            timeout_ms = 0;
        } else if (!loop.retries.empty()) {
            const auto next = std::min_element(loop.retries.begin(), loop.retries.end(),
                                               [](const Retry& left, const Retry& right) {
                                                   return left.due < right.due;
                                               })
                                  ->due;
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(next - SteadyClock::now())
                    .count();
            timeout_ms = static_cast<int>(std::clamp<std::int64_t>(remaining, 0, 50));
        } else if (running == 0 && loop.pending.empty()) {
            timeout_ms = 200;
        }
        // Publish the idle bit before checking the queue. With seq_cst on
        // both sides, either the producer sees the bit and wakes us, or we
        // see its queued work before polling.
        bool announced = false;
        if (room && timeout_ms != 0) {
            idle_loops_.fetch_or(loop.bit, std::memory_order_seq_cst);
            announced = true;
            if (http_backlog_.load(std::memory_order_seq_cst) != 0)
                timeout_ms = 0;
        }
        int descriptors = 0;
        curl_multi_poll(multi, nullptr, 0, timeout_ms, &descriptors);
        if (announced)
            idle_loops_.fetch_and(~loop.bit, std::memory_order_seq_cst);
    }

    while (!loop.active.empty()) {
        auto node = loop.active.extract(loop.active.begin());
        curl_multi_remove_handle(multi, node.mapped()->easy.get());
        discard_transfer(std::move(node.mapped()));
    }
    for (auto& transfer : loop.pending) {
        discard_transfer(std::move(transfer));
    }
    loop.pending.clear();
    for (Retry& retry : loop.retries) {
        discard_transfer(std::move(retry.transfer));
    }
    loop.retries.clear();
}

void Engine::file_loop() {
    while (true) {
        std::unique_ptr<Transfer> transfer;
        {
            std::unique_lock lock(queue_mutex_);
            file_cv_.wait(lock, [this] {
                return stop_.load(std::memory_order_acquire) || !file_queue_.empty();
            });
            if (file_queue_.empty()) {
                if (stop_.load(std::memory_order_acquire))
                    return;
                continue;
            }
            transfer = std::move(file_queue_.front());
            file_queue_.pop_front();
        }

        const std::string& path = transfer->locator->resolved.target;
        if (transfer->batch->is_cancelled()) {
            discard_transfer(std::move(transfer));
            continue;
        }
        if (!transport::ensure_sink(*transfer)) {
            finish_transfer(std::move(transfer), KARU_ERR_NOMEM,
                            concat(path, ": out of memory staging the read"));
            continue;
        }

        os::File file;
        if (const std::error_code error = file.open(path)) {
            finish_transfer(std::move(transfer), KARU_ERR_IO, concat(path, ": ", error.message()));
            continue;
        }

        std::string failure;
        while (transfer->received < transfer->length && !transfer->batch->is_cancelled()) {
            const auto read =
                file.read_at(transfer->sink + transfer->received,
                             static_cast<std::size_t>(transfer->length - transfer->received),
                             transfer->offset + transfer->received);
            if (!read) {
                failure = concat(path, ": ", read.error().message());
                break;
            }
            if (*read == 0)
                break;
            transfer->received += *read;
        }

        transfer->batch->counters.add(transfer->batch->counters.received_bytes, transfer->received);
        if (transfer->batch->is_cancelled()) {
            discard_transfer(std::move(transfer));
        } else if (!failure.empty()) {
            finish_transfer(std::move(transfer), KARU_ERR_IO, std::move(failure));
        } else {
            finish_transfer(std::move(transfer), KARU_OK);
        }
    }
}

void Engine::credential_loop() {
    // Profile helpers, metadata endpoints, and user callbacks may block. Keep
    // them away from the curl event loop.
    while (true) {
        std::unique_ptr<Transfer> transfer;
        {
            std::unique_lock lock(queue_mutex_);
            credential_cv_.wait(lock, [this] {
                return stop_.load(std::memory_order_acquire) || !credential_queue_.empty();
            });
            if (stop_.load(std::memory_order_acquire))
                return;
            transfer = std::move(credential_queue_.front());
            credential_queue_.pop_front();
        }

        if (transfer->batch->is_cancelled()) {
            discard_transfer(std::move(transfer));
            continue;
        }

        transfer->deadline.start(options_.request_timeout_seconds);
        try {
            auto credentials = request_builder_.resolve_credentials(*transfer->locator);
            if (!credentials) {
                finish_transfer(std::move(transfer), credentials.error().status,
                                std::move(credentials.error().message));
                continue;
            }
            if (transfer->batch->is_cancelled()) {
                discard_transfer(std::move(transfer));
                continue;
            }
            if (transfer->deadline.expired()) {
                const std::string detail = timeout_detail(*transfer);
                finish_transfer(std::move(transfer), KARU_TIMEOUT, detail);
                continue;
            }

            transfer->credentials = std::move(*credentials);
            transfer->credentials_ready = true;
            queue_http(std::move(transfer));
        } catch (const std::bad_alloc&) {
            finish_transfer(std::move(transfer), KARU_ERR_NOMEM,
                            "out of memory resolving credentials");
        } catch (const std::exception& error) {
            finish_transfer(std::move(transfer), KARU_ERR_CREDENTIALS,
                            concat("could not resolve credentials: ", error.what()));
        } catch (...) {
            finish_transfer(std::move(transfer), KARU_ERR_CREDENTIALS,
                            "could not resolve credentials: unknown C++ exception");
        }
    }
}

void Engine::cancel(BatchCore& batch) {
    {
        // No live transfers means no workers need cancellation or a wait.
        std::lock_guard lock(batch.mutex);
        if (batch.live_transfers == 0) {
            batch.cancelled.store(true, std::memory_order_release);
            return;
        }
    }
    batch.cancelled.store(true, std::memory_order_release);
    batch.cv.notify_all();

    {
        std::lock_guard lock(queue_mutex_);
        for (auto iterator = file_queue_.begin(); iterator != file_queue_.end();) {
            if ((*iterator)->batch != &batch) {
                ++iterator;
                continue;
            }
            auto transfer = std::move(*iterator);
            iterator = file_queue_.erase(iterator);
            discard_transfer(std::move(transfer));
        }
        for (auto iterator = credential_queue_.begin(); iterator != credential_queue_.end();) {
            if ((*iterator)->batch != &batch) {
                ++iterator;
                continue;
            }
            auto transfer = std::move(*iterator);
            iterator = credential_queue_.erase(iterator);
            discard_transfer(std::move(transfer));
        }
    }

    {
        std::lock_guard lock(http_mutex_);
        for (auto iterator = http_queue_.begin(); iterator != http_queue_.end();) {
            if ((*iterator)->batch != &batch) {
                ++iterator;
                continue;
            }
            auto transfer = std::move(*iterator);
            iterator = http_queue_.erase(iterator);
            discard_transfer(std::move(transfer));
        }
        http_backlog_.store(http_queue_.size(), std::memory_order_seq_cst);
    }

    for (auto& loop : loops_)
        loop->cancellation_pending.store(true, std::memory_order_release);
    wake_loops();

    // Caller-owned destinations are safe to release once every transfer has
    // left its worker or the curl event loop.
    std::unique_lock lock(batch.mutex);
    batch.cv.wait(lock, [&batch] { return batch.live_transfers == 0; });
}

karu_status Engine::size_of(const Locator& locator, std::uint64_t& size) {
    // Bounded windows need no size probe.
    if (locator.resolved.window_length != TO_END) {
        size = locator.resolved.window_length;
        return KARU_OK;
    }
    std::string etag;
    return object_info(locator, size, etag);
}

karu_status Engine::object_info(const Locator& locator, std::uint64_t& size, std::string& etag) {
    const auto& resolved = locator.resolved;
    etag.clear();

    if (resolved.backend == Backend::File) {
        std::error_code error;
        const auto total = std::filesystem::file_size(os::path_from_utf8(resolved.target), error);
        if (error) {
            set_error(concat(resolved.target, ": ", error.message()));
            return KARU_ERR_IO;
        }
        size = visible_size(resolved, total);
        return KARU_OK;
    }

    Easy handle = take_size_handle();
    if (!handle) {
        set_error("curl_easy_init: out of memory");
        return KARU_ERR_NOMEM;
    }
    auto result =
        transport::object_info(locator, handle.get(), share_.get(), request_builder_, options_);
    return_size_handle(std::move(handle));
    if (!result) {
        set_error(result.error().detail);
        return result.error().status;
    }
    size = visible_size(resolved, result->size);
    etag = std::move(result->etag);
    return KARU_OK;
}

karu_status Engine::read_ends(const Locator& locator, std::span<std::byte> head,
                              std::span<std::byte> tail, std::uint64_t& size, std::string& etag) {
    using transport::ByteRange;
    using Reply = std::expected<transport::RangeReply, transport::Failure>;
    const Resolved& resolved = locator.resolved;
    etag.clear();
    if (resolved.backend == Backend::File)
        return read_file_ends(resolved, head, tail, size);
    if (head.empty() && tail.empty())
        return object_info(locator, size, etag);

    // A bounded window knows where its tail starts. Otherwise a suffix range
    // finds it, except on Azure, which answers one with the whole blob.
    const std::uint64_t offset = resolved.window_offset;
    const bool bounded = resolved.window_length != TO_END;
    const bool by_suffix = !bounded && !tail.empty() && resolved.backend != Backend::Azure;
    std::array<std::byte, 1> scratch{};
    std::span<std::byte> head_part =
        bounded ? head.first(at_most(head.size(), resolved.window_length)) : head;
    if (!bounded && head.empty() && !by_suffix)
        head_part = scratch; // the tail needs the size first
    ByteRange tail_range{0, tail.size(), true};
    std::span<std::byte> tail_part = by_suffix ? tail : std::span<std::byte>{};
    if (bounded && resolved.window_length > head_part.size()) {
        const std::size_t length = at_most(tail.size(), resolved.window_length);
        tail_range = ByteRange{offset + resolved.window_length - length, length};
        tail_part = tail.first(length);
    }

    const auto get_head = [&] {
        return get_range(locator, ByteRange{offset, head_part.size()}, head_part, {});
    };
    Reply head_reply = transport::RangeReply{};
    Reply tail_reply = transport::RangeReply{};
    std::future<Reply> pending;
    if (!head_part.empty() && !tail_part.empty())
        pending = std::async(std::launch::async, get_head);
    else if (!head_part.empty())
        head_reply = get_head();
    if (!tail_part.empty())
        tail_reply = get_range(locator, tail_range, tail_part, {});
    if (pending.valid())
        head_reply = pending.get();

    const auto fail = [](const transport::Failure& failure) {
        set_error(failure.detail);
        return failure.status;
    };
    const auto changed = [&resolved] {
        set_error(concat(shown_uri(resolved),
                         ": the object changed between reading its head and its tail"));
        return KARU_ERR_PRECONDITION;
    };
    if (!head_reply)
        return fail(head_reply.error());
    if (!tail_reply)
        return fail(tail_reply.error());
    const bool head_sent = !head_part.empty();
    const bool tail_sent = !tail_part.empty();
    if (head_sent && tail_sent && !same_version(*head_reply, *tail_reply))
        return changed();
    // Ranges inside a bounded window must exist in full.
    if (bounded && head_reply->got != head_part.size())
        return short_read(resolved, offset, head_part.size(), head_reply->got);
    if (bounded && tail_reply->got != tail_part.size())
        return short_read(resolved, tail_range.first, tail_part.size(), tail_reply->got);

    const transport::RangeReply& known = head_sent ? *head_reply : *tail_reply;
    const std::uint64_t visible = visible_size(resolved, known.total);
    const std::size_t head_length = at_most(head.size(), visible);
    const std::size_t tail_length = at_most(tail.size(), visible);
    etag = known.etag.empty() ? tail_reply->etag : known.etag;
    if (tail_length > 0) {
        if (tail_sent && !tail_reply->suffix_ignored) {
            // A suffix longer than the window starts before it.
            if (by_suffix) {
                std::memmove(tail.data(), tail.data() + (tail_reply->got - tail_length),
                             tail_length);
            }
        } else if (visible <= head_length) {
            std::memcpy(tail.data(), head.data() + (visible - tail_length), tail_length);
        } else {
            auto reply = get_range(locator, ByteRange{offset + visible - tail_length, tail_length},
                                   tail.first(tail_length), etag);
            if (!reply)
                return fail(reply.error());
            if (!same_version(*reply, known))
                return changed();
            if (etag.empty())
                etag = std::move(reply->etag);
        }
    }
    size = visible;
    return KARU_OK;
}

Easy Engine::take_size_handle() {
    std::lock_guard lock(size_pool_mutex_);
    if (size_pool_.empty())
        return Easy(curl_easy_init());
    Easy handle = std::move(size_pool_.back());
    size_pool_.pop_back();
    return handle;
}

void Engine::return_size_handle(Easy handle) noexcept {
    curl_easy_reset(handle.get());
    std::lock_guard lock(size_pool_mutex_);
    if (size_pool_.size() < static_cast<std::size_t>(options_.concurrency))
        size_pool_.push_back(std::move(handle));
}

std::expected<transport::RangeReply, transport::Failure>
Engine::get_range(const Locator& locator, const transport::ByteRange& range,
                  std::span<std::byte> destination, std::string_view if_match) {
    Easy handle = take_size_handle();
    if (!handle)
        return std::unexpected(transport::Failure{KARU_ERR_NOMEM, "curl_easy_init: out of memory"});
    auto reply = transport::get_range(locator, handle.get(), share_.get(), request_builder_,
                                      options_, range, destination, if_match);
    return_size_handle(std::move(handle));
    return reply;
}

} // namespace karu
