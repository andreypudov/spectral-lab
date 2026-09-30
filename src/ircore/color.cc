#include "ircore/color.h"

#include <algorithm>
#include <cmath>

namespace ircore {

Matrix3 Matrix3::identity() { return diagonal({1.0, 1.0, 1.0}); }

Matrix3 Matrix3::diagonal(const Vector3 &values) {
  Matrix3 result;
  for (std::size_t i = 0; i < 3; ++i) {
    result.m[i][i] = values[i];
  }
  return result;
}

Matrix3 Matrix3::from_row_major(const std::array<double, 9> &v) {
  Matrix3 result;
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t col = 0; col < 3; ++col) {
      result.m[row][col] = v[row * 3 + col];
    }
  }
  return result;
}

Matrix3 operator*(const Matrix3 &a, const Matrix3 &b) {
  Matrix3 result;
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t col = 0; col < 3; ++col) {
      double sum = 0.0;
      for (std::size_t k = 0; k < 3; ++k) {
        sum += a.m[row][k] * b.m[k][col];
      }
      result.m[row][col] = sum;
    }
  }
  return result;
}

Vector3 operator*(const Matrix3 &a, const Vector3 &v) {
  Vector3 result{};
  for (std::size_t row = 0; row < 3; ++row) {
    result[row] =
        a.m[row][0] * v[0] + a.m[row][1] * v[1] + a.m[row][2] * v[2];
  }
  return result;
}

std::optional<Matrix3> inverse(const Matrix3 &matrix) {
  const auto &a = matrix.m;
  const double c00 = a[1][1] * a[2][2] - a[1][2] * a[2][1];
  const double c01 = a[1][2] * a[2][0] - a[1][0] * a[2][2];
  const double c02 = a[1][0] * a[2][1] - a[1][1] * a[2][0];
  const double det = a[0][0] * c00 + a[0][1] * c01 + a[0][2] * c02;

  double scale = 0.0;
  for (const auto &row : a) {
    for (const double value : row) {
      scale = std::max(scale, std::abs(value));
    }
  }
  if (!std::isfinite(det) || scale == 0.0 ||
      std::abs(det) < 1e-12 * scale * scale * scale) {
    return std::nullopt;
  }

  Matrix3 result;
  auto &r = result.m;
  r[0][0] = c00 / det;
  r[1][0] = c01 / det;
  r[2][0] = c02 / det;
  r[0][1] = (a[0][2] * a[2][1] - a[0][1] * a[2][2]) / det;
  r[1][1] = (a[0][0] * a[2][2] - a[0][2] * a[2][0]) / det;
  r[2][1] = (a[0][1] * a[2][0] - a[0][0] * a[2][1]) / det;
  r[0][2] = (a[0][1] * a[1][2] - a[0][2] * a[1][1]) / det;
  r[1][2] = (a[0][2] * a[1][0] - a[0][0] * a[1][2]) / det;
  r[2][2] = (a[0][0] * a[1][1] - a[0][1] * a[1][0]) / det;
  return result;
}

double max_component(const Vector3 &v) { return std::max({v[0], v[1], v[2]}); }

double min_component(const Vector3 &v) { return std::min({v[0], v[1], v[2]}); }

Vector3 xy_to_xyz(Chromaticity xy) {
  return {xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y};
}

Matrix3 bradford_adaptation(Chromaticity from, Chromaticity to) {
  const Matrix3 bradford = Matrix3::from_row_major(
      {0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685,
       1.0296});
  const Matrix3 bradford_inverse = *inverse(bradford);
  const Vector3 cone_from = bradford * xy_to_xyz(from);
  const Vector3 cone_to = bradford * xy_to_xyz(to);
  const Matrix3 scale = Matrix3::diagonal({cone_to[0] / cone_from[0],
                                           cone_to[1] / cone_from[1],
                                           cone_to[2] / cone_from[2]});
  return bradford_inverse * scale * bradford;
}

Matrix3 rgb_to_xyz(Chromaticity red, Chromaticity green, Chromaticity blue,
                   Chromaticity white_point) {
  const Vector3 r = xy_to_xyz(red);
  const Vector3 g = xy_to_xyz(green);
  const Vector3 b = xy_to_xyz(blue);
  Matrix3 primaries;
  for (std::size_t row = 0; row < 3; ++row) {
    primaries.m[row] = {r[row], g[row], b[row]};
  }
  // Primaries are linearly independent for any valid RGB space.
  const Vector3 scale = *inverse(primaries) * xy_to_xyz(white_point);
  return primaries * Matrix3::diagonal(scale);
}

Matrix3 prophoto_to_xyz_d50() {
  return rgb_to_xyz({0.734699, 0.265301}, {0.159597, 0.840403},
                    {0.036598, 0.000105}, white::d50);
}

Matrix3 xyz_d50_to_prophoto() { return *inverse(prophoto_to_xyz_d50()); }

Matrix3 srgb_to_xyz_d50() {
  return bradford_adaptation(white::d65, white::d50) *
         rgb_to_xyz({0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}, white::d65);
}

Matrix3 xyz_d50_to_srgb() { return *inverse(srgb_to_xyz_d50()); }

namespace {

// Planckian locus approximation (Kang et al., 2002), 1667 K to 25000 K.
Chromaticity planckian_xy(double kelvin) {
  const double t = std::clamp(kelvin, 1667.0, 25000.0);
  const double t2 = t * t;
  const double t3 = t2 * t;
  const double x = t <= 4000.0 ? -0.2661239e9 / t3 - 0.2343589e6 / t2 +
                                     0.8776956e3 / t + 0.179910
                               : -3.0258469e9 / t3 + 2.1070379e6 / t2 +
                                     0.2226347e3 / t + 0.240390;
  const double x2 = x * x;
  const double x3 = x2 * x;
  double y = 0.0;
  if (t <= 2222.0) {
    y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
  } else if (t <= 4000.0) {
    y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
  } else {
    y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
  }
  return {x, y};
}

} // namespace

// Illuminant groups follow the DNG SDK (dng_camera_profile.cpp).
Chromaticity illuminant_white(std::uint16_t light_source) {
  switch (light_source) {
  case 3:  // Tungsten
  case 17: // Standard light A
    return white::std_a;
  case 23: // D50
    return white::d50;
  case 1:  // Daylight
  case 4:  // Flash
  case 9:  // Fine weather
  case 18: // Standard light B
  case 20: // D55
    return white::d55;
  case 10: // Cloudy weather
  case 19: // Standard light C
  case 21: // D65
    return white::d65;
  case 11: // Shade
  case 22: // D75
    return white::d75;
  case 0:   // Unknown
  case 255: // Other
    return white::d65;
  default:
    return planckian_xy(illuminant_temperature(light_source));
  }
}

double illuminant_temperature(std::uint16_t light_source) {
  switch (light_source) {
  case 3:
  case 17:
    return 2850.0;
  case 24: // ISO studio tungsten
    return 3200.0;
  case 23:
    return 5000.0;
  case 1:
  case 4:
  case 9:
  case 18:
  case 20:
    return 5500.0;
  case 10:
  case 19:
  case 21:
    return 6500.0;
  case 11:
  case 22:
    return 7500.0;
  case 12: // Daylight fluorescent
    return 6430.0;
  case 13: // Day white fluorescent
    return 5000.0;
  case 2:  // Fluorescent
  case 14: // Cool white fluorescent
    return 4150.0;
  case 15: // White fluorescent
    return 3450.0;
  case 16: // Warm white fluorescent
    return 2940.0;
  default:
    return 6500.0;
  }
}

} // namespace ircore
