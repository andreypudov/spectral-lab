#pragma once

#include <expected>
#include <string>
#include <string_view>

namespace ircore {

enum class ErrorCode {
  io_error,
  invalid_format,
  unsupported_format,
  invalid_argument,
  processing_failed,
  out_of_memory,
};

struct Error {
  ErrorCode code{ErrorCode::processing_failed};
  std::string message;
};

template <typename T> using Result = std::expected<T, Error>;

[[nodiscard]] std::unexpected<Error> make_error(ErrorCode code,
                                                std::string message);

[[nodiscard]] std::string_view to_string(ErrorCode code);

} // namespace ircore
