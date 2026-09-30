#include "ircore/internal/normalization.h"

#include "support/synthetic.h"

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

const Vector3 ir_neutral{1.0, 0.58, 0.47};

TEST(CameraTransform, PicksTheDaylightCalibration) {
  const auto meta = test_support::camera_metadata({});
  const auto *reference = ircore::detail::reference_calibration(meta);
  ASSERT_NE(reference, nullptr);
  EXPECT_EQ(reference->illuminant, ircore::light_source::d65);
}

TEST(CameraTransform, MapsTheIrNeutralToD50WithColorMatrix) {
  const auto meta = test_support::camera_metadata({});
  const auto transform = ircore::detail::camera_transform(meta, ir_neutral);
  ASSERT_TRUE(transform.has_value()) << transform.error().message;
  EXPECT_FALSE(transform->used_forward_matrix);
  EXPECT_EQ(transform->illuminant, ircore::light_source::d65);
  expect_near(transform->balanced_to_xyz * Vector3{1.0, 1.0, 1.0},
              ircore::xy_to_xyz(ircore::white::d50), 1e-9);
}

TEST(CameraTransform, UsesForwardMatrixAndCalibration) {
  auto meta = test_support::camera_metadata({});
  // A forward matrix whose rows sum to D50, as the DNG spec requires.
  const Matrix3 forward = Matrix3::from_row_major(
      {0.6, 0.25, 0.1142, 0.25, 0.8, -0.05, 0.02, -0.2, 1.0049});
  meta.calibrations[1].forward_matrix = forward;
  meta.calibrations[1].camera_calibration =
      Matrix3::diagonal({1.05, 1.0, 0.97});
  meta.analog_balance = Vector3{1.1, 1.0, 0.9};

  const auto transform = ircore::detail::camera_transform(meta, ir_neutral);
  ASSERT_TRUE(transform.has_value());
  EXPECT_TRUE(transform->used_forward_matrix);
  expect_near(transform->balanced_to_xyz * Vector3{1.0, 1.0, 1.0},
              forward * Vector3{1.0, 1.0, 1.0}, 1e-9);
}

TEST(CameraTransform, FallsBackToSrgbWithoutMatrices) {
  ircore::DngMetadata meta;
  const auto transform = ircore::detail::camera_transform(meta, ir_neutral);
  ASSERT_TRUE(transform.has_value());
  EXPECT_FALSE(transform->warnings.empty());
  expect_near(transform->balanced_to_xyz * Vector3{1.0, 1.0, 1.0},
              ircore::xy_to_xyz(ircore::white::d50), 1e-9);
}

class OutputTransform
    : public ::testing::TestWithParam<ircore::ChannelMapping> {};

TEST_P(OutputTransform, NeutralStaysExactlyWhite) {
  const auto meta = test_support::camera_metadata({});
  const auto camera = ircore::detail::camera_transform(meta, ir_neutral);
  ASSERT_TRUE(camera.has_value());
  const auto m =
      ircore::detail::output_transform(camera->balanced_to_xyz, GetParam());
  ASSERT_TRUE(m.has_value()) << m.error().message;
  expect_near(*m * Vector3{1.0, 1.0, 1.0}, {1.0, 1.0, 1.0}, 1e-12);
  expect_near(*m * Vector3{0.3, 0.3, 0.3}, {0.3, 0.3, 0.3}, 1e-12);
}

INSTANTIATE_TEST_SUITE_P(Mappings, OutputTransform,
                         ::testing::Values(ircore::ChannelMapping::native,
                                           ircore::ChannelMapping::false_color,
                                           ircore::ChannelMapping::monochrome));

TEST(OutputTransformMapping, FalseColorSwapsRedAndBlueInSrgb) {
  // With an sRGB camera the swap must turn sRGB red into sRGB blue.
  const Matrix3 camera = ircore::srgb_to_xyz_d50();
  const auto native =
      ircore::detail::output_transform(camera, ircore::ChannelMapping::native);
  const auto swapped = ircore::detail::output_transform(
      camera, ircore::ChannelMapping::false_color);
  ASSERT_TRUE(native.has_value());
  ASSERT_TRUE(swapped.has_value());
  const Matrix3 to_srgb =
      ircore::xyz_d50_to_srgb() * ircore::prophoto_to_xyz_d50();
  expect_near(to_srgb * (*native * Vector3{0.8, 0.2, 0.1}), {0.8, 0.2, 0.1},
              1e-9);
  expect_near(to_srgb * (*swapped * Vector3{0.8, 0.2, 0.1}), {0.1, 0.2, 0.8},
              1e-9);
}

TEST(OutputTransformMapping, MonochromeAveragesBalancedChannels) {
  const auto m = ircore::detail::output_transform(
      Matrix3::identity(), ircore::ChannelMapping::monochrome);
  ASSERT_TRUE(m.has_value());
  expect_near(*m * Vector3{0.3, 0.6, 0.9}, {0.6, 0.6, 0.6}, 1e-12);
  EXPECT_FALSE(ircore::detail::output_transform(
                   Matrix3::identity(), ircore::ChannelMapping::auto_select)
                   .has_value());
}

TEST(GamutMap, KeepsInGamutColorsAndLuminance) {
  std::array<float, 3> inside{0.2F, 0.5F, 0.7F};
  ircore::detail::gamut_map(inside);
  EXPECT_FLOAT_EQ(inside[0], 0.2F);
  EXPECT_FLOAT_EQ(inside[1], 0.5F);
  EXPECT_FLOAT_EQ(inside[2], 0.7F);

  const auto luminance = [](const std::array<float, 3> &c) {
    return 0.2880402F * c[0] + 0.7118741F * c[1] + 0.0000857F * c[2];
  };
  std::array<float, 3> negative{-0.2F, 0.4F, 0.6F};
  const float y = luminance(negative);
  ircore::detail::gamut_map(negative);
  EXPECT_GE(std::min({negative[0], negative[1], negative[2]}), 0.0F);
  EXPECT_NEAR(luminance(negative), y, 1e-6);
  EXPECT_NEAR(negative[0], 0.0F, 1e-6);

  std::array<float, 3> bright{1.4F, 0.6F, 0.3F};
  const float bright_y = luminance(bright);
  ircore::detail::gamut_map(bright);
  EXPECT_LE(std::max({bright[0], bright[1], bright[2]}), 1.0F);
  EXPECT_NEAR(luminance(bright), bright_y, 1e-6);

  std::array<float, 3> dark{-0.5F, -0.1F, 0.2F};
  ircore::detail::gamut_map(dark);
  EXPECT_EQ(dark, (std::array<float, 3>{0.0F, 0.0F, 0.0F}));
  std::array<float, 3> white{1.5F, 1.2F, 1.1F};
  ircore::detail::gamut_map(white);
  EXPECT_EQ(white, (std::array<float, 3>{1.0F, 1.0F, 1.0F}));
}

} // namespace
