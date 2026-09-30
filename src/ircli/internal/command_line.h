#pragma once

#include "ircore/options.h"

#include <expected>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ircli {
namespace detail {

struct CommandLine {
  bool help{false};
  bool version{false};
  bool analyze{false};
  bool force{false};
  bool quiet{false};
  std::optional<std::filesystem::path> output_dir;
  std::vector<std::filesystem::path> positional;
  ircore::ProcessingOptions options;
};

struct Job {
  std::filesystem::path input;
  // Empty for --analyze.
  std::filesystem::path output;
};

// Parses arguments; the error is a message for the user.
[[nodiscard]] std::expected<CommandLine, std::string>
parse_command_line(std::span<const std::string_view> arguments);

// Parses the value of --wb.
[[nodiscard]] std::expected<ircore::WhiteBalanceSettings, std::string>
parse_white_balance(std::string_view text);

// Expands the command line into input/output pairs and validates them.
[[nodiscard]] std::expected<std::vector<Job>, std::string>
plan_jobs(const CommandLine &command_line);

} // namespace detail
} // namespace ircli
