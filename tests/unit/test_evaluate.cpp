#include "evaluate.hpp"

#include <gtest/gtest.h>

using namespace execcontract;
using ojson = nlohmann::ordered_json;

namespace {

FdInventory inventory(std::vector<std::pair<int, FdType>> fds, bool complete = true) {
    FdInventory inv;
    for (auto [fd, type] : fds) {
        inv.entries.push_back(FdEntry{fd, type, std::nullopt});
    }
    inv.complete = complete;
    inv.scanner_fd = 3;
    if (!complete) {
        inv.error = SysError{"inventory_capped", 0, "capped"};
    }
    return inv;
}

FdRule req(int fd, std::optional<FdType> t = std::nullopt) {
    return FdRule{fd, true, t};
}
FdRule perm(int fd, std::optional<FdType> t = std::nullopt) {
    return FdRule{fd, false, t};
}

std::vector<Check> eval_fds(std::vector<FdRule> rules, const FdInventory& inv) {
    Contract c;
    c.fds = std::move(rules);
    Snapshot s;
    s.fds = inv;
    return evaluate(c, s);
}

// `id` by value: the result can only refer into `checks`.
const Check& find(const std::vector<Check>& checks, std::string_view id) {
    for (const Check& c : checks) {
        if (c.id == id)
            return c;
    }
    throw std::runtime_error("no check " + std::string(id));
}

std::vector<std::string> ids(const std::vector<Check>& checks) {
    std::vector<std::string> out;
    for (const Check& c : checks) out.push_back(c.id);
    return out;
}

Check with_status(Status s) {
    Check c;
    c.id = "x";
    c.status = s;
    return c;
}

}  // namespace

// ------------------------------------------------------------- descriptors

TEST(EvaluateFds, RequiredPresentPasses) {
    const auto checks = eval_fds({req(0), req(1), req(2)},
                                 inventory({{0, FdType::char_device}, {1, FdType::pipe}, {2, FdType::pipe}}));
    EXPECT_EQ(ids(checks), (std::vector<std::string>{"fd.inventory", "fd.0", "fd.1", "fd.2"}));
    EXPECT_EQ(aggregate(checks), Outcome::passed);
    EXPECT_EQ(find(checks, "fd.1").observed, (ojson{{"present", true}, {"type", "pipe"}}));
}

TEST(EvaluateFds, RequiredMissingFails) {
    const auto checks = eval_fds({req(0), req(3)}, inventory({{0, FdType::char_device}}));
    const Check& c = find(checks, "fd.3");
    EXPECT_EQ(c.status, Status::fail);
    EXPECT_EQ(c.reason, "missing");
    EXPECT_EQ(c.observed, (ojson{{"present", false}, {"type", nullptr}}));
    EXPECT_EQ(aggregate(checks), Outcome::violated);
}

TEST(EvaluateFds, PermittedAbsentPasses) {
    const auto checks = eval_fds({req(0), perm(5, FdType::socket)}, inventory({{0, FdType::char_device}}));
    EXPECT_EQ(find(checks, "fd.5").status, Status::pass);
    EXPECT_EQ(find(checks, "fd.5").expected, (ojson{{"presence", "permitted"}, {"type", "socket"}}));
    EXPECT_EQ(aggregate(checks), Outcome::passed);
}

TEST(EvaluateFds, UnexpectedDescriptorsFailInAscendingOrder) {
    const auto checks =
        eval_fds({req(0)}, inventory({{0, FdType::char_device}, {4, FdType::regular}, {9, FdType::socket}}));
    EXPECT_EQ(ids(checks),
              (std::vector<std::string>{"fd.inventory", "fd.0", "fd.unexpected.4", "fd.unexpected.9"}));
    EXPECT_EQ(find(checks, "fd.unexpected.9").reason, "unexpected");
    EXPECT_EQ(find(checks, "fd.unexpected.9").observed, (ojson{{"present", true}, {"type", "socket"}}));
    EXPECT_EQ(aggregate(checks), Outcome::violated);
}

