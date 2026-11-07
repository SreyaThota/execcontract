#include "model.hpp"

#include <gtest/gtest.h>

using namespace execcontract;

TEST(Utf8, AcceptsAsciiAndMultibyte) {
    EXPECT_FALSE(first_invalid_utf8(""));
    EXPECT_FALSE(first_invalid_utf8("/srv/app with space"));
    EXPECT_FALSE(first_invalid_utf8("/tmp/\xC3\xA9t\xC3\xA9"));  // é
    EXPECT_FALSE(first_invalid_utf8("\xE2\x82\xAC"));            // €
    EXPECT_FALSE(first_invalid_utf8("\xF0\x9F\x98\x80"));        // U+1F600
    EXPECT_FALSE(first_invalid_utf8("\xF4\x8F\xBF\xBF"));        // U+10FFFF
}

TEST(Utf8, RejectsInvalidSequencesWithOffset) {
    EXPECT_EQ(first_invalid_utf8("ab\xFF"), 2u);            // invalid lead byte
    EXPECT_EQ(first_invalid_utf8("a\x80"), 1u);             // stray continuation
    EXPECT_EQ(first_invalid_utf8("\xC0\xAF"), 0u);          // overlong '/'
    EXPECT_EQ(first_invalid_utf8("\xE0\x80\xAF"), 0u);      // overlong 3-byte
    EXPECT_EQ(first_invalid_utf8("\xED\xA0\x80"), 0u);      // surrogate U+D800
    EXPECT_EQ(first_invalid_utf8("\xF4\x90\x80\x80"), 0u);  // > U+10FFFF
    EXPECT_EQ(first_invalid_utf8("x\xE2\x82"), 1u);         // truncated
}

TEST(Umask, FormatsFourOctalDigits) {
    EXPECT_EQ(format_umask(0), "0000");
    EXPECT_EQ(format_umask(022), "0022");
    EXPECT_EQ(format_umask(0777), "0777");
}

TEST(FdTypeNames, RoundTrip) {
    for (FdType t : {FdType::regular, FdType::directory, FdType::pipe, FdType::socket, FdType::char_device,
                     FdType::block_device, FdType::other}) {
        EXPECT_EQ(fd_type_from_string(to_string(t)), t);
    }
    EXPECT_FALSE(fd_type_from_string("fifo"));
    EXPECT_FALSE(fd_type_from_string("Pipe"));
    EXPECT_FALSE(fd_type_from_string(""));
}

TEST(LimitValueEquality, UnlimitedNeverEqualsAnyFiniteValue) {
    EXPECT_EQ(LimitValue::infinity(), LimitValue::infinity());
    EXPECT_NE(LimitValue::infinity(), LimitValue::finite(0));
    EXPECT_NE(LimitValue::infinity(), LimitValue::finite(UINT64_MAX));
    EXPECT_EQ(LimitValue::finite(1024), LimitValue::finite(1024));
    EXPECT_NE(LimitValue::finite(1024), LimitValue::finite(1025));
}
