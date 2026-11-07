// Deterministic mutation ("fuzz smoke") test for the contract input path.
//
// Starting from valid contracts, each iteration applies a few random byte
// mutations and checks properties that must hold for *any* input:
//   * parse_contract() returns exactly one of a contract or an error and never
//     lets an exception escape;
//   * every error code is one of the documented stable codes;
//   * error messages and pointers are valid UTF-8 (they end up in the JSON
//     report, which is serialised strictly);
//   * the full report renders for both outcomes, so input can never turn into
//     a serialization_failed / exit 3.
// The seed is fixed, so a failure is reproducible; the iteration count keeps
// the test well under a second in a normal build.

#include "contract.hpp"
#include "evaluate.hpp"
#include "report.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

using namespace execcontract;

namespace {

// Small, fast, deterministic PRNG (xorshift64*).
class Rng {
   public:
    explicit Rng(std::uint64_t seed) : s_(seed) {}
    std::uint64_t next() {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 0x2545F4914F6CDD1DULL;
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }

   private:
    std::uint64_t s_;
};

const std::vector<std::string>& seeds() {
    static const std::vector<std::string> s = {
        R"({"version": 1, "umask": "0022"})",
        R"({"version": 1, "cwd": "/srv/app", "rlimit_core": {"soft": 0, "hard": "unlimited"}})",
        R"({"version": 1, "fds": {"required": [{"fd": 0, "type": "char_device"}, {"fd": 1}],
            "permitted": [{"fd": 3, "type": "socket"}]}, "rlimit_nofile": {"soft": 1024, "hard": 4096}})",
    };
    return s;
}

// Bytes that are interesting to a JSON lexer and to the validator.
const std::string& interesting() {
    static const std::string s = std::string("{}[]\":,\\-+.0123456789eEtrufalsn \t\n") +
                                 std::string(1, '\0') +
                                 "\xFF\xC3\xED";  // NUL, invalid lead byte, UTF-8/surrogate lead bytes
    return s;
}

std::string mutate(std::string text, Rng& rng) {
    const std::size_t rounds = 1 + rng.below(4);
    for (std::size_t r = 0; r < rounds; ++r) {
        const std::size_t pos = text.empty() ? 0 : rng.below(text.size());
        switch (rng.below(5)) {
            case 0:  // overwrite with an interesting byte
                if (!text.empty())
                    text[pos] = interesting()[rng.below(interesting().size())];
                break;
            case 1:  // insert an interesting byte
                text.insert(pos, 1, interesting()[rng.below(interesting().size())]);
                break;
            case 2:  // delete a short range
                if (!text.empty())
                    text.erase(pos, 1 + rng.below(8));
                break;
            case 3:  // duplicate a short range (duplicate keys, nesting)
                if (!text.empty())
                    text.insert(pos, text.substr(pos, 1 + rng.below(16)));
                break;
            default:  // flip a random bit
                if (!text.empty())
                    text[pos] = static_cast<char>(text[pos] ^ (1 << rng.below(8)));
                break;
        }
    }
    return text;
}

const std::set<std::string> kDocumentedCodes = {
    "malformed_json",  "duplicate_key",        "too_deep",      "invalid_type",
    "missing_version", "unsupported_version",  "unknown_field", "empty_contract",
    "empty_area",      "missing_field",        "out_of_range",  "invalid_fd_type",
    "duplicate_fd",    "too_many_fd_entries",  "invalid_path",  "noncanonical_path",
    "invalid_umask",   "contradictory_limits",
};

}  // namespace

TEST(ContractRobustness, RandomMutationsStayOnTheDocumentedErrorPath) {
    constexpr int kIterations = 20000;
    Rng rng(0x5EED'2026'1108ULL);
    int valid = 0;
    std::set<std::string> seen;
    for (int i = 0; i < kIterations; ++i) {
        const std::string input = mutate(seeds()[rng.below(seeds().size())], rng);
        ContractResult result;
        ASSERT_NO_THROW(result = parse_contract(input)) << "iteration " << i;
        ASSERT_NE(result.contract.has_value(), result.error.has_value()) << "iteration " << i;

        ReportInput report;
        if (result.contract) {
            ++valid;
            report.checks = evaluate(*result.contract, Snapshot{});  // empty snapshot: every check errors
            report.unchecked = unchecked_areas(*result.contract);
            report.outcome = aggregate(report.checks);
        } else {
            const ContractError& e = *result.error;
            seen.insert(e.code);
            ASSERT_EQ(kDocumentedCodes.count(e.code), 1u)
                << "undocumented code " << e.code << " at iteration " << i;
            ASSERT_FALSE(first_invalid_utf8(e.message)) << "iteration " << i;
            ASSERT_FALSE(first_invalid_utf8(e.pointer)) << "iteration " << i;
            report.outcome = Outcome::invalid_contract;
            report.error = ReportError{e.code, e.message, 0, e.pointer};
        }
        ASSERT_NO_THROW((void)render_report(report)) << "iteration " << i;
    }
    // Guard against a mutator that only ever produces one kind of input.
    EXPECT_GT(valid, 0);
    EXPECT_GE(seen.size(), 8u);
    RecordProperty("valid_mutants", valid);
    RecordProperty("distinct_error_codes", static_cast<int>(seen.size()));
}
