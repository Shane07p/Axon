#include <array>
#include <cstdint>

#include <gtest/gtest.h>

#include "axon/fault/fault.hpp"

namespace {

using axon::FaultConfig;
using axon::FaultPlan;
using axon::SplitMix64;

// Reference values printed by the C version (CNProject common/fault.c).
static_assert([] {
    SplitMix64 r{0};
    return r.next() == 0xE220A8397B1DCDAFULL && r.next() == 0x6E789E6AA1B965F4ULL &&
           r.next() == 0x06C45D188009454FULL;
}());

TEST(FaultPlan, MatchesTheCVersion) {
    struct Expected {
        bool armed;
        std::uint64_t cut_at;
    };
    // fault_plan_make with p = 0.5, seed 42, expected 1000000, connections 1..6.
    constexpr std::array<Expected, 6> c_version{
        {{false, 0}, {false, 0}, {true, 879447}, {false, 0}, {false, 0}, {false, 0}}};
    for (std::uint64_t conn = 1; conn <= c_version.size(); ++conn) {
        const FaultPlan plan{{.probability = 0.5, .seed = 42}, conn, 1'000'000};
        EXPECT_EQ(plan.armed(), c_version[conn - 1].armed) << "connection " << conn;
        EXPECT_EQ(plan.cut_at(), c_version[conn - 1].cut_at) << "connection " << conn;
    }
}

TEST(FaultPlan, ProbabilityZeroNeverCuts) {
    for (std::uint64_t conn = 0; conn < 1000; ++conn) {
        EXPECT_FALSE(FaultPlan({.probability = 0, .seed = 1}, conn, 1000).armed());
    }
}

TEST(FaultPlan, ProbabilityOneAlwaysCutsBeforeTheEnd) {
    for (std::uint64_t conn = 0; conn < 1000; ++conn) {
        const FaultPlan plan{{.probability = 1, .seed = 1}, conn, 1000};
        EXPECT_TRUE(plan.armed());
        EXPECT_LT(plan.cut_at(), 1000u);
    }
}

TEST(FaultPlan, NothingToSendMeansNoCut) {
    EXPECT_FALSE(FaultPlan({.probability = 1, .seed = 1}, 1, 0).armed());
}

TEST(FaultPlan, SameSeedSameFateDifferentConnectionsDiffer) {
    const FaultConfig config{.probability = 1, .seed = 5};
    EXPECT_EQ(FaultPlan(config, 9, 1 << 30).cut_at(), FaultPlan(config, 9, 1 << 30).cut_at());
    EXPECT_NE(FaultPlan(config, 9, 1 << 30).cut_at(), FaultPlan(config, 10, 1 << 30).cut_at());
}

TEST(FaultPlan, RateAndCutPointsLookUniform) {
    constexpr int n = 4000;
    constexpr std::uint64_t expected = 1'000'000;
    int armed = 0;
    double cut_fraction_sum = 0;
    for (std::uint64_t conn = 1; conn <= n; ++conn) {
        const FaultPlan plan{{.probability = 0.3, .seed = 99}, conn, expected};
        if (plan.armed()) {
            ++armed;
            cut_fraction_sum += static_cast<double>(plan.cut_at()) / expected;
        }
    }
    EXPECT_NEAR(static_cast<double>(armed) / n, 0.3, 0.03);
    EXPECT_NEAR(cut_fraction_sum / armed, 0.5, 0.03);
}

TEST(FaultPlan, AllowCountsDownToTheCut) {
    FaultPlan plan{{.probability = 1, .seed = 42}, 3, 1'000'000};
    const std::uint64_t k = plan.cut_at();
    ASSERT_GT(k, 10u);
    EXPECT_EQ(plan.allow(k + 10), k);
    plan.sent(k - 5);
    EXPECT_EQ(plan.allow(64), 5u);
    plan.sent(5);
    EXPECT_EQ(plan.allow(1), 0u);

    const FaultPlan never;
    EXPECT_EQ(never.allow(12345), 12345u);
}

}  // namespace
