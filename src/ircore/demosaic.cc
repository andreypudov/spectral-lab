#include "ircore/internal/demosaic.h"

#include <array>

namespace ircore {
namespace detail {

namespace {

std::ptrdiff_t mirror(std::ptrdiff_t index, std::ptrdiff_t size) {
  if (index < 0) {
    return -index;
  }
  if (index >= size) {
    return 2 * size - 2 - index;
  }
  return index;
}

} // namespace

void demosaic_row(const LinearImage &mosaic, std::size_t row,
                  std::span<float> rgb) {
  const auto width = static_cast<std::ptrdiff_t>(mosaic.width);
  const auto height = static_cast<std::ptrdiff_t>(mosaic.height);
  const auto r = static_cast<std::ptrdiff_t>(row);

  std::array<const float *, 5> rows{};
  for (std::ptrdiff_t d = -2; d <= 2; ++d) {
    rows[static_cast<std::size_t>(d + 2)] =
        mosaic.data.data() + mirror(r + d, height) * width;
  }

  for (std::ptrdiff_t c = 0; c < width; ++c) {
    std::array<std::ptrdiff_t, 5> cols{};
    for (std::ptrdiff_t d = -2; d <= 2; ++d) {
      cols[static_cast<std::size_t>(d + 2)] = mirror(c + d, width);
    }
    const auto p = [&](int dr, int dc) {
      return rows[static_cast<std::size_t>(dr + 2)]
                 [cols[static_cast<std::size_t>(dc + 2)]];
    };
    const float center = p(0, 0);
    const float cross1 = p(-1, 0) + p(1, 0) + p(0, -1) + p(0, 1);
    const float cross2 = p(-2, 0) + p(2, 0) + p(0, -2) + p(0, 2);
    const float diagonal = p(-1, -1) + p(-1, 1) + p(1, -1) + p(1, 1);

    const CfaColor color = mosaic.cfa_pattern.at(row, static_cast<std::size_t>(c));
    std::array<float, 3> out{};
    out[static_cast<std::size_t>(color)] = center;
    if (color == CfaColor::green) {
      const float horizontal =
          (5.0F * center + 4.0F * (p(0, -1) + p(0, 1)) - (p(0, -2) + p(0, 2)) -
           diagonal + 0.5F * (p(-2, 0) + p(2, 0))) /
          8.0F;
      const float vertical =
          (5.0F * center + 4.0F * (p(-1, 0) + p(1, 0)) - (p(-2, 0) + p(2, 0)) -
           diagonal + 0.5F * (p(0, -2) + p(0, 2))) /
          8.0F;
      const CfaColor h_color =
          mosaic.cfa_pattern.at(row, static_cast<std::size_t>(c) + 1);
      const CfaColor v_color =
          mosaic.cfa_pattern.at(row + 1, static_cast<std::size_t>(c));
      out[static_cast<std::size_t>(h_color)] = horizontal;
      out[static_cast<std::size_t>(v_color)] = vertical;
    } else {
      out[1] = (4.0F * center + 2.0F * cross1 - cross2) / 8.0F;
      const float opposite =
          (6.0F * center + 2.0F * diagonal - 1.5F * cross2) / 8.0F;
      out[color == CfaColor::red ? 2 : 0] = opposite;
    }
    const auto at = static_cast<std::size_t>(c) * 3;
    rgb[at] = out[0];
    rgb[at + 1] = out[1];
    rgb[at + 2] = out[2];
  }
}

} // namespace detail
} // namespace ircore
