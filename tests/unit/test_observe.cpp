#include "observe.hpp"
#include "test_support.hpp"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

using namespace execcontract;

namespace {

bool contains_fd(const FdInventory& inv, int fd) {
    return std::any_of(inv.entries.begin(), inv.entries.end(), [&](const FdEntry& e) { return e.fd == fd; });
}

std::optional<FdType> fstat_type(int fd) {
    FdInventory inv;
    inv.entries.push_back(FdEntry{fd, std::nullopt, std::nullopt});
    classify_fds(inv);
    return inv.entries[0].type;
}

}  // namespace

// ------------------------------------------------------------- scanner (seam: directory path)

TEST(FdScan, ParsesNumericEntriesFromDirectory) {
    test::TempDir dir;
    for (const char* name : {"0", "1", "17", "2147483647"}) {
        dir.write(name, "");
    }
    const FdInventory inv = scan_fd_dir(dir.path().c_str(), kMaxInventoryFds);
    ASSERT_TRUE(inv.complete) << inv.error->code;
    std::vector<int> fds;
    for (const FdEntry& e : inv.entries) fds.push_back(e.fd);
    EXPECT_EQ(fds, (std::vector<int>{0, 1, 17, 2147483647}));  // sorted
    EXPECT_GE(inv.scanner_fd, 0);
}

TEST(FdScan, ExcludesExactlyTheScannerDescriptor) {
    // Pre-create a name for every descriptor number the scanner could
    // plausibly receive. Only the actual dirfd() number may disappear.
    test::TempDir dir;
    for (int i = 0; i < 64; ++i) dir.write(std::to_string(i), "");
    const FdInventory inv = scan_fd_dir(dir.path().c_str(), kMaxInventoryFds);
    ASSERT_TRUE(inv.complete);
    ASSERT_GE(inv.scanner_fd, 0);
    ASSERT_LT(inv.scanner_fd, 64);
    EXPECT_EQ(inv.entries.size(), 63u);
    EXPECT_FALSE(contains_fd(inv, inv.scanner_fd));
}

TEST(FdScan, CapBoundary) {
    test::TempDir dir;
    // Names start above any descriptor this test process could hold, so the
    // scanner exclusion cannot remove one of them.
    for (std::size_t i = 0; i < kMaxInventoryFds + 1; ++i) dir.write(std::to_string(1000000 + i), "");
    const FdInventory over = scan_fd_dir(dir.path().c_str(), kMaxInventoryFds);
    EXPECT_FALSE(over.complete);
    ASSERT_TRUE(over.error);
    EXPECT_EQ(over.error->code, "inventory_capped");
    EXPECT_EQ(over.entries.size(), kMaxInventoryFds);  // partial evidence retained

    const FdInventory at = scan_fd_dir(dir.path().c_str(), kMaxInventoryFds + 1);
    EXPECT_TRUE(at.complete);
    EXPECT_EQ(at.entries.size(), kMaxInventoryFds + 1);
}

TEST(FdScan, UnparseableEntryFailsClosed) {
    for (const char* bad : {"abc", "01", "-1", "2147483648", "1x"}) {
        test::TempDir dir;
        dir.write("5", "");
        dir.write(bad, "");
        const FdInventory inv = scan_fd_dir(dir.path().c_str(), kMaxInventoryFds);
        EXPECT_FALSE(inv.complete) << bad;
        ASSERT_TRUE(inv.error) << bad;
        EXPECT_EQ(inv.error->code, "fd_entry_unparseable") << bad;
    }
}

TEST(FdScan, MissingDirectoryIsAnErrorNotAnEmptyInventory) {
    const FdInventory inv = scan_fd_dir("/nonexistent/execcontract/fd", kMaxInventoryFds);
    EXPECT_FALSE(inv.complete);
    EXPECT_TRUE(inv.entries.empty());
    EXPECT_EQ(inv.scanner_fd, -1);
    ASSERT_TRUE(inv.error);
    EXPECT_EQ(inv.error->code, "fd_enumeration_failed");
    EXPECT_EQ(inv.error->err, ENOENT);
}

TEST(FdScan, LiveProcfsSeesOwnDescriptorAndClosesScanner) {
    UniqueFd mine(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    ASSERT_TRUE(mine);
    const FdInventory inv = scan_fd_dir("/proc/self/fd", kMaxInventoryFds);
    ASSERT_TRUE(inv.complete);
    EXPECT_TRUE(contains_fd(inv, mine.get()));
    EXPECT_FALSE(contains_fd(inv, inv.scanner_fd));
    // The stream is closed on return: its number is free again.
    EXPECT_EQ(::fcntl(inv.scanner_fd, F_GETFD), -1);
    EXPECT_EQ(errno, EBADF);
}

TEST(FdScan, ScannerTakesLowestFreeNumberButDoesNotReportIt) {
    // Free a low number, then scan: dirfd() reuses it, and it must not appear.
    UniqueFd probe(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    ASSERT_TRUE(probe);
    const int freed = probe.get();
    probe.reset();
    const FdInventory inv = scan_fd_dir("/proc/self/fd", kMaxInventoryFds);
    ASSERT_TRUE(inv.complete);
    EXPECT_EQ(inv.scanner_fd, freed);
    EXPECT_FALSE(contains_fd(inv, freed));
}

TEST(FdScan, NotBoundedBySoftNofileLimit) {
    struct rlimit saved{};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &saved), 0);
    if (saved.rlim_max != RLIM_INFINITY && saved.rlim_max < 600) {
        GTEST_SKIP() << "hard RLIMIT_NOFILE < 600";
    }
    UniqueFd high(::fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 500));
    ASSERT_TRUE(high);
    struct rlimit lowered = saved;
    lowered.rlim_cur = 64;  // soft only; hard unchanged so it can be restored
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &lowered), 0);
    const FdInventory inv = scan_fd_dir("/proc/self/fd", kMaxInventoryFds);
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &saved), 0);
    ASSERT_TRUE(inv.complete);
    EXPECT_TRUE(contains_fd(inv, high.get()));
}

