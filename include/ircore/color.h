#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace ircore {

using Vector3 = std::array<double, 3>;

struct Matrix3 {
  std::array<std::array<double, 3>, 3> m{};

  [[nodiscard]] static Matrix3 identity();
  [[nodiscard]] static Matrix3 diagonal(const Vector3 &values);
  [[nodiscard]] static Matrix3 from_row_major(const std::array<double, 9> &v);

  [[nodiscard]] double operator()(std::size_t row, std::size_t col) const {
    return m[row][col];
  }
  [[nodiscard]] double &operator()(std::size_t row, std::size_t col) {
    return m[row][col];
  }
};

[[nodiscard]] Matrix3 operator*(const Matrix3 &a, const Matrix3 &b);
[[nodiscard]] Vector3 operator*(const Matrix3 &a, const Vector3 &v);

[[nodiscard]] std::optional<Matrix3> inverse(const Matrix3 &matrix);
[[nodiscard]] double max_component(const Vector3 &v);
[[nodiscard]] double min_component(const Vector3 &v);

struct Chromaticity {
  double x{};
  double y{};
};

// XYZ of a white point with Y = 1.
[[nodiscard]] Vector3 xy_to_xyz(Chromaticity xy);

namespace white {
inline constexpr Chromaticity d50{0.3457, 0.3585};
inline constexpr Chromaticity d55{0.3324, 0.3474};
inline constexpr Chromaticity d65{0.3127, 0.3290};
inline constexpr Chromaticity d75{0.2990, 0.3149};
inline constexpr Chromaticity std_a{0.4476, 0.4074};
} // namespace white

// Bradford chromatic adaptation from one white point to another.
[[nodiscard]] Matrix3 bradford_adaptation(Chromaticity from, Chromaticity to);

// RGB to XYZ matrix of an RGB space defined by its primaries and white.
[[nodiscard]] Matrix3 rgb_to_xyz(Chromaticity red, Chromaticity green,
                                 Chromaticity blue, Chromaticity white_point);

// Linear ProPhoto (ROMM) RGB, D50 native white.
[[nodiscard]] Matrix3 prophoto_to_xyz_d50();
[[nodiscard]] Matrix3 xyz_d50_to_prophoto();

// Linear sRGB / Rec.709 primaries, Bradford-adapted to D50 (ICC style).
[[nodiscard]] Matrix3 srgb_to_xyz_d50();
[[nodiscard]] Matrix3 xyz_d50_to_srgb();

// EXIF / DNG LightSource codes used by CalibrationIlluminant tags.
namespace light_source {
inline constexpr std::uint16_t unknown = 0;
inline constexpr std::uint16_t daylight = 1;
inline constexpr std::uint16_t standard_a = 17;
inline constexpr std::uint16_t d55 = 20;
inline constexpr std::uint16_t d65 = 21;
inline constexpr std::uint16_t d75 = 22;
inline constexpr std::uint16_t d50 = 23;
} // namespace light_source

// White chromaticity of a DNG calibration illuminant. Unknown and "other"
// illuminants are treated as D65, which is the most common second
// calibration illuminant.
[[nodiscard]] Chromaticity illuminant_white(std::uint16_t light_source);

// Approximate correlated color temperature in kelvin of a DNG illuminant.
[[nodiscard]] double illuminant_temperature(std::uint16_t light_source);

} // namespace ircore
