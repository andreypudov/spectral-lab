#include "ircore/preview.h"
#include "ircore/color.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace ircore {

namespace {

float srgb_encode(double linear) {
  const double v = std::clamp(linear, 0.0, 1.0);
  const double encoded =
      v <= 0.0031308 ? 12.92 * v : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
  return static_cast<float>(encoded);
}

} // namespace

Rgb8Image render_preview(const RawImage &prophoto, std::uint32_t max_size,
                         double exposure_ev) {
  Rgb8Image preview;
  if (prophoto.samples_per_pixel != 3 || prophoto.width == 0 ||
      prophoto.height == 0 || max_size == 0 ||
      prophoto.samples.size() != prophoto.sample_count()) {
    return preview;
  }
  const std::uint32_t longest = std::max(prophoto.width, prophoto.height);
  const std::uint32_t factor = (longest + max_size - 1) / max_size;
  preview.width = (prophoto.width + factor - 1) / factor;
  preview.height = (prophoto.height + factor - 1) / factor;
  preview.pixels.resize(std::size_t{preview.width} * preview.height * 3);

  const Matrix3 to_srgb = xyz_d50_to_srgb() * prophoto_to_xyz_d50();
  const double gain = std::exp2(exposure_ev) / 65535.0;
  for (std::uint32_t y = 0; y < preview.height; ++y) {
    for (std::uint32_t x = 0; x < preview.width; ++x) {
      Vector3 sum{};
      std::size_t count = 0;
      const std::uint32_t row_end = std::min(prophoto.height, (y + 1) * factor);
      const std::uint32_t col_end = std::min(prophoto.width, (x + 1) * factor);
      for (std::uint32_t row = y * factor; row < row_end; ++row) {
        for (std::uint32_t col = x * factor; col < col_end; ++col) {
          for (std::size_t c = 0; c < 3; ++c) {
            sum[c] += prophoto.at(row, col, c);
          }
          ++count;
        }
      }
      for (double &value : sum) {
        value *= gain / static_cast<double>(count);
      }
      const Vector3 srgb = to_srgb * sum;
      for (std::size_t c = 0; c < 3; ++c) {
        preview.pixels[(std::size_t{y} * preview.width + x) * 3 + c] =
            static_cast<std::uint8_t>(std::lround(srgb_encode(srgb[c]) * 255.0F));
      }
    }
  }
  return preview;
}

} // namespace ircore