TEST(EvaluateFds, EmptyAllowlistMakesEveryDescriptorUnexpected) {
    const auto checks = eval_fds({}, inventory({{0, FdType::char_device}}));
    EXPECT_EQ(ids(checks), (std::vector<std::string>{"fd.inventory", "fd.unexpected.0"}));
}

TEST(EvaluateFds, TypeMatchAndMismatch) {
    const auto checks = eval_fds({req(0, FdType::char_device), perm(4, FdType::pipe)},
                                 inventory({{0, FdType::char_device}, {4, FdType::regular}}));
    EXPECT_EQ(find(checks, "fd.0").status, Status::pass);
    const Check& c = find(checks, "fd.4");
    EXPECT_EQ(c.status, Status::fail);
    EXPECT_EQ(c.reason, "type_mismatch");
    EXPECT_EQ(c.observed["type"], "regular");
}

TEST(EvaluateFds, MissingRequiredWithTypeIsMissingNotTypeMismatch) {
    const auto checks = eval_fds({req(3, FdType::pipe)}, inventory({}));
    EXPECT_EQ(find(checks, "fd.3").reason, "missing");
}

TEST(EvaluateFds, UnavailableTypeIsAnErrorOnlyWhenATypeIsExpected) {
    FdInventory inv = inventory({});
    inv.entries.push_back(FdEntry{4, std::nullopt, SysError{"fd_type_unavailable", EBADF, "fstat"}});
    inv.entries.push_back(FdEntry{5, std::nullopt, SysError{"fd_type_unavailable", EBADF, "fstat"}});
    const auto checks = eval_fds({req(4, FdType::pipe), req(5)}, inv);
    const Check& typed = find(checks, "fd.4");
    EXPECT_EQ(typed.status, Status::error);
    ASSERT_TRUE(typed.error);
    EXPECT_EQ(typed.error->code, "fd_type_unavailable");
    EXPECT_EQ(typed.error->err, EBADF);
    EXPECT_EQ(typed.observed, (ojson{{"present", true}, {"type", nullptr}}));
    EXPECT_EQ(find(checks, "fd.5").status, Status::pass);
    EXPECT_EQ(aggregate(checks), Outcome::inspection_failed);
}

TEST(EvaluateFds, FailedEnumerationIsNeverAnEmptySuccessfulInventory) {
    FdInventory inv;  // e.g. opendir failed: nothing known
    inv.error = SysError{"fd_enumeration_failed", ENOENT, "opendir"};
    const auto checks = eval_fds({req(0), perm(1)}, inv);
    EXPECT_EQ(find(checks, "fd.inventory").status, Status::error);
    EXPECT_EQ(find(checks, "fd.inventory").observed["scanner_fd"], nullptr);
    // Neither "required missing" (fail) nor "permitted absent" (pass) can be concluded.
    EXPECT_EQ(find(checks, "fd.0").status, Status::error);
    EXPECT_EQ(find(checks, "fd.0").observed, nullptr);
    EXPECT_EQ(find(checks, "fd.1").status, Status::error);
    EXPECT_EQ(aggregate(checks), Outcome::inspection_failed);
}

TEST(EvaluateFds, PartialInventoryKeepsKnownEvidence) {
    const FdInventory inv = inventory({{0, FdType::char_device}, {7, FdType::pipe}}, /*complete=*/false);
    const auto checks = eval_fds({req(0, FdType::socket), req(1)}, inv);
    EXPECT_EQ(find(checks, "fd.inventory").status, Status::error);
    EXPECT_EQ(find(checks, "fd.inventory").error->code, "inventory_capped");
    EXPECT_EQ(find(checks, "fd.0").status, Status::fail);   // seen: mismatch is known
    EXPECT_EQ(find(checks, "fd.1").status, Status::error);  // not seen: unknown
    EXPECT_EQ(find(checks, "fd.1").error->code, "fd_inventory_incomplete");
    EXPECT_EQ(find(checks, "fd.unexpected.7").status, Status::fail);  // seen: known violation
    EXPECT_EQ(aggregate(checks), Outcome::inspection_failed);
}

