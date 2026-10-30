#pragma once

#include <cstddef>
#include <cstdint>

namespace axon {

// splitmix64: a tiny, fast generator in which every seed gives an
// independent-looking stream. Bit-for-bit the same as the C version's.
class SplitMix64 {
public:
    explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_{seed} {}

    constexpr std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    // Uniform in [0, 1).
    constexpr double unit() noexcept { return static_cast<double>(next() >> 11) * 0x1.0p-53; }

private:
    std::uint64_t state_;
};

struct FaultConfig {
    double probability = 0;  // chance that a connection is cut, 0 to 1
    std::uint64_t seed = 0;
};

// The fate of one connection: whether it is cut, and after how many payload
// (file) bytes. Each connection draws from its own stream, the seed mixed
// with its id, so a run is repeatable and a reconnect never repeats the cut
// before it. Matches the C version for the same seed and connection id.
class FaultPlan {
public:
    FaultPlan() noexcept = default;  // never cuts

    // `expected`: payload bytes the connection should carry; the cut point is
    // uniform below it.
    FaultPlan(const FaultConfig& config, std::uint64_t conn_id, std::uint64_t expected) noexcept;

    [[nodiscard]] bool armed() const noexcept { return armed_; }
    [[nodiscard]] std::uint64_t cut_at() const noexcept { return cut_at_; }

    // How many of `want` payload bytes may be sent before the cut.
    [[nodiscard]] std::size_t allow(std::size_t want) const noexcept;
    void sent(std::size_t n) noexcept { sent_ += n; }

private:
    bool armed_ = false;
    std::uint64_t cut_at_ = 0;
    std::uint64_t sent_ = 0;
};

}  // namespace axon
