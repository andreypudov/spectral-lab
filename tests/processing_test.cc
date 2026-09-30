#include "ircore/processing.h"

#include "support/synthetic.h"

#include <gtest/gtest.h>

#include <cmath>
#include <random>

namespace {

using ircore::ChannelMapping;
using ircore::FilterType;
using ircore::Vector3;
using test_support::make_cfa_dng;
using test_support::uniform_scene;

// Raw response of foliage-like IR reflectors through a 720 nm filter.
const Vector3 ir_response{1.0, 0.6, 0.5};

// Mostly IR-neutral scene with a patch that reflects relatively more in the
// red channel, like sky or water in false color work.
ircore::DngImage patch_scene(std::uint32_t size = 64) {
  return make_cfa_dng(size, size, [size](std::uint32_t x, std::uint32_t y) {
    const bool patch = x >= size / 4 && x < size / 2 && y >= size / 4 &&
                       y < size / 2;
    const double level = 0.4;
    if (patch) {
      return Vector3{level * 1.25, level * 0.6, level * 0.4};
    }
    return Vector3{level * ir_response[0], level * ir_response[1],
                   level * ir_response[2]};
  });
}

Vector3 pixel(const ircore::RawImage &image, std::uint32_t x, std::uint32_t y) {
  return {image.at(y, x, 0) / 65535.0, image.at(y, x, 1) / 65535.0,
          image.at(y, x, 2) / 65535.0};
}

Vector3 to_srgb(const Vector3 &prophoto) {
  return ircore::xyz_d50_to_srgb() * (ircore::prophoto_to_xyz_d50() * prophoto);
}

ircore::ProcessingOptions options_for(ChannelMapping mapping) {
  ircore::ProcessingOptions options;
  options.mapping = mapping;
  options.filter = FilterType::nm720;
  return options;
}

class NeutralOutput : public ::testing::TestWithParam<ChannelMapping> {};

TEST_P(NeutralOutput, IrNeutralBecomesGray) {
  const auto dng = make_cfa_dng(48, 40, uniform_scene({0.5, 0.3, 0.25}));
  const auto result = ircore::normalize(dng, options_for(GetParam()));
  ASSERT_TRUE(result.has_value()) << result.error().message;
  const auto &image = result->dng.image;
  EXPECT_EQ(image.samples_per_pixel, 3U);
  for (std::uint32_t y = 0; y < image.height; y += 7) {
    for (std::uint32_t x = 0; x < image.width; x += 5) {
      const Vector3 rgb = pixel(image, x, y);
      EXPECT_NEAR(rgb[0], rgb[1], 2e-3) << x << "," << y;
      EXPECT_NEAR(rgb[2], rgb[1], 2e-3) << x << "," << y;
      // The neutral is normalized so that its largest channel maps to itself.
      EXPECT_NEAR(rgb[1], 0.5, 2e-3);
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Mappings, NeutralOutput,
                         ::testing::Values(ChannelMapping::native,
                                           ChannelMapping::false_color,
                                           ChannelMapping::monochrome));

TEST(Processing, FalseColorSwapsTheRenderedHue) {
  const auto dng = patch_scene();
  const auto native = ircore::normalize(dng, options_for(ChannelMapping::native));
  const auto swapped =
      ircore::normalize(dng, options_for(ChannelMapping::false_color));
  const auto mono =
      ircore::normalize(dng, options_for(ChannelMapping::monochrome));
  ASSERT_TRUE(native.has_value());
  ASSERT_TRUE(swapped.has_value());
  ASSERT_TRUE(mono.has_value());

  const Vector3 n = to_srgb(pixel(native->dng.image, 24, 24));
  const Vector3 s = to_srgb(pixel(swapped->dng.image, 24, 24));
  // Warm in native, cool after the swap; red and blue trade places.
  EXPECT_GT(n[0], n[2]);
  EXPECT_GT(s[2], s[0]);
  EXPECT_NEAR(n[0], s[2], 0.02);
  EXPECT_NEAR(n[2], s[0], 0.02);
  EXPECT_NEAR(n[1], s[1], 0.02);

  const Vector3 m = pixel(mono->dng.image, 24, 24);
  EXPECT_NEAR(m[0], m[1], 1e-4);
  EXPECT_NEAR(m[2], m[1], 1e-4);
}

TEST(Processing, ClippedHighlightsStayNeutral) {
  const auto dng =
      make_cfa_dng(32, 32, [](std::uint32_t x, std::uint32_t) -> Vector3 {
        // Right half: red clipped, other channels high but valid.
        return x >= 16 ? Vector3{1.5, 0.9, 0.8}
                       : Vector3{0.3 * ir_response[0], 0.3 * ir_response[1],
                                 0.3 * ir_response[2]};
      });
  auto options = options_for(ChannelMapping::native);
  options.white_balance.mode = ircore::WhiteBalanceMode::explicit_neutral;
  options.white_balance.neutral = ir_response;
  const auto result = ircore::normalize(dng, options);
  ASSERT_TRUE(result.has_value()) << result.error().message;
  for (std::uint32_t y = 4; y < 28; ++y) {
    const Vector3 rgb = pixel(result->dng.image, 26, y);
    EXPECT_EQ(rgb, (Vector3{1.0, 1.0, 1.0}));
  }
  EXPECT_GT(result->report.clipped_fraction, 0.4);
}

TEST(Processing, WhiteBalanceModes) {
  auto dng = patch_scene();
  dng.metadata.as_shot_neutral = Vector3{0.9, 0.6, 0.3};

  auto options = options_for(ChannelMapping::native);
  options.white_balance.mode = ircore::WhiteBalanceMode::as_shot;
  auto report = ircore::analyze(dng, options);
  ASSERT_TRUE(report.has_value()) << report.error().message;
  EXPECT_NEAR(report->camera_neutral[0], 1.0, 1e-12);
  EXPECT_NEAR(report->camera_neutral[1], 0.6 / 0.9, 1e-12);
  EXPECT_NEAR(report->camera_neutral[2], 0.3 / 0.9, 1e-12);

  options.white_balance.mode = ircore::WhiteBalanceMode::reference_area;
  options.white_balance.area = {16, 16, 16, 16}; // The patch.
  report = ircore::analyze(dng, options);
  ASSERT_TRUE(report.has_value()) << report.error().message;
  EXPECT_NEAR(report->camera_neutral[1], 0.6 / 1.25, 5e-3);
  EXPECT_NEAR(report->camera_neutral[2], 0.4 / 1.25, 5e-3);

  options.white_balance.area = {60, 60, 16, 16};
  EXPECT_FALSE(ircore::analyze(dng, options).has_value());

  options.white_balance.mode = ircore::WhiteBalanceMode::explicit_neutral;
  options.white_balance.neutral = {2.0, 1.0, 1.0};
  report = ircore::analyze(dng, options);
  ASSERT_TRUE(report.has_value());
  EXPECT_NEAR(report->camera_neutral[1], 0.5, 1e-12);
  options.white_balance.neutral = {1.0, 0.0, 1.0};
  EXPECT_FALSE(ircore::analyze(dng, options).has_value());
  options.white_balance.neutral = {1.0, 1e-5, 1.0};
  EXPECT_FALSE(ircore::analyze(dng, options).has_value());

  dng.metadata.as_shot_neutral.reset();
  options.white_balance.mode = ircore::WhiteBalanceMode::as_shot;
  EXPECT_FALSE(ircore::analyze(dng, options).has_value());
  dng.metadata.as_shot_white_xy = ircore::white::d65;
  EXPECT_TRUE(ircore::analyze(dng, options).has_value());
}

TEST(Processing, AutoWhiteBalanceFindsTheDominantNeutral) {
  const auto report =
      ircore::analyze(patch_scene(), options_for(ChannelMapping::native));
  ASSERT_TRUE(report.has_value());
  // One sixteenth of the frame is the patch; gray world stays close to the
  // IR neutral of the rest.
  EXPECT_NEAR(report->camera_neutral[1], 0.6, 0.03);
  EXPECT_NEAR(report->camera_neutral[2], 0.5, 0.03);
}

TEST(Processing, AutoMappingFollowsTheFilter) {
  EXPECT_EQ(ircore::resolve_mapping(ChannelMapping::auto_select,
                                    FilterType::nm850),
            ChannelMapping::monochrome);
  EXPECT_EQ(ircore::resolve_mapping(ChannelMapping::auto_select,
                                    FilterType::nm720),
            ChannelMapping::false_color);
  EXPECT_EQ(ircore::resolve_mapping(ChannelMapping::auto_select,
                                    FilterType::nm590),
            ChannelMapping::false_color);
  EXPECT_EQ(ircore::resolve_mapping(ChannelMapping::auto_select,
                                    FilterType::full_spectrum),
            ChannelMapping::native);
  EXPECT_EQ(ircore::resolve_mapping(ChannelMapping::native, FilterType::nm850),
            ChannelMapping::native);

  ircore::ProcessingOptions options;
  const auto report = ircore::analyze(
      make_cfa_dng(64, 64, uniform_scene({0.5, 0.48, 0.49})), options);
  ASSERT_TRUE(report.has_value());
  EXPECT_TRUE(report->filter_detected);
  EXPECT_EQ(report->filter, FilterType::nm850);
  EXPECT_EQ(report->mapping, ChannelMapping::monochrome);
}

TEST(Processing, OutputMetadataDescribesLinearProPhoto) {
  auto dng = patch_scene();
  dng.metadata.orientation = 6;
  dng.metadata.default_crop_origin = std::array<double, 2>{2.0, 2.0};
  dng.metadata.default_crop_size = std::array<double, 2>{60.0, 60.0};
  dng.metadata.opcode_list3 = {1, 2, 3, 4};
  dng.metadata.opcode_list2 = {5, 6, 7, 8};
  dng.metadata.exif.push_back({33437, 5, 1, {28, 0, 0, 0, 10, 0, 0, 0}});

  const auto result = ircore::normalize(dng, options_for(ChannelMapping::native));
  ASSERT_TRUE(result.has_value());
  const auto &meta = result->dng.metadata;
  EXPECT_EQ(meta.photometric, ircore::Photometric::linear_raw);
  EXPECT_EQ(meta.unique_camera_model, ircore::output_camera_model);
  EXPECT_EQ(meta.profile_name, ircore::output_profile_name);
  EXPECT_EQ(meta.make, "Synthetic");
  EXPECT_EQ(meta.orientation, 6);
  EXPECT_EQ(meta.white_level, (std::vector<double>{65535.0, 65535.0, 65535.0}));
  EXPECT_TRUE(meta.black_level.empty());
  ASSERT_EQ(meta.calibrations.size(), 1U);
  EXPECT_EQ(meta.calibrations[0].illuminant, ircore::light_source::d50);
  EXPECT_TRUE(meta.calibrations[0].forward_matrix.has_value());
  EXPECT_EQ(meta.as_shot_neutral, (Vector3{1.0, 1.0, 1.0}));
  EXPECT_DOUBLE_EQ(meta.baseline_exposure, 0.35);
  EXPECT_EQ(meta.default_crop_size, dng.metadata.default_crop_size);
  EXPECT_EQ(meta.opcode_list3, dng.metadata.opcode_list3);
  EXPECT_TRUE(meta.opcode_list2.empty());
  EXPECT_EQ(meta.dng_backward_version[1], 3);
  ASSERT_EQ(meta.exif.size(), 1U);
  EXPECT_FALSE(result->report.warnings.empty());
  ASSERT_TRUE(result->thumbnail.has_value());
  EXPECT_EQ(result->thumbnail->width, 64U);
}

TEST(Processing, LinearRawInput) {
  const auto dng =
      test_support::make_linear_dng(32, 24, uniform_scene({0.6, 0.36, 0.3}));
  const auto result = ircore::normalize(dng, options_for(ChannelMapping::native));
  ASSERT_TRUE(result.has_value()) << result.error().message;
  const Vector3 rgb = pixel(result->dng.image, 10, 10);
  EXPECT_NEAR(rgb[0], 0.6, 2e-3);
  EXPECT_NEAR(rgb[1], 0.6, 2e-3);
  EXPECT_NEAR(rgb[2], 0.6, 2e-3);
  EXPECT_EQ(result->report.input_photometric, ircore::Photometric::linear_raw);
}

TEST(Processing, RejectsTinyImages) {
  const auto dng = make_cfa_dng(4, 4, uniform_scene({0.5, 0.5, 0.5}));
  const auto result = ircore::normalize(dng, {});
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code, ircore::ErrorCode::unsupported_format);
}

TEST(Processing, ResultDoesNotDependOnThreadCount) {
  const auto dng = patch_scene(96);
  auto options = options_for(ChannelMapping::false_color);
  options.threads = 1;
  const auto single = ircore::normalize(dng, options);
  options.threads = 7;
  const auto multi = ircore::normalize(dng, options);
  ASSERT_TRUE(single.has_value());
  ASSERT_TRUE(multi.has_value());
  EXPECT_EQ(single->dng.image.samples, multi->dng.image.samples);
}

TEST(ProcessFile, WritesReadableLinearDng) {
  test_support::TemporaryDirectory directory;
  const auto input = directory.path() / "in.dng";
  const auto output = directory.path() / "out.dng";
  ASSERT_TRUE(ircore::write_dng(input, patch_scene(), {}).has_value());

  const auto report =
      ircore::process_file(input, output, options_for(ChannelMapping::native));
  ASSERT_TRUE(report.has_value()) << report.error().message;
  const auto written = ircore::read_dng(output);
  ASSERT_TRUE(written.has_value()) << written.error().message;
  EXPECT_EQ(written->metadata.photometric, ircore::Photometric::linear_raw);
  EXPECT_EQ(written->image.width, 64U);
  EXPECT_EQ(written->metadata.unique_camera_model, ircore::output_camera_model);

  // The output must be processed identically to the in-memory pipeline.
  const auto in_memory = ircore::normalize(*ircore::read_dng(input),
                                           options_for(ChannelMapping::native));
  ASSERT_TRUE(in_memory.has_value());
  EXPECT_EQ(written->image.samples, in_memory->dng.image.samples);

  const auto same = ircore::process_file(input, input, {});
  ASSERT_FALSE(same.has_value());
  EXPECT_EQ(same.error().code, ircore::ErrorCode::invalid_argument);
  const auto dotted = ircore::process_file(
      input, directory.path() / "." / "in.dng", {});
  EXPECT_FALSE(dotted.has_value());
}

} // namespace