// ------------------------------------------------------------- types

TEST(FdType, ClassifyModeCoversAllCategories) {
    EXPECT_EQ(classify_mode(S_IFREG | 0644), FdType::regular);
    EXPECT_EQ(classify_mode(S_IFDIR | 0755), FdType::directory);
    EXPECT_EQ(classify_mode(S_IFIFO), FdType::pipe);
    EXPECT_EQ(classify_mode(S_IFSOCK), FdType::socket);
    EXPECT_EQ(classify_mode(S_IFCHR), FdType::char_device);
    EXPECT_EQ(classify_mode(S_IFBLK), FdType::block_device);
    EXPECT_EQ(classify_mode(S_IFLNK), FdType::other);
    EXPECT_EQ(classify_mode(0), FdType::other);
}

TEST(FdType, FstatOnRealObjects) {
    test::TempDir dir;
    const auto file = dir.write("f", "x");
    UniqueFd reg(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
    UniqueFd d(::open(dir.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    UniqueFd null(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    int p[2];
    ASSERT_EQ(::pipe2(p, O_CLOEXEC), 0);
    UniqueFd pr(p[0]), pw(p[1]);
    int sv[2];
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv), 0);
    UniqueFd s0(sv[0]), s1(sv[1]);

    EXPECT_EQ(fstat_type(reg.get()), FdType::regular);
    EXPECT_EQ(fstat_type(d.get()), FdType::directory);
    EXPECT_EQ(fstat_type(null.get()), FdType::char_device);
    EXPECT_EQ(fstat_type(pr.get()), FdType::pipe);
    EXPECT_EQ(fstat_type(s0.get()), FdType::socket);
}

TEST(FdType, DeletedFileStillClassifiedThroughDescriptor) {
    test::TempDir dir;
    const auto file = dir.write("gone", "x");
    UniqueFd fd(::open(file.c_str(), O_RDONLY | O_CLOEXEC));
    ASSERT_TRUE(fd);
    std::filesystem::remove(file);
    EXPECT_EQ(fstat_type(fd.get()), FdType::regular);
}

TEST(FdType, ClosedDescriptorYieldsTypeError) {
    UniqueFd fd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    const int n = fd.get();
    fd.reset();
    FdInventory inv;
    inv.entries.push_back(FdEntry{n, std::nullopt, std::nullopt});
    classify_fds(inv);
    EXPECT_FALSE(inv.entries[0].type);
    ASSERT_TRUE(inv.entries[0].type_error);
    EXPECT_EQ(inv.entries[0].type_error->code, "fd_type_unavailable");
    EXPECT_EQ(inv.entries[0].type_error->err, EBADF);
}

// ------------------------------------------------------------- umask (seam: status path)

TEST(Umask, ParsesProcStatusFormat) {
    EXPECT_EQ(parse_umask_status("Name:\tx\nUmask:\t0022\nState:\tR\n").value, std::optional<unsigned>(022));
    EXPECT_EQ(parse_umask_status("Umask:\t0000\n").value, std::optional<unsigned>(0));
    EXPECT_EQ(parse_umask_status("Umask:\t0777").value,
              std::optional<unsigned>(0777));  // no trailing newline
    EXPECT_EQ(parse_umask_status("Umask:  027 \n").value, std::optional<unsigned>(027));
}

TEST(Umask, MissingOrMalformedIsAnError) {
    EXPECT_EQ(parse_umask_status("").error->code, "umask_unavailable");
    EXPECT_EQ(parse_umask_status("Name:\tx\nXUmask:\t0022\n").error->code, "umask_unavailable");
    for (const char* bad : {"Umask:\t\n", "Umask:\t0088\n", "Umask:\t00022\n", "Umask:\t0022x\n",
                            "Umask:\t-022\n", "Umask:\t1000\n", "Umask:\t0022\nUmask:\t0022\n"}) {
        const auto r = parse_umask_status(bad);
        EXPECT_FALSE(r.value) << bad;
        ASSERT_TRUE(r.error) << bad;
        EXPECT_EQ(r.error->code, "umask_malformed") << bad;
    }
}

TEST(Umask, ReadFromFileAndMissingFile) {
    test::TempDir dir;
    EXPECT_EQ(read_umask(dir.write("status", "Umask:\t0077\n").c_str()).value, std::optional<unsigned>(077));
    const auto missing = read_umask((dir.path() / "nope").c_str());
    ASSERT_TRUE(missing.error);
    EXPECT_EQ(missing.error->code, "umask_unavailable");
    EXPECT_EQ(missing.error->err, ENOENT);
    const auto huge = read_umask(dir.write("big", std::string(kMaxStatusBytes + 1, 'x')).c_str());
    ASSERT_TRUE(huge.error);
    EXPECT_EQ(huge.error->code, "umask_unavailable");
}

TEST(Umask, LiveObservationMatchesAndDoesNotChangeIt) {
    const mode_t original = ::umask(027);  // test-process setup only
    const auto obs = read_umask("/proc/self/status");
    const mode_t after = ::umask(original);
    ASSERT_TRUE(obs.value) << obs.error->code;
    EXPECT_EQ(*obs.value, 027u);
    EXPECT_EQ(after, 027u);  // observation did not modify the mask
}

// ------------------------------------------------------------- limits / cwd

TEST(Rlimit, MatchesGetrlimitAndMapsInfinity) {
    struct rlimit rl{};
    ASSERT_EQ(::getrlimit(RLIMIT_CORE, &rl), 0);
    const auto obs = observe_rlimit(RLIMIT_CORE);
    ASSERT_TRUE(obs.value);
    EXPECT_EQ(obs.value->soft.unlimited, rl.rlim_cur == RLIM_INFINITY);
    EXPECT_EQ(obs.value->hard.unlimited, rl.rlim_max == RLIM_INFINITY);
    if (rl.rlim_cur != RLIM_INFINITY) {
        EXPECT_EQ(obs.value->soft.value, rl.rlim_cur);
    }
}

TEST(Rlimit, InvalidResourceIsAnError) {
    const auto obs = observe_rlimit(-1);
    ASSERT_TRUE(obs.error);
    EXPECT_EQ(obs.error->code, "rlimit_unavailable");
    EXPECT_EQ(obs.error->err, EINVAL);
}

class CwdTest : public ::testing::Test {
   protected:
    void SetUp() override { saved_ = UniqueFd(::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC)); }
    void TearDown() override { ASSERT_EQ(::fchdir(saved_.get()), 0); }
    UniqueFd saved_;
};

TEST_F(CwdTest, PhysicalPathIgnoresPwdAndSymlinks) {
    test::TempDir dir;
    const auto real = dir.path() / "real dir";
    std::filesystem::create_directory(real);
    std::filesystem::create_directory_symlink(real, dir.path() / "link");
    ASSERT_EQ(::chdir((dir.path() / "link").c_str()), 0);
    ::setenv("PWD", (dir.path() / "link").c_str(), 1);
    const auto obs = observe_cwd(kMaxCwdBytes);
    ASSERT_TRUE(obs.value);
    EXPECT_EQ(*obs.value, std::filesystem::canonical(real).string());
}

TEST_F(CwdTest, LongPathGrowsBuffer) {
    test::TempDir dir;
    std::filesystem::path p = dir.path();
    for (int i = 0; i < 6; ++i)
        p /= std::string(100, static_cast<char>('a' + i));  // > 256-byte initial buffer
    std::filesystem::create_directories(p);
    ASSERT_EQ(::chdir(p.c_str()), 0);
    const auto obs = observe_cwd(kMaxCwdBytes);
    ASSERT_TRUE(obs.value);
    EXPECT_EQ(*obs.value, std::filesystem::canonical(p).string());
    const auto capped = observe_cwd(64);
    ASSERT_TRUE(capped.error);
    EXPECT_EQ(capped.error->code, "cwd_too_long");
}

TEST_F(CwdTest, DeletedDirectoryIsAnError) {
    test::TempDir dir;
    const auto sub = dir.path() / "doomed";
    std::filesystem::create_directory(sub);
    ASSERT_EQ(::chdir(sub.c_str()), 0);
    ASSERT_EQ(::rmdir(sub.c_str()), 0);
    const auto obs = observe_cwd(kMaxCwdBytes);
    ASSERT_TRUE(obs.error);
    EXPECT_EQ(obs.error->code, "cwd_deleted");
    EXPECT_EQ(obs.error->err, ENOENT);
}

TEST_F(CwdTest, NonUtf8PathIsAnErrorWithBoundedDiagnostic) {
    test::TempDir dir;
    const auto sub = dir.path() / "bad\xFFname";
    std::filesystem::create_directory(sub);
    ASSERT_EQ(::chdir(sub.c_str()), 0);
    const auto obs = observe_cwd(kMaxCwdBytes);
    ASSERT_TRUE(obs.error);
    EXPECT_EQ(obs.error->code, "cwd_not_utf8");
    EXPECT_EQ(obs.error->detail.find('\xFF'), std::string::npos);  // raw bytes never copied
    EXPECT_LT(obs.error->detail.size(), 200u);
}
