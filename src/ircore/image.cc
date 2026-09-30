#include "ircore/image.h"

namespace ircore {

bool CfaPattern::is_bayer() const {
  int red = 0;
  int green = 0;
  int blue = 0;
  for (const auto &row : colors) {
    for (const CfaColor color : row) {
      red += color == CfaColor::red ? 1 : 0;
      green += color == CfaColor::green ? 1 : 0;
      blue += color == CfaColor::blue ? 1 : 0;
    }
  }
  if (red != 1 || green != 2 || blue != 1) {
    return false;
  }
  // With a single red and blue, a matching diagonal must hold both greens.
  return colors[0][0] == colors[1][1] || colors[0][1] == colors[1][0];
}

CfaPattern CfaPattern::shifted(std::size_t row_offset,
                               std::size_t col_offset) const {
  CfaPattern result;
  for (std::size_t row = 0; row < 2; ++row) {
    for (std::size_t col = 0; col < 2; ++col) {
      result.colors[row][col] = at(row + row_offset, col + col_offset);
    }
  }
  return result;
}

} // namespace ircore
