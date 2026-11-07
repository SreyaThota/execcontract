#include "report.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

using namespace execcontract;
using json = nlohmann::json;

namespace {

Check check(const std::string& id, Status s) {
    Check c;
    c.id = id;
    c.status = s;
    c.expected = "e";
    c.observed = nullptr;
    c.diagnostic = "d";
    return c;
}

ReportInput sample() {
    ReportInput in;
    Check pass = check("cwd", Status::pass);
    pass.observed = "/a \"quoted\"\n\\path\x01";
    Check fail = check("fd.3", Status::fail);
    fail.reason = "missing";
    Check err = check("umask", Status::error);
    err.error = SysError{"umask_unavailable", ENOENT, "open /proc/self/status"};
    in.checks = {pass, fail, err};
    in.unchecked = {"rlimit_core"};
    in.outcome = aggregate(in.checks);
    return in;
}

}  // namespace

TEST(Report, SchemaAndStableFields) {
    const json r = json::parse(render_report(sample()));
    EXPECT_EQ(r["report_version"], 1);
    EXPECT_EQ(r["tool"]["name"], "execcontract");
    EXPECT_EQ(r["tool"]["version"], tool_version());
    EXPECT_EQ(r["outcome"], "inspection_failed");
    EXPECT_EQ(r["exit_code"], 3);
    EXPECT_EQ(r["summary"], (json{{"checks", 3}, {"passed", 1}, {"failed", 1}, {"errors", 1}}));
    EXPECT_EQ(r["unchecked"], (json{"rlimit_core"}));
    EXPECT_TRUE(r["error"].is_null());
    ASSERT_EQ(r["checks"].size(), 3u);
    for (const auto& c : r["checks"]) {
        for (const char* key : {"id", "status", "expected", "observed", "reason", "diagnostic", "error"}) {
            EXPECT_TRUE(c.contains(key)) << key;
        }
    }
    EXPECT_EQ(r["checks"][1]["reason"], "missing");
    EXPECT_TRUE(r["checks"][0]["reason"].is_null());
    EXPECT_TRUE(r["checks"][0]["error"].is_null());
    const json& e = r["checks"][2]["error"];
    EXPECT_EQ(e["code"], "umask_unavailable");
    EXPECT_EQ(e["errno"], ENOENT);
    EXPECT_EQ(e["errno_name"], "ENOENT");
}

TEST(Report, EscapingRoundTrips) {
    const json r = json::parse(render_report(sample()));
    EXPECT_EQ(r["checks"][0]["observed"], "/a \"quoted\"\n\\path\x01");
    const std::string text = render_report(sample());
    EXPECT_NE(text.find("\\u0001"), std::string::npos);  // control characters escaped
    EXPECT_EQ(text.back(), '\n');
}

TEST(Report, CheckOrderPreserved) {
    const json r = json::parse(render_report(sample()));
    EXPECT_EQ(r["checks"][0]["id"], "cwd");
    EXPECT_EQ(r["checks"][1]["id"], "fd.3");
    EXPECT_EQ(r["checks"][2]["id"], "umask");
}

TEST(Report, Deterministic) {
    EXPECT_EQ(render_report(sample()), render_report(sample()));
}

TEST(Report, InvalidContractCarriesTopLevelError) {
    ReportInput in;
    in.outcome = Outcome::invalid_contract;
    in.error = ReportError{"contract_not_found", "cannot open contract", ENOENT, ""};
    const json r = json::parse(render_report(in));
    EXPECT_EQ(r["exit_code"], 2);
    EXPECT_TRUE(r["checks"].empty());
    EXPECT_EQ(r["error"]["code"], "contract_not_found");
    EXPECT_EQ(r["error"]["errno_name"], "ENOENT");
    EXPECT_TRUE(r["error"]["pointer"].is_null());
}

TEST(Report, InvalidUtf8IsASerialisationFailureNotReplacement) {
    ReportInput in = sample();
    in.checks[0].observed = "/bad\xFF";
    EXPECT_THROW(render_report(in), nlohmann::json::exception);
}

TEST(Report, FallbackIsValidJson) {
    const json r = json::parse(render_internal_error_fallback("serialization_failed"));
    EXPECT_EQ(r["outcome"], "internal_error");
    EXPECT_EQ(r["exit_code"], 3);
    EXPECT_EQ(r["error"]["code"], "serialization_failed");
}
