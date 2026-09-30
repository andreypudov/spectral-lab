#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ircore {

enum class Photometric {
  // Single-sample color filter array mosaic (PhotometricInterpretation 32803).
  cfa,
  // Demosaiced linear camera RGB (PhotometricInterpretation 34892).
  linear_raw,
};

enum class CfaColor : std::uint8_t { red = 0, green = 1, blue = 2 };

// 2x2 Bayer pattern. Row and column 0 are the top-left pixel of the image the
// pattern belongs to.
struct CfaPattern {
  std::array<std::array<CfaColor, 2>, 2> colors{
      {{CfaColor::red, CfaColor::green}, {CfaColor::green, CfaColor::blue}}};

  [[nodiscard]] CfaColor at(std::size_t row, std::size_t col) const {
    return colors[row & 1U][col & 1U];
  }

  // True for the four standard Bayer layouts (one red, two greens diagonal to
  // each other, one blue).
  [[nodiscard]] bool is_bayer() const;

  // Pattern of the same mosaic when the image origin is moved by the given
  // offset (for example when cropping to the active area).
  [[nodiscard]] CfaPattern shifted(std::size_t row_offset,
                                   std::size_t col_offset) const;

  friend bool operator==(const CfaPattern &, const CfaPattern &) = default;
};

// Stored (not yet linearized) raw samples, row-major and pixel-interleaved.
struct RawImage {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t samples_per_pixel{1};
  std::vector<std::uint16_t> samples;

  [[nodiscard]] std::size_t sample_count() const {
    return static_cast<std::size_t>(width) * height * samples_per_pixel;
  }
  [[nodiscard]] std::uint16_t at(std::size_t row, std::size_t col,
                                 std::size_t sample = 0) const {
    return samples[(row * width + col) * samples_per_pixel + sample];
  }
};

// 8-bit sRGB image, row-major RGB triplets.
struct Rgb8Image {
  std::uint32_t width{};
  std::uint32_t height{};
  std::vector<std::uint8_t> pixels;
};

} // namespace ircore
