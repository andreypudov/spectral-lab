#pragma once

#include "ircore/image.h"

#include <cstdint>

namespace ircore {

// Renders a small sRGB preview of a 3-sample linear ProPhoto RGB image
// (0 black, 65535 white). The long edge is at most `max_size` pixels.
// `exposure_ev` is added before the sRGB transfer curve.
[[nodiscard]] Rgb8Image render_preview(const RawImage &prophoto,
                                       std::uint32_t max_size,
                                       double exposure_ev = 0.0);

} // namespace ircore
