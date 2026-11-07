#include "contract.hpp"
#include "test_support.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

using namespace execcontract;

namespace {

std::string error_code(std::string_view text) {
    const ContractResult r = parse_contract(text);
    if (r.contract) {
        return "<valid>";
    }
    return r.error->code;
}

std::string error_pointer(std::string_view text) {
    const ContractResult r = parse_contract(text);
    return r.error ? r.error->pointer : "<valid>";
}

Contract valid(std::string_view text) {
    const ContractResult r = parse_contract(text);
    if (!r.contract) {
        ADD_FAILURE() << "unexpected error " << r.error->code << ": " << r.error->message;
        return {};
    }
    return *r.contract;
}

std::string with_version(const std::string& body) {
    return R"({"version": 1, )" + body + "}";
}

}  // namespace

// ------------------------------------------------------------- happy paths

TEST(ContractParse, FullContract) {
    const Contract c = valid(R"({
        "version": 1,
        "fds": {"required": [{"fd": 2}, {"fd": 0, "type": "char_device"}],
                "permitted": [{"fd": 7, "type": "socket"}]},
        "cwd": "/srv/my app",
        "umask": "0027",
        "rlimit_nofile": {"soft": 1024, "hard": 4096},
        "rlimit_core": {"soft": 0, "hard": "unlimited"}
    })");
    ASSERT_TRUE(c.fds);
    ASSERT_EQ(c.fds->size(), 3u);
    // Sorted by descriptor number regardless of input order.
    EXPECT_EQ((*c.fds)[0].fd, 0);
    EXPECT_TRUE((*c.fds)[0].required);
    EXPECT_EQ((*c.fds)[0].type, FdType::char_device);
    EXPECT_EQ((*c.fds)[1].fd, 2);
    EXPECT_FALSE((*c.fds)[1].type);
    EXPECT_EQ((*c.fds)[2].fd, 7);
    EXPECT_FALSE((*c.fds)[2].required);
    EXPECT_EQ(c.cwd, std::optional<std::string>("/srv/my app"));
    EXPECT_EQ(c.umask, std::optional<unsigned>(027));
    EXPECT_EQ(c.rlimit_nofile->soft, LimitValue::finite(1024));
    EXPECT_EQ(c.rlimit_nofile->hard, LimitValue::finite(4096));
    EXPECT_EQ(c.rlimit_core->soft, LimitValue::finite(0));
    EXPECT_EQ(c.rlimit_core->hard, LimitValue::infinity());
    EXPECT_TRUE(unchecked_areas(c).empty());
}

TEST(ContractParse, SingleAreasAndUncheckedList) {
    const Contract c = valid(with_version(R"("umask": "0000")"));
    EXPECT_EQ(c.umask, std::optional<unsigned>(0));
    EXPECT_EQ(unchecked_areas(c), (std::vector<std::string>{"fds", "cwd", "rlimit_nofile", "rlimit_core"}));
}

TEST(ContractParse, ExplicitNoDescriptorsAllowed) {
    const Contract c = valid(with_version(R"("fds": {"required": [], "permitted": []})"));
    ASSERT_TRUE(c.fds);
    EXPECT_TRUE(c.fds->empty());
}

TEST(ContractParse, IndependentSoftOrHard) {
    const Contract c =
        valid(with_version(R"("rlimit_nofile": {"hard": 8}, "rlimit_core": {"soft": "unlimited"})"));
    EXPECT_FALSE(c.rlimit_nofile->soft);
    EXPECT_EQ(c.rlimit_nofile->hard, LimitValue::finite(8));
    EXPECT_EQ(c.rlimit_core->soft, LimitValue::infinity());
    EXPECT_FALSE(c.rlimit_core->hard);
}

TEST(ContractParse, NumericBoundaries) {
    // Largest finite rlim_t value is RLIM_INFINITY - 1.
    EXPECT_EQ(valid(with_version(R"("rlimit_core": {"soft": 18446744073709551614})")).rlimit_core->soft,
              LimitValue::finite(18446744073709551614ULL));
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {"soft": 18446744073709551615})")), "out_of_range");
    // Beyond 64 bits the library yields a float: a type error, not a coercion.
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {"soft": 18446744073709551616})")), "invalid_type");
    EXPECT_EQ(valid(with_version(R"("fds": {"required": [{"fd": 2147483647}]})")).fds->at(0).fd, 2147483647);
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [{"fd": 2147483648}]})")), "out_of_range");
}