// ------------------------------------------------------------- cwd / umask

TEST(EvaluateCwd, ExactByteComparison) {
    Contract c;
    c.cwd = "/srv/app";
    Snapshot s;
    s.cwd = Observed<std::string>::ok("/srv/app");
    EXPECT_EQ(evaluate(c, s).at(0).status, Status::pass);
    for (const char* other : {"/srv/app/", "/srv/App", "/srv/app2", "/srv"}) {
        s.cwd = Observed<std::string>::ok(other);
        const auto checks = evaluate(c, s);
        EXPECT_EQ(checks.at(0).status, Status::fail) << other;
        EXPECT_EQ(checks.at(0).observed, other);
    }
}

TEST(EvaluateCwd, ObservationErrorPropagates) {
    Contract c;
    c.cwd = "/srv/app";
    Snapshot s;
    s.cwd = Observed<std::string>::fail(SysError{"cwd_deleted", ENOENT, "getcwd"});
    const auto checks = evaluate(c, s);
    EXPECT_EQ(checks.at(0).status, Status::error);
    EXPECT_EQ(checks.at(0).observed, nullptr);
    EXPECT_EQ(checks.at(0).error->code, "cwd_deleted");
}

TEST(EvaluateUmask, MatchMismatchAndRepresentation) {
    Contract c;
    c.umask = 027;
    Snapshot s;
    s.umask = Observed<unsigned>::ok(027);
    EXPECT_EQ(evaluate(c, s).at(0).status, Status::pass);
    s.umask = Observed<unsigned>::ok(022);
    const auto checks = evaluate(c, s);
    EXPECT_EQ(checks.at(0).status, Status::fail);
    EXPECT_EQ(checks.at(0).expected, "0027");
    EXPECT_EQ(checks.at(0).observed, "0022");
    s.umask = Observed<unsigned>::fail(SysError{"umask_unavailable", 0, "no field"});
    EXPECT_EQ(evaluate(c, s).at(0).status, Status::error);
}

// ------------------------------------------------------------- limits

TEST(EvaluateRlimit, SoftAndHardAreIndependent) {
    Contract c;
    c.rlimit_nofile = LimitRule{LimitValue::finite(1024), std::nullopt};
    c.rlimit_core = LimitRule{std::nullopt, LimitValue::infinity()};
    Snapshot s;
    s.rlimit_nofile = Observed<Limits>::ok(Limits{LimitValue::finite(1024), LimitValue::finite(1)});
    s.rlimit_core = Observed<Limits>::ok(Limits{LimitValue::finite(0), LimitValue::infinity()});
    const auto checks = evaluate(c, s);
    EXPECT_EQ(ids(checks), (std::vector<std::string>{"rlimit_nofile.soft", "rlimit_core.hard"}));
    EXPECT_EQ(aggregate(checks), Outcome::passed);
    EXPECT_EQ(find(checks, "rlimit_core.hard").observed, "unlimited");
}

TEST(EvaluateRlimit, UnlimitedVersusFinite) {
    Contract c;
    c.rlimit_core = LimitRule{LimitValue::infinity(), LimitValue::finite(UINT64_MAX - 1)};
    Snapshot s;
    s.rlimit_core = Observed<Limits>::ok(Limits{LimitValue::finite(UINT64_MAX - 1), LimitValue::infinity()});
    const auto checks = evaluate(c, s);
    EXPECT_EQ(find(checks, "rlimit_core.soft").status, Status::fail);
    EXPECT_EQ(find(checks, "rlimit_core.soft").observed, ojson(UINT64_MAX - 1));
    EXPECT_EQ(find(checks, "rlimit_core.hard").status, Status::fail);
    EXPECT_EQ(find(checks, "rlimit_core.hard").observed, "unlimited");
}

