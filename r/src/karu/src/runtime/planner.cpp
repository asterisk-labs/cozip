#include "planner.hpp"

#include "../text.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <optional>
#include <string_view>
#include <tuple>

namespace karu {
namespace {

struct ValidatedRequest {
    const Request* request;
    std::uint64_t absolute_offset;
    std::uint64_t last_byte;
};

bool same_resource(const Request& left, const Request& right) {
    // Different preconditions must remain different HTTP requests even when
    // they address the same object.
    return left.locator->resolved.backend == right.locator->resolved.backend &&
           left.locator->resolved.canonical_uri == right.locator->resolved.canonical_uri &&
           left.if_match == right.if_match;
}

bool resource_less(const Request& left, const Request& right) {
    return std::tie(left.locator->resolved.backend, left.locator->resolved.canonical_uri,
                    left.if_match) < std::tie(right.locator->resolved.backend,
                                              right.locator->resolved.canonical_uri,
                                              right.if_match);
}

std::string range_error(const Request& request, std::string_view reason) {
    const auto& resolved = request.locator->resolved;
    const std::string uri = resolved.backend == Backend::Http ? redact_url(resolved.canonical_uri)
                                                              : resolved.canonical_uri;
    return concat(uri, ": range [", request.offset, ", +", request.length, ") ", reason);
}

std::optional<ValidatedRequest> validate(const Request& request, std::string& error) {
    const auto& resolved = request.locator->resolved;
    if (resolved.window_length != TO_END &&
        (request.offset > resolved.window_length ||
         request.length > resolved.window_length - request.offset)) {
        error = range_error(request, "leaves the locator window");
        return std::nullopt;
    }
    if (request.offset > std::numeric_limits<std::uint64_t>::max() - resolved.window_offset) {
        error = range_error(request, "overflows its absolute offset");
        return std::nullopt;
    }
    const auto absolute = resolved.window_offset + request.offset;
    if (request.length - 1 > std::numeric_limits<std::uint64_t>::max() - absolute) {
        error = range_error(request, "overflows its end offset");
        return std::nullopt;
    }
    return ValidatedRequest{&request, absolute, absolute + request.length - 1};
}

bool allocate_part(Part& part) {
    if (part.buffer != nullptr)
        return true;
    if (part.length > std::numeric_limits<std::size_t>::max())
        return false;
    part.owned_buffer.reset(std::malloc(static_cast<std::size_t>(part.length)));
    part.buffer = part.owned_buffer.get();
    return part.buffer != nullptr;
}

bool excessive_amplification(std::uint64_t span, std::uint64_t useful,
                             std::uint64_t maximum) noexcept {
    if (useful > std::numeric_limits<std::uint64_t>::max() / maximum)
        return false;
    return span > useful * maximum;
}

} // namespace

TransferPlan plan_transfers(BatchCore& batch, std::span<const Request> requests,
                            const ClientOptions& options, karu_status& status) {
    TransferPlan plan;
    status = KARU_OK;

    std::vector<ValidatedRequest> valid;
    valid.reserve(requests.size());

    for (const Request& request : requests) {
        if (request.locator->resolved.backend == Backend::File && !request.if_match.empty()) {
            Completion completion{};
            completion.tag = request.tag;
            completion.status = KARU_ERR_UNSUPPORTED;
            completion.buffer = request.buffer;
            completion.detail = request.locator->resolved.canonical_uri +
                                ": ETag preconditions are not available for local files";
            plan.immediate.push_back(std::move(completion));
            continue;
        }
        std::string detail;
        auto checked = validate(request, detail);
        if (checked) {
            valid.push_back(*checked);
        } else {
            Completion completion{};
            completion.tag = request.tag;
            completion.status = KARU_ERR_RANGE;
            completion.buffer = request.buffer;
            completion.detail = std::move(detail);
            plan.immediate.push_back(std::move(completion));
        }
    }

    std::stable_sort(valid.begin(), valid.end(),
                     [](const ValidatedRequest& left, const ValidatedRequest& right) {
                         if (resource_less(*left.request, *right.request))
                             return true;
                         if (resource_less(*right.request, *left.request))
                             return false;
                         return left.absolute_offset < right.absolute_offset;
                     });

    std::shared_ptr<const Locator> shared_locator;
    for (std::size_t first = 0; first < valid.size();) {
        const auto& head = valid[first];
        if (!shared_locator ||
            shared_locator->resolved.backend != head.request->locator->resolved.backend ||
            shared_locator->resolved.canonical_uri !=
                head.request->locator->resolved.canonical_uri) {
            shared_locator = std::make_shared<Locator>(*head.request->locator);
        }
        auto transfer = std::make_unique<Transfer>();
        transfer->batch = &batch;
        transfer->locator = shared_locator;
        transfer->offset = head.absolute_offset;
        transfer->length = head.request->length;
        transfer->if_match = head.request->if_match;
        Part first_part{};
        first_part.length = head.request->length;
        first_part.buffer = head.request->buffer;
        first_part.tag = head.request->tag;
        transfer->parts.push_back(std::move(first_part));

        std::uint64_t last_byte = head.last_byte;
        std::uint64_t useful_bytes = head.request->length;
        std::size_t next = first + 1;
        // A local pread gains no round-trip savings from reading through gaps.
        const std::uint64_t coalesce_gap =
            head.request->locator->resolved.backend == Backend::File ? 0 : options.coalesce_gap;
        while (options.coalesce_gap > 0 && next < valid.size()) {
            const auto& candidate = valid[next];
            if (!same_resource(*head.request, *candidate.request)) {
                break;
            }
            if (candidate.absolute_offset > last_byte) {
                const std::uint64_t distance = candidate.absolute_offset - last_byte;
                if (distance - 1 > coalesce_gap)
                    break;
            }
            const std::uint64_t merged_last = std::max(last_byte, candidate.last_byte);
            if (merged_last - transfer->offset == std::numeric_limits<std::uint64_t>::max())
                break;
            const std::uint64_t merged_length = merged_last - transfer->offset + 1;
            const std::uint64_t merged_useful =
                candidate.request->length > std::numeric_limits<std::uint64_t>::max() - useful_bytes
                    ? std::numeric_limits<std::uint64_t>::max()
                    : useful_bytes + candidate.request->length;
            if (transfer->parts.size() == options.coalesce_parts ||
                merged_length > options.coalesce_limit ||
                excessive_amplification(merged_length, merged_useful,
                                        options.coalesce_amplification)) {
                break;
            }
            Part part{};
            part.relative_offset = candidate.absolute_offset - transfer->offset;
            part.length = candidate.request->length;
            part.buffer = candidate.request->buffer;
            part.tag = candidate.request->tag;
            transfer->parts.push_back(std::move(part));
            last_byte = merged_last;
            useful_bytes = merged_useful;
            ++next;
        }
        transfer->length = last_byte - transfer->offset + 1;

        for (Part& part : transfer->parts) {
            if (!allocate_part(part)) {
                status = KARU_ERR_NOMEM;
                return plan;
            }
        }
        transfer->scattered = transfer->parts.size() != 1 ||
                              transfer->parts.front().relative_offset != 0 ||
                              transfer->parts.front().length != transfer->length;
        plan.transfers.push_back(std::move(transfer));
        first = next;
    }
    return plan;
}

} // namespace karu
