#include "axon/fault/fault.hpp"

#include <algorithm>

namespace axon {

FaultPlan::FaultPlan(const FaultConfig& config, std::uint64_t conn_id,
                     std::uint64_t expected) noexcept {
    SplitMix64 rng{config.seed ^ (conn_id * 0xD1B54A32D192ED03ULL)};
    armed_ = expected > 0 && rng.unit() < config.probability;
    cut_at_ = armed_ ? rng.next() % expected : 0;
}

std::size_t FaultPlan::allow(std::size_t want) const noexcept {
    if (!armed_) {
        return want;
    }
    const std::uint64_t left = cut_at_ > sent_ ? cut_at_ - sent_ : 0;
    return static_cast<std::size_t>(std::min<std::uint64_t>(left, want));
}

}  // namespace axon