TEST(EvaluateRlimit, UnavailableIsNeverUnlimited) {
    Contract c;
    c.rlimit_nofile = LimitRule{LimitValue::infinity(), LimitValue::infinity()};
    Snapshot s;
    s.rlimit_nofile = Observed<Limits>::fail(SysError{"rlimit_unavailable", EINVAL, "getrlimit"});
    const auto checks = evaluate(c, s);
    ASSERT_EQ(checks.size(), 2u);
    for (const Check& ch : checks) {
        EXPECT_EQ(ch.status, Status::error);
        EXPECT_EQ(ch.observed, nullptr);
    }
}

TEST(Evaluate, ObservationWithNeitherValueNorErrorFailsClosed) {
    Contract c;
    c.cwd = "/";
    c.umask = 022;
    c.rlimit_core = LimitRule{LimitValue::finite(0), std::nullopt};
    const Snapshot empty;  // every Observed<> holds neither value nor error
    const auto checks = evaluate(c, empty);
    ASSERT_EQ(checks.size(), 3u);
    for (const Check& ch : checks) {
        EXPECT_EQ(ch.status, Status::error) << ch.id;
        ASSERT_TRUE(ch.error) << ch.id;
        EXPECT_EQ(ch.error->code, "observation_missing") << ch.id;
        EXPECT_EQ(ch.observed, nullptr) << ch.id;
    }
    EXPECT_EQ(aggregate(checks), Outcome::inspection_failed);
}

// ------------------------------------------------------------- aggregation

TEST(Aggregate, Precedence) {
    EXPECT_EQ(aggregate({with_status(Status::pass)}), Outcome::passed);
    EXPECT_EQ(aggregate({with_status(Status::pass), with_status(Status::fail)}), Outcome::violated);
    EXPECT_EQ(aggregate({with_status(Status::fail), with_status(Status::error)}), Outcome::inspection_failed);
    EXPECT_EQ(aggregate({with_status(Status::error), with_status(Status::fail)}), Outcome::inspection_failed);
    EXPECT_EQ(aggregate({}), Outcome::internal_error);
}

TEST(Aggregate, ExitCodes) {
    EXPECT_EQ(exit_code_for(Outcome::passed), 0);
    EXPECT_EQ(exit_code_for(Outcome::violated), 1);
    EXPECT_EQ(exit_code_for(Outcome::invalid_contract), 2);
    EXPECT_EQ(exit_code_for(Outcome::inspection_failed), 3);
    EXPECT_EQ(exit_code_for(Outcome::internal_error), 3);
}

TEST(Evaluate, AreaOrderIsFixed) {
    Contract c;
    c.rlimit_core = LimitRule{LimitValue::finite(0), LimitValue::finite(0)};
    c.umask = 022;
    c.cwd = "/";
    c.rlimit_nofile = LimitRule{LimitValue::finite(1), LimitValue::finite(2)};
    c.fds = std::vector<FdRule>{req(0)};
    Snapshot s;
    s.fds = inventory({{0, FdType::char_device}});
    s.cwd = Observed<std::string>::ok("/");
    s.umask = Observed<unsigned>::ok(022);
    s.rlimit_nofile = Observed<Limits>::ok(Limits{LimitValue::finite(1), LimitValue::finite(2)});
    s.rlimit_core = Observed<Limits>::ok(Limits{LimitValue::finite(0), LimitValue::finite(0)});
    EXPECT_EQ(ids(evaluate(c, s)),
              (std::vector<std::string>{"fd.inventory", "fd.0", "cwd", "umask", "rlimit_nofile.soft",
                                        "rlimit_nofile.hard", "rlimit_core.soft", "rlimit_core.hard"}));
}
