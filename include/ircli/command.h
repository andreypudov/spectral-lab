#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace ircli {

inline constexpr int exit_success = 0;
inline constexpr int exit_failure = 1;
inline constexpr int exit_usage = 2;

// Runs the spectrallab command line. Returns the process exit code.
[[nodiscard]] int run(std::span<const std::string_view> arguments,
                      std::ostream &output, std::ostream &error);

} // namespace ircli
