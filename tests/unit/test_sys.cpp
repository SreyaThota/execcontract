#include "sys.hpp"
#include "test_support.hpp"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <gtest/gtest.h>

using namespace execcontract;

TEST(UniqueFd, OwnsAndClosesOnce) {
    int raw = -1;
    {
        UniqueFd fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
        ASSERT_TRUE(fd);
        raw = fd.get();
        UniqueFd moved(std::move(fd));
        EXPECT_FALSE(fd);  // NOLINT(bugprone-use-after-move): verifying moved-from state
        EXPECT_EQ(moved.get(), raw);
    }
    EXPECT_EQ(::fcntl(raw, F_GETFD), -1);
}

TEST(UniqueFd, CloseCheckedReleasesEvenOnError) {
    UniqueFd fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    const int raw = fd.get();
    ASSERT_EQ(::close(raw), 0);  // simulate an already-invalid descriptor
    EXPECT_EQ(fd.close_checked(), EBADF);
    EXPECT_FALSE(fd);
    EXPECT_EQ(fd.close_checked(), 0);  // idempotent, no second close()
}

TEST(ErrnoName, SymbolicNames) {
    EXPECT_EQ(errno_name(ENOENT), std::optional<std::string>("ENOENT"));
    EXPECT_EQ(errno_name(EPIPE), std::optional<std::string>("EPIPE"));
    EXPECT_FALSE(errno_name(0));
    EXPECT_FALSE(errno_name(99999));
}

TEST(ReadBounded, CapBoundary) {
    test::TempDir dir;
    const auto at = dir.write("at", std::string(100, 'a'));
    const auto over = dir.write("over", std::string(101, 'a'));
    UniqueFd a(::open(at.c_str(), O_RDONLY | O_CLOEXEC));
    UniqueFd b(::open(over.c_str(), O_RDONLY | O_CLOEXEC));
    const ReadResult ra = read_bounded(a.get(), 100);
    EXPECT_FALSE(ra.too_large);
    EXPECT_EQ(ra.data.size(), 100u);
    const ReadResult rb = read_bounded(b.get(), 100);
    EXPECT_TRUE(rb.too_large);
}

TEST(ReadBounded, ReadErrorReported) {
    test::TempDir dir;
    UniqueFd d(::open(dir.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    EXPECT_EQ(read_bounded(d.get(), 10).err, EISDIR);
}

TEST(WriteAll, DevFullReportsEnospc) {
    UniqueFd full(::open("/dev/full", O_WRONLY | O_CLOEXEC));
    if (!full)
        GTEST_SKIP() << "/dev/full unavailable";
    EXPECT_EQ(write_all(full.get(), "report"), ENOSPC);
}

TEST(WriteAll, BrokenPipeReportsEpipeWhenSigpipeIgnored) {
    struct sigaction ign{}, old{};
    ign.sa_handler = SIG_IGN;
    ASSERT_EQ(::sigaction(SIGPIPE, &ign, &old), 0);
    int p[2];
    ASSERT_EQ(::pipe2(p, O_CLOEXEC), 0);
    UniqueFd r(p[0]), w(p[1]);
    r.reset();
    EXPECT_EQ(write_all(w.get(), "report"), EPIPE);
    ASSERT_EQ(::sigaction(SIGPIPE, &old, nullptr), 0);
}

TEST(WriteAll, ClosedDescriptorReportsEbadf) {
    UniqueFd fd(::open("/dev/null", O_WRONLY | O_CLOEXEC));
    const int raw = fd.get();
    fd.reset();
    EXPECT_EQ(write_all(raw, "x"), EBADF);
}

TEST(WriteAll, LargeWriteThroughPipeIsComplete) {
    int p[2];
    ASSERT_EQ(::pipe2(p, O_CLOEXEC), 0);
    UniqueFd r(p[0]), w(p[1]);
    // Larger than a default pipe buffer would need a reader; stay below 64 KiB.
    const std::string data(60000, 'z');
    ASSERT_EQ(write_all(w.get(), data), 0);
    w.reset();
    const ReadResult got = read_bounded(r.get(), 100000);
    EXPECT_EQ(got.data, data);
}
