// Small ownership wrappers and syscall helpers.
//
// Convention used throughout the code base:
//   * UniqueFd / DirStream  -> descriptors ExecContract opened and therefore owns.
//   * plain `int` fd        -> borrowed descriptor (inherited, e.g. STDOUT_FILENO).
//                              Borrowed descriptors are never closed, read,
//                              seeked or have their flags changed.
#pragma once

#include <dirent.h>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace execcontract {

// A failed observation or I/O operation. `code` is a stable identifier that
// tests and callers may rely on; `err` is the errno value (0 if not a syscall
// failure); `detail` is human-readable context and is not stable.
struct SysError {
    std::string code;
    int err = 0;
    std::string detail;
};

// Symbolic errno name ("ENOENT") or nullopt when unknown or err == 0.
std::optional<std::string> errno_name(int err);
// strerror text; locale-dependent, for diagnostics only.
std::string errno_message(int err);

// Owned file descriptor. Closing never retries: on Linux the descriptor is
// released even when close() reports EINTR, and a retry could close a
// descriptor that another open() has meanwhile been given.
class UniqueFd {
   public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept;

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }
    int release() noexcept;
    // Close and discard any error (destructor path).
    void reset() noexcept;
    // Close and report the error (0 on success). The descriptor is released
    // in all cases.
    int close_checked() noexcept;

   private:
    int fd_ = -1;
};

// Owned directory stream (opendir/closedir).
class DirStream {
   public:
    explicit DirStream(DIR* dir) noexcept : dir_(dir) {}
    ~DirStream();
    DirStream(const DirStream&) = delete;
    DirStream& operator=(const DirStream&) = delete;

    DIR* get() const noexcept { return dir_; }

   private:
    DIR* dir_;
};

// Read the whole file behind `fd` (owned or borrowed: this function only
// reads) up to `cap` bytes. Returns the data, or an error. If the content is
// larger than `cap`, `too_large` is set and the returned data is truncated to
// cap + 1 bytes; callers must treat that as an error, never as content.
struct ReadResult {
    std::string data;
    bool too_large = false;
    int err = 0;  // errno of a failed read(), 0 on success
};
ReadResult read_bounded(int fd, std::size_t cap);

// Write all bytes, retrying on EINTR and short writes. Returns 0 on success or
// the errno of the failure (EIO if write() unexpectedly returned 0).
int write_all(int fd, std::string_view data) noexcept;

}  // namespace execcontract
