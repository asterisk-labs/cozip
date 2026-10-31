#ifndef KARU_PLANNER_HPP
#define KARU_PLANNER_HPP

#include "../config.hpp"
#include "batch.hpp"
#include "transfer.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace karu {

struct TransferPlan {
    std::vector<std::unique_ptr<Transfer>> transfers;
    std::vector<Completion> immediate;
};

[[nodiscard]] TransferPlan plan_transfers(BatchCore& batch, std::span<const Request> requests,
                                          const ClientOptions& options, karu_status& status);

} // namespace karu

#endif
