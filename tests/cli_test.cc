#include "ircli/command.h"
#include "ircli/internal/command_line.h"

#include "ircore/dng.h"
#include "ircore/processing.h"

#include "support/synthetic.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct Outcome {
  int exit_code{};
  std::string output;
  std::string error;
};

Outcome run(std::vector<std::string> arguments) {
  const std::vector<std::string_view> views(arguments.begin(), arguments.end());
  std::ostringstream output;
  std::ostringstream error;
  const int code = ircli::run(views, output, error);
  return {code, output.str(), error.str()};
}

void write_input(const std::filesystem::path &path) {
  const auto dng = test_support::make_cfa_dng(
      48, 32, test_support::uniform_scene({0.5, 0.3, 0.25}));
  ASSERT_TRUE(ircore::write_dng(path, dng, {}).has_value());
}

TEST(Cli, HelpAndVersion) {
  const auto help = run({"--help"});
  EXPECT_EQ(help.exit_code, ircli::exit_success);
  EXPECT_NE(help.output.find("Usage:"), std::string::npos);
  const auto version = run({"--version"});
  EXPECT_EQ(version.exit_code, ircli::exit_success);
  EXPECT_EQ(version.output, "spectrallab 0.1.0\n");
}

TEST(Cli, UsageErrors) {
  for (const auto &arguments : std::vector<std::vector<std::string>>{
           {},
           {"--bogus"},
           {"--filter", "900", "a.dng", "b.dng"},
           {"--wb"},
           {"--wb", "area=1,2,3", "a.dng", "b.dng"},
           {"--mapping", "sepia", "a.dng", "b.dng"},
           {"--threads", "0", "a.dng", "b.dng"},
           {"--help=yes"},
           {"a.dng", "b.dng", "c.dng"},
           {"--analyze", "-o", "out", "a.dng"}}) {
    const auto outcome = run(arguments);
    EXPECT_EQ(outcome.exit_code, ircli::exit_usage);
    EXPECT_NE(outcome.error.find("spectrallab:"), std::string::npos);
  }
}

TEST(Cli, ParsesOptions) {
  const std::vector<std::string_view> arguments{
      "--filter=850",     "--mapping",          "monochrome",
      "--wb",             "neutral=1,0.5,0.25", "--compression=none",
      "--no-thumbnail",   "--threads",          "3",
      "-f",               "--",                 "-odd-name.dng",
      "out.dng"};
  const auto parsed = ircli::detail::parse_command_line(arguments);
  ASSERT_TRUE(parsed.has_value()) << parsed.error();
  EXPECT_EQ(parsed->options.filter, ircore::FilterType::nm850);
  EXPECT_EQ(parsed->options.mapping, ircore::ChannelMapping::monochrome);
  EXPECT_EQ(parsed->options.white_balance.mode,
            ircore::WhiteBalanceMode::explicit_neutral);
  EXPECT_EQ(parsed->options.white_balance.neutral,
            (ircore::Vector3{1.0, 0.5, 0.25}));
  EXPECT_EQ(parsed->options.compression, ircore::DngCompression::uncompressed);
  EXPECT_FALSE(parsed->options.embed_thumbnail);
  EXPECT_EQ(parsed->options.threads, 3U);
  EXPECT_TRUE(parsed->force);
  ASSERT_EQ(parsed->positional.size(), 2U);
  EXPECT_EQ(parsed->positional[0], "-odd-name.dng");
}

TEST(Cli, ParsesWhiteBalance) {
  const auto area = ircli::detail::parse_white_balance("area=10,20,30,40");
  ASSERT_TRUE(area.has_value());
  EXPECT_EQ(area->mode, ircore::WhiteBalanceMode::reference_area);
  EXPECT_EQ(area->area.x, 10U);
  EXPECT_EQ(area->area.height, 40U);
  EXPECT_TRUE(ircli::detail::parse_white_balance("as-shot").has_value());
  EXPECT_FALSE(ircli::detail::parse_white_balance("area=1,2,3,4,5").has_value());
  EXPECT_FALSE(ircli::detail::parse_white_balance("area=1,2,1,1").has_value());
  EXPECT_FALSE(ircli::detail::parse_white_balance("neutral=1,-1,1").has_value());
  EXPECT_FALSE(ircli::detail::parse_white_balance("neutral=1,x,1").has_value());
  EXPECT_FALSE(ircli::detail::parse_white_balance("daylight").has_value());
}

