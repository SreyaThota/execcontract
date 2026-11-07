#include "cli.hpp"

#include <gtest/gtest.h>

using namespace execcontract;

namespace {
CliResult parse(std::vector<std::string_view> args) {
    return parse_args(args);
}
}  // namespace

TEST(Cli, ContractOnly) {
    const auto r = parse({"--contract", "c.json"});
    ASSERT_TRUE(r.options);
    EXPECT_EQ(r.options->mode, CliOptions::Mode::check);
    EXPECT_EQ(r.options->contract_path, "c.json");
    EXPECT_FALSE(r.options->report_path);
}

TEST(Cli, EqualsFormAndReport) {
    const auto r = parse({"--contract=/a b/c.json", "--report", "/tmp/r.json"});
    ASSERT_TRUE(r.options);
    EXPECT_EQ(r.options->contract_path, "/a b/c.json");
    EXPECT_EQ(r.options->report_path, std::optional<std::string>("/tmp/r.json"));
}

TEST(Cli, HelpAndVersionWin) {
    EXPECT_EQ(parse({"--bogus", "--help"}).options->mode, CliOptions::Mode::help);
    EXPECT_EQ(parse({"-h"}).options->mode, CliOptions::Mode::help);
    EXPECT_EQ(parse({"--version"}).options->mode, CliOptions::Mode::version);
}

TEST(Cli, UsageErrors) {
    EXPECT_FALSE(parse({}).options);
    EXPECT_FALSE(parse({"--contract"}).options);
    EXPECT_FALSE(parse({"--contract="}).options);
    EXPECT_FALSE(parse({"--contract", "a", "--contract", "b"}).options);
    EXPECT_FALSE(parse({"--report", "r"}).options);
    EXPECT_FALSE(parse({"--contract", "a", "--report", "r", "--report", "s"}).options);
    EXPECT_FALSE(parse({"--contract", "a", "positional"}).options);
    EXPECT_FALSE(parse({"--unknown=1", "--contract", "a"}).options);
    EXPECT_FALSE(parse({"-"}).options);
    EXPECT_FALSE(parse({"--contract", "a"}).error.size() > 0);
    EXPECT_FALSE(parse({}).error.empty());
}
