#pragma once

#include "ircore/error.h"

#include <cstdint>
#include <span>
#include <vector>

namespace ircore {
namespace detail {

// Lossless JPEG (ITU T.81, Huffman coded, process 14) as used by DNG
// compression 7.
struct LosslessJpegImage {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t components{};
  std::uint32_t precision{};
  // Row-major, component-interleaved samples.
  std::vector<std::uint16_t> samples;
};

[[nodiscard]] Result<LosslessJpegImage>
decode_lossless_jpeg(std::span<const std::uint8_t> stream);

struct LosslessJpegEncodeParams {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t components{1};
  std::uint32_t precision{16};
  // Prediction selection value, 1 to 7.
  std::uint32_t predictor{1};
};

[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_lossless_jpeg(std::span<const std::uint16_t> samples,
                     const LosslessJpegEncodeParams &params);

} // namespace detail
} // namespace ircore
