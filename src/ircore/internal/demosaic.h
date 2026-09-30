#pragma once

#include "ircore/analysis.h"

#include <cstddef>
#include <span>

namespace ircore {
namespace detail {

// Interpolates one row of a Bayer mosaic into RGB triplets with the
// gradient-corrected linear filter of Malvar, He and Cutler (2004). Pixels
// beyond the border are mirrored, which preserves the CFA phase. The mosaic
// must be at least 3x3 pixels and `rgb` must hold width * 3 values.
void demosaic_row(const LinearImage &mosaic, std::size_t row,
                  std::span<float> rgb);

} // namespace detail
} // namespace ircore
