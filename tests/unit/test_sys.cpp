#include "sys.hpp"
#include "test_support.hpp"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <thread>

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

TEST(WriteAll, LargeWriteThroughPipeIsCompleteWithConcurrentReader) {
    // Pipe capacity is not guaranteed (pipe(7)), so the test must not rely on
    // the data fitting. A concurrent reader drains the 1 MiB payload.
    // This checks complete delivery, not deterministic short-write or EINTR coverage.
    struct sigaction ign{}, old{};
    ign.sa_handler = SIG_IGN;
    ASSERT_EQ(::sigaction(SIGPIPE, &ign, &old), 0);  // a dead reader -> EPIPE, not a killed test
    int p[2];
    ASSERT_EQ(::pipe2(p, O_CLOEXEC), 0);
    UniqueFd r(p[0]), w(p[1]);
    const std::string data(std::size_t{1} << 20, 'z');

    ReadResult got;
    std::thread reader([&] {
        got = read_bounded(r.get(), data.size() + 1);
        r.reset();  // if the reader stops early, the writer fails instead of blocking
    });
    const int err = write_all(w.get(), data);
    w.reset();  // EOF for the reader, on success and on failure
    reader.join();
    ASSERT_EQ(::sigaction(SIGPIPE, &old, nullptr), 0);

    EXPECT_EQ(err, 0);
    EXPECT_EQ(got.err, 0);
    EXPECT_FALSE(got.too_large);
    EXPECT_EQ(got.data.size(), data.size());
    EXPECT_TRUE(got.data == data);  // avoid printing 1 MiB on failure
}
