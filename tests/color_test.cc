#include "ircore/color.h"

#include <gtest/gtest.h>

namespace {

using ircore::Matrix3;
using ircore::Vector3;

void expect_near(const Vector3 &actual, const Vector3 &expected,
                 double tolerance) {
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_NEAR(actual[i], expected[i], tolerance) << "component " << i;
  }
}

TEST(Color, InverseOfProductIsIdentity) {
  const Matrix3 m = Matrix3::from_row_major(
      {0.7662, -0.3686, -0.0183, -0.5051, 1.2914, 0.2433, -0.1003, 0.2025,
       0.6010});
  const auto inv = ircore::inverse(m);
  ASSERT_TRUE(inv.has_value());
  const Matrix3 product = m * *inv;
  for (std::size_t r = 0; r < 3; ++r) {
    for (std::size_t c = 0; c < 3; ++c) {
      EXPECT_NEAR(product(r, c), r == c ? 1.0 : 0.0, 1e-12);
    }
  }
}

TEST(Color, SingularMatrixHasNoInverse) {
  const Matrix3 m =
      Matrix3::from_row_major({1.0, 2.0, 3.0, 2.0, 4.0, 6.0, 0.0, 1.0, 1.0});
  EXPECT_FALSE(ircore::inverse(m).has_value());
  EXPECT_FALSE(ircore::inverse(Matrix3{}).has_value());
}

TEST(Color, BradfordMapsSourceWhiteToTargetWhite) {
  const Matrix3 adapt =
      ircore::bradford_adaptation(ircore::white::d65, ircore::white::d50);
  expect_near(adapt * ircore::xy_to_xyz(ircore::white::d65),
              ircore::xy_to_xyz(ircore::white::d50), 1e-12);

  const Matrix3 same =
      ircore::bradford_adaptation(ircore::white::d50, ircore::white::d50);
  expect_near(same * Vector3{0.2, 0.5, 0.7}, {0.2, 0.5, 0.7}, 1e-12);
}

TEST(Color, ProPhotoWhiteIsD50) {
  expect_near(ircore::prophoto_to_xyz_d50() * Vector3{1.0, 1.0, 1.0},
              ircore::xy_to_xyz(ircore::white::d50), 1e-9);
  // Well-known luminance row of ROMM RGB.
  const Matrix3 m = ircore::prophoto_to_xyz_d50();
  EXPECT_NEAR(m(1, 0), 0.2880, 1e-3);
  EXPECT_NEAR(m(1, 1), 0.7119, 1e-3);
  EXPECT_NEAR(m(1, 2), 0.0001, 1e-3);
}

TEST(Color, SrgbD50MatrixMatchesIccValues) {
  const Matrix3 m = ircore::srgb_to_xyz_d50();
  // ICC sRGB profile colorants (Bradford adapted to D50).
  EXPECT_NEAR(m(0, 0), 0.4361, 2e-3);
  EXPECT_NEAR(m(0, 1), 0.3851, 2e-3);
  EXPECT_NEAR(m(0, 2), 0.1431, 2e-3);
  EXPECT_NEAR(m(1, 0), 0.2225, 2e-3);
  EXPECT_NEAR(m(1, 1), 0.7169, 2e-3);
  EXPECT_NEAR(m(1, 2), 0.0606, 2e-3);
  EXPECT_NEAR(m(2, 0), 0.0139, 2e-3);
  EXPECT_NEAR(m(2, 1), 0.0971, 2e-3);
  EXPECT_NEAR(m(2, 2), 0.7142, 2e-3);
}

TEST(Color, IlluminantTable) {
  EXPECT_DOUBLE_EQ(ircore::illuminant_temperature(ircore::light_source::d65),
                   6500.0);
  EXPECT_DOUBLE_EQ(
      ircore::illuminant_temperature(ircore::light_source::standard_a), 2850.0);
  const auto d50 = ircore::illuminant_white(ircore::light_source::d50);
  EXPECT_DOUBLE_EQ(d50.x, ircore::white::d50.x);
  // Fluorescent illuminants come from the Planckian locus.
  const auto cool_white = ircore::illuminant_white(14);
  EXPECT_NEAR(cool_white.x, 0.372, 0.01);
  EXPECT_NEAR(cool_white.y, 0.375, 0.01);
}

} // namespace
