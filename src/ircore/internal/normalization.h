#pragma once

#include "ircore/color.h"
#include "ircore/dng.h"
#include "ircore/error.h"
#include "ircore/options.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ircore {
namespace detail {

struct CameraTransform {
  // White balanced camera RGB (neutral = 1, 1, 1) to XYZ D50.
  Matrix3 balanced_to_xyz;
  std::uint16_t illuminant{light_source::unknown};
  bool used_forward_matrix{false};
  std::vector<std::string> warnings;
};

// The calibration set whose illuminant is closest to D65 (in mired). IR
// photographs are taken in daylight, so the daylight matrices describe the
// sensor best.
[[nodiscard]] const ColorCalibration *
reference_calibration(const DngMetadata &meta);

// Camera transform that renders `camera_neutral` as D50 white while using
// the camera matrices of the reference (daylight) calibration.
[[nodiscard]] Result<CameraTransform>
camera_transform(const DngMetadata &meta, const Vector3 &camera_neutral);

// White balanced camera RGB to linear ProPhoto RGB for a resolved channel
// mapping. The result maps (1, 1, 1) exactly to (1, 1, 1).
[[nodiscard]] Result<Matrix3> output_transform(const Matrix3 &balanced_to_xyz,
                                               ChannelMapping mapping);

// Brings a linear ProPhoto color into [0, 1] by moving it towards the gray
// of equal luminance, which keeps luminance and hue where possible.
void gamut_map(std::array<float, 3> &rgb);

// Camera neutral of the `as_shot_white_xy` chromaticity.
[[nodiscard]] Result<Vector3> neutral_from_white_xy(const DngMetadata &meta,
                                                    Chromaticity xy);

} // namespace detail
} // namespace ircore