TEST(ContractParse, RootPathAndEscapedCharacters) {
    EXPECT_EQ(valid(with_version(R"("cwd": "/")")).cwd, std::optional<std::string>("/"));
    EXPECT_EQ(valid(with_version(R"("cwd": "/a \"q\"\\b\tc")")).cwd,
              std::optional<std::string>("/a \"q\"\\b\tc"));
    EXPECT_EQ(valid(with_version(R"("cwd": "/caf\u00e9")")).cwd, std::optional<std::string>("/caf\xC3\xA9"));
}

// ------------------------------------------------------------- malformed input

TEST(ContractParse, MalformedJson) {
    EXPECT_EQ(error_code(""), "malformed_json");
    EXPECT_EQ(error_code("{"), "malformed_json");
    EXPECT_EQ(error_code(R"({"version": 1,})"), "malformed_json");
    EXPECT_EQ(error_code(R"({"version": 1} trailing)"), "malformed_json");
    EXPECT_EQ(error_code("{\"version\": 1, \"cwd\": \"/\xFF\"}"), "malformed_json");  // invalid UTF-8
    EXPECT_EQ(error_code(R"({"version": 1, /* c */ "umask": "0022"})"), "malformed_json");
}

TEST(ContractParse, EmbeddedNulIsRejectedNotTreatedAsEndOfInput) {
    // nlohmann's lexer would stop at the NUL and accept the weaker first part.
    const std::string text =
        std::string(R"({"version":1,"umask":"0022"})") + '\0' + R"({"version":1,"fds":{"required":[]}})";
    const ContractResult r = parse_contract(text);
    ASSERT_TRUE(r.error);
    EXPECT_EQ(r.error->code, "malformed_json");
    EXPECT_NE(r.error->message.find("offset 28"), std::string::npos);
    EXPECT_EQ(error_code(std::string("\0", 1)), "malformed_json");
}

TEST(ContractParse, NumberOverflowIsInvalidInputNotAnException) {
    EXPECT_EQ(error_code(R"({"version":1,"rlimit_core":{"soft":1e999}})"), "out_of_range");
    EXPECT_EQ(error_code(R"({"version":1e999,"umask":"0022"})"), "out_of_range");
    EXPECT_EQ(error_code(R"({"version":1,"fds":{"required":[{"fd":-1e400}]}})"), "out_of_range");
}

TEST(ContractParse, ErrorMessagesAreAsciiEvenForInvalidUtf8Input) {
    for (const char* bad : {"{\"version\":1,\"cwd\":\"/\xFF\"}", "\xFF{}", "{\"\xC3\x28\":1}"}) {
        const ContractResult r = parse_contract(bad);
        ASSERT_TRUE(r.error) << bad;
        EXPECT_EQ(r.error->code, "malformed_json") << bad;
        EXPECT_FALSE(first_invalid_utf8(r.error->message)) << r.error->message;
        for (const char c : r.error->message) {
            EXPECT_TRUE(static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7f);
        }
    }
}

TEST(ContractParse, PointerTokensAreEscaped) {
    EXPECT_EQ(error_pointer(with_version(R"("rlimit_core": {"a/b~c": 1})")), "/rlimit_core/a~1b~0c");
}

TEST(ContractParse, Utf8ByteOrderMarkIsAccepted) {
    EXPECT_EQ(error_code("\xEF\xBB\xBF" + with_version(R"("umask": "0022")")), "<valid>");
}

TEST(ContractParse, TopLevelMustBeObject) {
    EXPECT_EQ(error_code("[]"), "invalid_type");
    EXPECT_EQ(error_code("1"), "invalid_type");
    EXPECT_EQ(error_code("null"), "invalid_type");
}

TEST(ContractParse, DuplicateKeysRejectedAtAnyDepth) {
    EXPECT_EQ(error_code(R"({"version": 1, "version": 1, "umask": "0022"})"), "duplicate_key");
    EXPECT_EQ(error_code(R"({"version": 1, "umask": "0022", "umask": "0077"})"), "duplicate_key");
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {"soft": 0, "soft": 1})")), "duplicate_key");
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [{"fd": 1, "fd": 3}]})")), "duplicate_key");
    // Same key in sibling objects is fine.
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [{"fd": 1}, {"fd": 3}]})")), "<valid>");
}

TEST(ContractParse, DepthLimit) {
    std::string deep = R"({"version": 1, "x": )";
    for (int i = 0; i < 20; ++i) deep += "[";
    for (int i = 0; i < 20; ++i) deep += "]";
    deep += "}";
    EXPECT_EQ(error_code(deep), "too_deep");
}