TEST(Cli, ProcessesSingleFile) {
  test_support::TemporaryDirectory directory;
  const auto input = directory.path() / "in.dng";
  const auto output = directory.path() / "out.dng";
  write_input(input);

  const auto outcome = run({input.string(), output.string(), "--filter", "720"});
  ASSERT_EQ(outcome.exit_code, ircli::exit_success) << outcome.error;
  EXPECT_NE(outcome.output.find("white balance: auto"), std::string::npos);
  EXPECT_NE(outcome.output.find("mapping:       false-color"),
            std::string::npos);
  const auto written = ircore::read_dng(output);
  ASSERT_TRUE(written.has_value());
  EXPECT_EQ(written->metadata.unique_camera_model, ircore::output_camera_model);

  // Existing outputs are protected unless --force is given.
  const auto refused = run({input.string(), output.string()});
  EXPECT_EQ(refused.exit_code, ircli::exit_usage);
  EXPECT_NE(refused.error.find("--force"), std::string::npos);
  const auto forced = run({"-q", "-f", input.string(), output.string()});
  EXPECT_EQ(forced.exit_code, ircli::exit_success) << forced.error;
  EXPECT_TRUE(forced.output.empty());

  const auto self = run({"-f", input.string(), input.string()});
  EXPECT_EQ(self.exit_code, ircli::exit_usage);
}

TEST(Cli, BatchIntoDirectory) {
  test_support::TemporaryDirectory directory;
  const auto a = directory.path() / "a.dng";
  const auto b = directory.path() / "b.DNG";
  write_input(a);
  write_input(b);
  const auto out = directory.path() / "out";

  const auto outcome =
      run({"--output-dir", out.string(), a.string(), b.string(), "-q"});
  ASSERT_EQ(outcome.exit_code, ircli::exit_success) << outcome.error;
  EXPECT_TRUE(std::filesystem::exists(out / "a.dng"));
  EXPECT_TRUE(std::filesystem::exists(out / "b.dng"));

  // The last positional argument may also be an existing directory.
  const auto out2 = directory.path() / "out2";
  std::filesystem::create_directory(out2);
  const auto implicit = run({"-q", a.string(), out2.string()});
  EXPECT_EQ(implicit.exit_code, ircli::exit_success) << implicit.error;
  EXPECT_TRUE(std::filesystem::exists(out2 / "a.dng"));

  // Two inputs with the same name would collide.
  std::filesystem::create_directory(directory.path() / "sub");
  const auto duplicate = directory.path() / "sub" / "a.dng";
  write_input(duplicate);
  const auto collision =
      run({"-o", out.string(), "-f", a.string(), duplicate.string()});
  EXPECT_EQ(collision.exit_code, ircli::exit_usage);
}

TEST(Cli, ReportsFailuresPerFile) {
  test_support::TemporaryDirectory directory;
  const auto good = directory.path() / "good.dng";
  write_input(good);
  const auto bad = directory.path() / "bad.dng";
  {
    std::ofstream stream(bad);
    stream << "not a dng";
  }
  const auto outcome = run({"-q", "-o", (directory.path() / "out").string(),
                            bad.string(), good.string()});
  EXPECT_EQ(outcome.exit_code, ircli::exit_failure);
  EXPECT_NE(outcome.error.find("bad.dng"), std::string::npos);
  EXPECT_NE(outcome.error.find("1 of 2 files failed"), std::string::npos);
  EXPECT_TRUE(std::filesystem::exists(directory.path() / "out" / "good.dng"));
}

TEST(Cli, AnalyzeWritesNothing) {
  test_support::TemporaryDirectory directory;
  const auto input = directory.path() / "in.dng";
  write_input(input);
  const auto outcome = run({"--analyze", input.string()});
  ASSERT_EQ(outcome.exit_code, ircli::exit_success) << outcome.error;
  EXPECT_NE(outcome.output.find("filter:"), std::string::npos);
  EXPECT_NE(outcome.output.find("detected"), std::string::npos);
  std::size_t files = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory.path())) {
    (void)entry;
    ++files;
  }
  EXPECT_EQ(files, 1U);
}

} // namespace
