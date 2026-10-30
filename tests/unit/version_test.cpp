#include <gtest/gtest.h>

#include "axon/version.hpp"

TEST(Version, IsMajorMinorPatch) {
    const auto v = axon::version();
    ASSERT_FALSE(v.empty());
    int dots = 0;
    for (const char c : v) {
        if (c == '.') {
            ++dots;
        } else {
            EXPECT_TRUE(c >= '0' && c <= '9') << "unexpected char '" << c << "' in " << v;
        }
    }
    EXPECT_EQ(dots, 2) << v;
}