TEST(ContractParse, VersionHandling) {
    EXPECT_EQ(error_code(R"({"umask": "0022"})"), "missing_version");
    EXPECT_EQ(error_code(R"({"version": 2, "umask": "0022"})"), "unsupported_version");
    EXPECT_EQ(error_code(R"({"version": 0, "umask": "0022"})"), "unsupported_version");
    EXPECT_EQ(error_code(R"({"version": -1, "umask": "0022"})"), "unsupported_version");
    EXPECT_EQ(error_code(R"({"version": 1.0, "umask": "0022"})"), "invalid_type");
    EXPECT_EQ(error_code(R"({"version": "1", "umask": "0022"})"), "invalid_type");
    EXPECT_EQ(error_code(R"({"version": true, "umask": "0022"})"), "invalid_type");
    EXPECT_EQ(error_code(R"({"version": null, "umask": "0022"})"), "invalid_type");
}

TEST(ContractParse, EmptyContractAndAreas) {
    EXPECT_EQ(error_code(R"({"version": 1})"), "empty_contract");
    EXPECT_EQ(error_code(with_version(R"("fds": {})")), "empty_area");
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {})")), "empty_area");
    EXPECT_EQ(error_code(with_version(R"("rlimit_nofile": {})")), "empty_area");
}

TEST(ContractParse, NullIsNotUnchecked) {
    EXPECT_EQ(error_code(with_version(R"("cwd": null)")), "invalid_type");
    EXPECT_EQ(error_code(with_version(R"("fds": null)")), "invalid_type");
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {"soft": null})")), "invalid_type");
}

TEST(ContractParse, UnknownFieldsRejectedWithPointer) {
    EXPECT_EQ(error_code(with_version(R"("umask": "0022", "env": {})")), "unknown_field");
    EXPECT_EQ(error_pointer(with_version(R"("umask": "0022", "env": {})")), "/env");
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [], "allow_others": true})")), "unknown_field");
    EXPECT_EQ(error_pointer(with_version(R"("fds": {"required": [{"fd": 0, "path": "/dev/null"}]})")),
              "/fds/required/0/path");
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {"soft": 0, "max": 1})")), "unknown_field");
}

// ------------------------------------------------------------- descriptors

TEST(ContractParse, DescriptorEntryTypes) {
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [1]})")), "invalid_type");
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": {"fd": 1}})")), "invalid_type");
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [{}]})")), "missing_field");
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [{"type": "pipe"}]})")), "missing_field");
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [{"fd": true}]})")), "invalid_type");
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [{"fd": 1.0}]})")), "invalid_type");
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [{"fd": "1"}]})")), "invalid_type");
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [{"fd": -1}]})")), "out_of_range");
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [{"fd": 1, "type": "fifo"}]})")),
              "invalid_fd_type");
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [{"fd": 1, "type": 3}]})")), "invalid_type");
}

TEST(ContractParse, DuplicateDescriptors) {
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [{"fd": 3}, {"fd": 3}]})")), "duplicate_fd");
    EXPECT_EQ(error_code(with_version(R"("fds": {"permitted": [{"fd": 3}, {"fd": 3, "type": "pipe"}]})")),
              "duplicate_fd");
    // Required implies permitted: listing both is a contradiction, not a no-op.
    EXPECT_EQ(error_code(with_version(R"("fds": {"required": [{"fd": 3}], "permitted": [{"fd": 3}]})")),
              "duplicate_fd");
}

TEST(ContractParse, DescriptorEntryCap) {
    auto make = [](std::size_t n) {
        std::string s = R"({"version": 1, "fds": {"permitted": [)";
        for (std::size_t i = 0; i < n; ++i) {
            s += (i ? "," : "") + std::string(R"({"fd": )") + std::to_string(i) + "}";
        }
        return s + "]}}";
    };
    EXPECT_EQ(error_code(make(kMaxContractFdEntries)), "<valid>");
    EXPECT_EQ(error_code(make(kMaxContractFdEntries + 1)), "too_many_fd_entries");
}

// ------------------------------------------------------------- cwd

TEST(ContractParse, CwdMustBeCanonicalAbsolute) {
    for (const char* bad :
         {"relative", "./x", "/srv/", "/srv//app", "/srv/./app", "/srv/../app", "/srv/.", "/srv/..", "//"}) {
        EXPECT_EQ(error_code(with_version(std::string(R"("cwd": ")") + bad + "\"")), "noncanonical_path")
            << bad;
    }
    EXPECT_EQ(error_code(with_version(R"("cwd": "")")), "invalid_path");
    EXPECT_EQ(error_code(with_version(R"("cwd": "/a\u0000b")")), "invalid_path");
    EXPECT_EQ(error_code(with_version(R"("cwd": 7)")), "invalid_type");
    // "..." and ".hidden" are ordinary names.
    EXPECT_EQ(error_code(with_version(R"("cwd": "/srv/.../.hidden")")), "<valid>");
}

