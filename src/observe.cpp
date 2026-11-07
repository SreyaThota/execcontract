#include "observe.hpp"

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>

namespace execcontract {
namespace {

// Parse a /proc/self/fd entry name: canonical decimal, 0..INT_MAX.
bool parse_fd_name(std::string_view name, int& out) noexcept {
    if (name.empty() || name.size() > 10 || (name.size() > 1 && name[0] == '0')) {
        return false;
    }
    long long value = 0;
    for (const char c : name) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    if (value > INT_MAX) {
        return false;
    }
    out = static_cast<int>(value);
    return true;
}

SysError sys_error(const char* code, int err, std::string detail) {
    return SysError{code, err, std::move(detail)};
}

}  // namespace

FdInventory scan_fd_dir(const char* dir_path, std::size_t cap, ReaddirFn next_entry) {
    FdInventory inv;
    DIR* raw = ::opendir(dir_path);
    if (raw == nullptr) {
        inv.error = sys_error("fd_enumeration_failed", errno, std::string("opendir ") + dir_path);
        return inv;
    }
    const DirStream dir(raw);  // closed when this function returns, before any other observation

    // The stream itself occupies the lowest free descriptor number (often 3).
    // Exclude exactly that number - never a hard-coded one - so the scanner
    // cannot satisfy a requirement for a descriptor that was not inherited.
    inv.scanner_fd = ::dirfd(raw);
    if (inv.scanner_fd < 0) {
        inv.error = sys_error("fd_enumeration_failed", errno, "dirfd");
        return inv;
    }

    std::vector<FdEntry> found;
    for (;;) {
        // readdir() returns NULL both at end-of-directory and on error; only
        // errno distinguishes them, so it is cleared immediately before the
        // call and read immediately after, with no other call in between.
        errno = 0;
        const dirent* entry = next_entry(raw);
        if (entry == nullptr) {
            const int err = errno;
            if (err != 0) {
                inv.entries = std::move(found);
                inv.error = sys_error("fd_enumeration_failed", err, "readdir");
                return inv;
            }
            break;
        }
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..") {
            continue;
        }
        int fd = -1;
        if (!parse_fd_name(name, fd)) {
            inv.entries = std::move(found);
            inv.error = sys_error("fd_entry_unparseable", 0,
                                  "unexpected entry name of length " + std::to_string(name.size()));
            return inv;
        }
        if (fd == inv.scanner_fd) {
            continue;
        }
        if (found.size() == cap) {
            inv.entries = std::move(found);
            inv.error =
                sys_error("inventory_capped", 0, "more than " + std::to_string(cap) + " open descriptors");
            return inv;
        }
        found.push_back(FdEntry{fd, std::nullopt, std::nullopt});
    }
    std::sort(found.begin(), found.end(), [](const FdEntry& a, const FdEntry& b) { return a.fd < b.fd; });
    inv.entries = std::move(found);
    inv.complete = true;
    return inv;
}

FdType classify_mode(mode_t mode) noexcept {
    switch (mode & S_IFMT) {
        case S_IFREG: return FdType::regular;
        case S_IFDIR: return FdType::directory;
        case S_IFIFO: return FdType::pipe;
        case S_IFSOCK: return FdType::socket;
        case S_IFCHR: return FdType::char_device;
        case S_IFBLK: return FdType::block_device;
        default: return FdType::other;  // e.g. S_IFLNK via O_PATH|O_NOFOLLOW, anon inodes
    }
}

void classify_fds(FdInventory& inventory) {
    for (FdEntry& e : inventory.entries) {
        // fstat on the descriptor itself classifies the open object, including
        // deleted files and anonymous pipes; procfs link text is only a label.
        struct stat st{};
        if (::fstat(e.fd, &st) == 0) {
            e.type = classify_mode(st.st_mode);
        } else {
            e.type_error = sys_error("fd_type_unavailable", errno, "fstat");
        }
    }
}

