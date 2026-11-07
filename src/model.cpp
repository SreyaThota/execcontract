#include "model.hpp"

#include <cstdio>

namespace execcontract {

const char* to_string(FdType type) noexcept {
    switch (type) {
        case FdType::regular: return "regular";
        case FdType::directory: return "directory";
        case FdType::pipe: return "pipe";
        case FdType::socket: return "socket";
        case FdType::char_device: return "char_device";
        case FdType::block_device: return "block_device";
        case FdType::other: return "other";
    }
    return "other";
}

std::optional<FdType> fd_type_from_string(std::string_view text) noexcept {
    for (FdType t : {FdType::regular, FdType::directory, FdType::pipe, FdType::socket, FdType::char_device,
                     FdType::block_device, FdType::other}) {
        if (text == to_string(t)) {
            return t;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> first_invalid_utf8(std::string_view text) noexcept {
    const auto* s = reinterpret_cast<const unsigned char*>(text.data());
    const std::size_t n = text.size();
    std::size_t i = 0;
    while (i < n) {
        const unsigned char c = s[i];
        if (c < 0x80) {
            ++i;
            continue;
        }
        std::size_t len = 0;
        unsigned char lo = 0x80;
        unsigned char hi = 0xBF;  // allowed range of the second byte
        if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
        } else if (c == 0xE0) {
            len = 3;
            lo = 0xA0;  // reject overlong encodings
        } else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) {
            len = 3;
        } else if (c == 0xED) {
            len = 3;
            hi = 0x9F;  // reject UTF-16 surrogates
        } else if (c == 0xF0) {
            len = 4;
            lo = 0x90;  // reject overlong encodings
        } else if (c >= 0xF1 && c <= 0xF3) {
            len = 4;
        } else if (c == 0xF4) {
            len = 4;
            hi = 0x8F;  // reject code points above U+10FFFF
        } else {
            return i;  // continuation byte, 0xC0/0xC1 or 0xF5..0xFF as lead byte
        }
        if (i + len > n || s[i + 1] < lo || s[i + 1] > hi) {
            return i;
        }
        for (std::size_t k = 2; k < len; ++k) {
            if (s[i + k] < 0x80 || s[i + k] > 0xBF) {
                return i;
            }
        }
        i += len;
    }
    return std::nullopt;
}

std::string format_umask(unsigned mask) {
    char buf[8];
    (void)std::snprintf(buf, sizeof buf, "%04o", mask & 0777u);  // always fits: <= 4 digits
    return buf;
}

}  // namespace execcontract
