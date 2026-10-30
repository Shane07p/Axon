#include <fcntl.h>
#include <unistd.h>

#include <utility>

#include <gtest/gtest.h>

#include "axon/fd.hpp"
#include "test_util.hpp"

namespace {

using axon::Fd;

class FdTest : public axon::test::NoFdLeak {};

int open_dev_null() { return ::open("/dev/null", O_RDONLY | O_CLOEXEC); }
bool is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

TEST_F(FdTest, DefaultIsEmpty) {
    const Fd fd;
    EXPECT_FALSE(fd);
    EXPECT_EQ(fd.get(), -1);
}

TEST_F(FdTest, ClosesWhenDestroyed) {
    int raw = -1;
    {
        const Fd fd{open_dev_null()};
        raw = fd.get();
        ASSERT_TRUE(is_open(raw));
    }
    EXPECT_FALSE(is_open(raw));
}

TEST_F(FdTest, MoveConstructTransfersOwnership) {
    Fd a{open_dev_null()};
    const int raw = a.get();
    const Fd b{std::move(a)};
    EXPECT_FALSE(a);
    EXPECT_EQ(b.get(), raw);
    EXPECT_TRUE(is_open(raw));
}

TEST_F(FdTest, MoveAssignClosesPreviousTarget) {
    Fd a{open_dev_null()};
    Fd b{open_dev_null()};
    const int raw_a = a.get();
    const int old_b = b.get();
    b = std::move(a);
    EXPECT_FALSE(is_open(old_b));
    EXPECT_EQ(b.get(), raw_a);
    EXPECT_TRUE(is_open(raw_a));
}

TEST_F(FdTest, SelfMoveAssignKeepsDescriptor) {
    Fd a{open_dev_null()};
    const int raw = a.get();
    Fd& alias = a;
    a = std::move(alias);
    EXPECT_EQ(a.get(), raw);
    EXPECT_TRUE(is_open(raw));
}

TEST_F(FdTest, ReleaseGivesUpOwnership) {
    int raw = -1;
    {
        Fd fd{open_dev_null()};
        raw = fd.release();
        EXPECT_FALSE(fd);
    }
    EXPECT_TRUE(is_open(raw));
    ::close(raw);
}

TEST_F(FdTest, ResetClosesOldDescriptor) {
    Fd fd{open_dev_null()};
    const int old = fd.get();
    fd.reset(open_dev_null());
    EXPECT_FALSE(is_open(old));
    EXPECT_TRUE(fd);
    fd.reset();
    EXPECT_FALSE(fd);
}

}  // namespace
