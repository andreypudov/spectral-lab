#include "ircore/error.h"

#include <utility>

namespace ircore {

std::unexpected<Error> make_error(ErrorCode code, std::string message) {
  return std::unexpected(Error{code, std::move(message)});
}

std::string_view to_string(ErrorCode code) {
  switch (code) {
  case ErrorCode::io_error:
    return "I/O error";
  case ErrorCode::invalid_format:
    return "invalid format";
  case ErrorCode::unsupported_format:
    return "unsupported format";
  case ErrorCode::invalid_argument:
    return "invalid argument";
  case ErrorCode::processing_failed:
    return "processing failed";
  case ErrorCode::out_of_memory:
    return "out of memory";
  }
  return "unknown error";
}

} // namespace ircore