Observed<unsigned> parse_umask_status(std::string_view text) {
    constexpr std::string_view key = "Umask:";
    std::size_t found_at = std::string_view::npos;
    std::size_t pos = 0;
    while (pos <= text.size()) {
        const std::size_t eol = std::min(text.find('\n', pos), text.size());
        const std::string_view line = text.substr(pos, eol - pos);
        if (line.substr(0, key.size()) == key) {
            if (found_at != std::string_view::npos) {
                return Observed<unsigned>::fail(sys_error("umask_malformed", 0, "duplicate Umask field"));
            }
            found_at = pos;
        }
        pos = eol + 1;
    }
    if (found_at == std::string_view::npos) {
        return Observed<unsigned>::fail(sys_error("umask_unavailable", 0, "no Umask field"));
    }
    std::string_view rest = text.substr(found_at + key.size());
    rest = rest.substr(0, std::min(rest.find('\n'), rest.size()));
    std::size_t i = 0;
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) {
        ++i;
    }
    const std::size_t digits_begin = i;
    unsigned value = 0;
    while (i < rest.size() && rest[i] >= '0' && rest[i] <= '7') {
        value = value * 8 + static_cast<unsigned>(rest[i] - '0');
        ++i;
        if (i - digits_begin > 4) {
            break;
        }
    }
    const std::size_t ndigits = i - digits_begin;
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) {
        ++i;
    }
    if (ndigits == 0 || ndigits > 4 || i != rest.size() || value > 0777) {
        return Observed<unsigned>::fail(sys_error("umask_malformed", 0, "unparseable Umask value"));
    }
    return Observed<unsigned>::ok(value);
}

Observed<unsigned> read_umask(const char* status_path) {
    // The umask cannot be read through umask(2) without temporarily changing
    // it; /proc/self/status (Linux >= 4.7) exposes it read-only.
    const UniqueFd fd(::open(status_path, O_RDONLY | O_CLOEXEC | O_NOCTTY));
    if (!fd) {
        return Observed<unsigned>::fail(
            sys_error("umask_unavailable", errno, std::string("open ") + status_path));
    }
    const ReadResult r = read_bounded(fd.get(), kMaxStatusBytes);
    if (r.err != 0) {
        return Observed<unsigned>::fail(sys_error("umask_unavailable", r.err, "read status"));
    }
    if (r.too_large) {
        return Observed<unsigned>::fail(sys_error("umask_unavailable", 0, "status file exceeds read cap"));
    }
    return parse_umask_status(r.data);
}

Observed<Limits> observe_rlimit(int resource) {
    struct rlimit rl{};
    if (::getrlimit(resource, &rl) != 0) {
        return Observed<Limits>::fail(sys_error("rlimit_unavailable", errno, "getrlimit"));
    }
    auto convert = [](rlim_t v) {
        return v == RLIM_INFINITY ? LimitValue::infinity()
                                  : LimitValue::finite(static_cast<std::uint64_t>(v));
    };
    return Observed<Limits>::ok(Limits{convert(rl.rlim_cur), convert(rl.rlim_max)});
}

Observed<std::string> observe_cwd(std::size_t cap) {
    std::string buf(std::min<std::size_t>(256, cap), '\0');
    for (;;) {
        if (::getcwd(buf.data(), buf.size()) != nullptr) {
            buf.resize(std::strlen(buf.c_str()));
            break;
        }
        const int err = errno;
        if (err == ERANGE && buf.size() < cap) {
            buf.resize(std::min(buf.size() * 2, cap));
            continue;
        }
        if (err == ERANGE) {
            return Observed<std::string>::fail(sys_error("cwd_too_long", err, "getcwd"));
        }
        if (err == ENOENT) {
            return Observed<std::string>::fail(sys_error("cwd_deleted", err, "getcwd: directory unlinked"));
        }
        return Observed<std::string>::fail(sys_error("cwd_unavailable", err, "getcwd"));
    }
    // Older glibc could return "(unreachable)/..." for directories outside the
    // process's root; never treat that as a path.
    if (buf.empty() || buf[0] != '/') {
        return Observed<std::string>::fail(
            sys_error("cwd_unreachable", 0, "getcwd returned a non-absolute path"));
    }
    if (const auto bad = first_invalid_utf8(buf)) {
        return Observed<std::string>::fail(sys_error(
            "cwd_not_utf8", 0,
            "observed path of " + std::to_string(buf.size()) +
                " bytes is not valid UTF-8 (first invalid byte at offset " + std::to_string(*bad) + ")"));
    }
    return Observed<std::string>::ok(std::move(buf));
}

Snapshot collect_snapshot() {
    Snapshot s;
    // 1. Descriptor inventory. Must run before ExecContract opens anything.
    s.fds = scan_fd_dir("/proc/self/fd", kMaxInventoryFds);
    classify_fds(s.fds);
    // 2. The remaining observations open at most a transient, owned
    //    descriptor (status file) that is closed before returning.
    s.umask = read_umask("/proc/self/status");
    s.rlimit_nofile = observe_rlimit(RLIMIT_NOFILE);
    s.rlimit_core = observe_rlimit(RLIMIT_CORE);
    s.cwd = observe_cwd(kMaxCwdBytes);
    return s;
}

}  // namespace execcontract
