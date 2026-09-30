#include "ircore/internal/demosaic.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {

using ircore::CfaColor;
using ircore::CfaPattern;
using ircore::LinearImage;

constexpr CfaColor R = CfaColor::red;
constexpr CfaColor G = CfaColor::green;
constexpr CfaColor B = CfaColor::blue;

std::vector<CfaPattern> all_bayer_patterns() {
  std::vector<CfaPattern> patterns(4);
  patterns[0].colors = {{{R, G}, {G, B}}};
  patterns[1].colors = {{{B, G}, {G, R}}};
  patterns[2].colors = {{{G, R}, {B, G}}};
  patterns[3].colors = {{{G, B}, {R, G}}};
  return patterns;
}

template <typename Scene>
LinearImage mosaic(std::uint32_t width, std::uint32_t height,
                   const CfaPattern &pattern, Scene scene) {
  LinearImage image;
  image.width = width;
  image.height = height;
  image.channels = 1;
  image.cfa_pattern = pattern;
  image.data.resize(std::size_t{width} * height);
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto rgb = scene(x, y);
      image.data[std::size_t{y} * width + x] =
          rgb[static_cast<std::size_t>(pattern.at(y, x))];
    }
  }
  return image;
}

TEST(Demosaic, FlatFieldIsReproducedExactlyForEveryPattern) {
  for (const auto &pattern : all_bayer_patterns()) {
    const auto image = mosaic(9, 7, pattern, [](std::uint32_t, std::uint32_t) {
      return std::array<float, 3>{0.25F, 0.5F, 0.75F};
    });
    std::vector<float> rgb(9 * 3);
    for (std::size_t row = 0; row < 7; ++row) {
      ircore::detail::demosaic_row(image, row, rgb);
      for (std::size_t col = 0; col < 9; ++col) {
        EXPECT_NEAR(rgb[col * 3], 0.25F, 1e-6);
        EXPECT_NEAR(rgb[col * 3 + 1], 0.5F, 1e-6);
        EXPECT_NEAR(rgb[col * 3 + 2], 0.75F, 1e-6);
      }
    }
  }
}

TEST(Demosaic, KeepsMeasuredSamplesAndFollowsLinearGradients) {
  const auto scene = [](std::uint32_t x, std::uint32_t y) {
    return std::array<float, 3>{0.01F * static_cast<float>(x),
                                0.2F + 0.005F * static_cast<float>(x + y),
                                0.9F - 0.01F * static_cast<float>(y)};
  };
  for (const auto &pattern : all_bayer_patterns()) {
    const auto image = mosaic(40, 30, pattern, scene);
    std::vector<float> rgb(40 * 3);
    for (std::uint32_t row = 0; row < 30; ++row) {
      ircore::detail::demosaic_row(image, row, rgb);
      for (std::uint32_t col = 0; col < 40; ++col) {
        const auto expected = scene(col, row);
        const auto measured = static_cast<std::size_t>(pattern.at(row, col));
        EXPECT_EQ(rgb[col * 3 + measured],
                  image.data[std::size_t{row} * 40 + col]);
        // Interior pixels of a linear ramp are interpolated exactly.
        if (row >= 2 && row < 28 && col >= 2 && col < 38) {
          for (std::size_t c = 0; c < 3; ++c) {
            EXPECT_NEAR(rgb[col * 3 + c], expected[c], 1e-5)
                << "row " << row << " col " << col << " channel " << c;
          }
        }
      }
    }
  }
}

} // namespace