TEST(ContractParse, CwdLengthCap) {
    const std::string at_cap = "/" + std::string(kMaxExpectedPathBytes - 1, 'a');
    const std::string over = "/" + std::string(kMaxExpectedPathBytes, 'a');
    EXPECT_EQ(error_code(with_version(R"("cwd": ")" + at_cap + "\"")), "<valid>");
    EXPECT_EQ(error_code(with_version(R"("cwd": ")" + over + "\"")), "invalid_path");
}

// ------------------------------------------------------------- umask

TEST(ContractParse, UmaskFormat) {
    EXPECT_EQ(valid(with_version(R"("umask": "0777")")).umask, std::optional<unsigned>(0777));
    for (const char* bad : {"022", "00022", "0o22", "0822", "1022", " 022", "0022 ", ""}) {
        EXPECT_EQ(error_code(with_version(std::string(R"("umask": ")") + bad + "\"")), "invalid_umask")
            << bad;
    }
    EXPECT_EQ(error_code(with_version(R"("umask": 18)")), "invalid_type");  // decimal 18 == octal 022
    EXPECT_EQ(error_code(with_version(R"("umask": true)")), "invalid_type");
}

// ------------------------------------------------------------- limits

TEST(ContractParse, LimitValueTypes) {
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {"soft": -1})")), "out_of_range");
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {"soft": 1.5})")), "invalid_type");
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {"soft": 1e3})")), "invalid_type");
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {"soft": true})")), "invalid_type");
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {"soft": "infinity"})")), "invalid_type");
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {"soft": "Unlimited"})")), "invalid_type");
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {"soft": "1024"})")), "invalid_type");
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": 0)")), "invalid_type");
}

TEST(ContractParse, ContradictoryLimits) {
    EXPECT_EQ(error_code(with_version(R"("rlimit_nofile": {"soft": 2048, "hard": 1024})")),
              "contradictory_limits");
    EXPECT_EQ(error_code(with_version(R"("rlimit_nofile": {"soft": "unlimited", "hard": 1024})")),
              "contradictory_limits");
    EXPECT_EQ(error_code(with_version(R"("rlimit_nofile": {"soft": 1024, "hard": 1024})")), "<valid>");
    EXPECT_EQ(error_code(with_version(R"("rlimit_core": {"soft": 5, "hard": "unlimited"})")), "<valid>");
}

// ------------------------------------------------------------- file loading

TEST(ContractFile, ReadsRegularFile) {
    test::TempDir dir;
    const auto p = dir.write("c.json", R"({"version":1,"umask":"0022"})");
    const ContractText t = load_contract_file(p.string());
    ASSERT_TRUE(t.text) << t.error->code;
    EXPECT_EQ(*t.text, R"({"version":1,"umask":"0022"})");
}

TEST(ContractFile, SizeCapBoundary) {
    test::TempDir dir;
    const auto at = dir.write("at.json", std::string(kMaxContractBytes, ' '));
    const auto over = dir.write("over.json", std::string(kMaxContractBytes + 1, ' '));
    EXPECT_TRUE(load_contract_file(at.string()).text);
    const ContractText t = load_contract_file(over.string());
    ASSERT_TRUE(t.error);
    EXPECT_EQ(t.error->code, "contract_too_large");
}

TEST(ContractFile, MissingDirectoryAndFifo) {
    test::TempDir dir;
    EXPECT_EQ(load_contract_file((dir.path() / "nope.json").string()).error->code, "contract_not_found");
    EXPECT_EQ(load_contract_file(dir.path().string()).error->code, "contract_not_regular_file");
    const auto fifo = dir.path() / "fifo";
    ASSERT_EQ(::mkfifo(fifo.c_str(), 0600), 0);
    // Must not block waiting for a writer.
    EXPECT_EQ(load_contract_file(fifo.string()).error->code, "contract_not_regular_file");
}

TEST(ContractFile, Unreadable) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root bypasses file permission checks";
    }
    test::TempDir dir;
    const auto p = dir.write("c.json", "{}");
    ASSERT_EQ(::chmod(p.c_str(), 0), 0);
    const ContractText t = load_contract_file(p.string());
    ASSERT_TRUE(t.error);
    EXPECT_EQ(t.error->code, "contract_unreadable");
    EXPECT_EQ(t.error->err, EACCES);
}
