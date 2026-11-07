#include "sys.hpp"

#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace execcontract {

std::optional<std::string> errno_name(int err) {
    if (err == 0) {
        return std::nullopt;
    }
    const char* name = strerrorname_np(err);  // glibc >= 2.32
    if (name == nullptr) {
        return std::nullopt;
    }
    return std::string(name);
}

std::string errno_message(int err) {
    if (err == 0) {
        return {};
    }
    return std::string(std::strerror(err));
}

UniqueFd& UniqueFd::operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
        reset();
        fd_ = other.release();
    }
    return *this;
}

int UniqueFd::release() noexcept {
    const int fd = fd_;
    fd_ = -1;
    return fd;
}

void UniqueFd::reset() noexcept {
    if (fd_ >= 0) {
        (void)::close(fd_);
        fd_ = -1;
    }
}

int UniqueFd::close_checked() noexcept {
    if (fd_ < 0) {
        return 0;
    }
    const int rc = ::close(fd_);
    const int err = rc == 0 ? 0 : errno;
    fd_ = -1;  // released even on EINTR; never retry
    return err;
}

DirStream::~DirStream() {
    if (dir_ != nullptr) {
        (void)::closedir(dir_);
    }
}

ReadResult read_bounded(int fd, std::size_t cap) {
    ReadResult result;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0) {
            if (errno == EINTR) {
                continue;  // read() has no side effects when interrupted before transfer
            }
            result.err = errno;
            return result;
        }
        if (n == 0) {
            return result;
        }
        const auto got = static_cast<std::size_t>(n);
        const std::size_t room = cap + 1 - result.data.size();
        result.data.append(buf, got < room ? got : room);
        if (result.data.size() > cap) {
            result.too_large = true;
            return result;
        }
    }
}

int write_all(int fd, std::string_view data) noexcept {
    while (!data.empty()) {
        const ssize_t n = ::write(fd, data.data(), data.size());
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return errno;
        }
        if (n == 0) {
            return EIO;
        }
        data.remove_prefix(static_cast<std::size_t>(n));
    }
    return 0;
}

}  // namespace execcontract
