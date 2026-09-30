#include "ircore/analysis.h"

#include "support/synthetic.h"

#include <gtest/gtest.h>

#include <cmath>
#include <random>

namespace {

using ircore::Vector3;
using test_support::make_cfa_dng;
using test_support::uniform_scene;

TEST(Linearize, AppliesBlackPatternDeltasAndTable) {
  auto dng = make_cfa_dng(4, 2, uniform_scene({0.5, 0.5, 0.5}));
  dng.image.samples = {100, 200, 300, 400, 100, 200, 300, 400};
  dng.metadata.black_level_repeat_rows = 1;
  dng.metadata.black_level_repeat_cols = 2;
  dng.metadata.black_level = {10.0, 20.0};
  dng.metadata.black_level_delta_h = {0.0, 0.0, 5.0, 5.0};
  dng.metadata.black_level_delta_v = {0.0, 10.0};
  dng.metadata.white_level = {1010.0};

  const auto linear = ircore::linearize(dng);
  ASSERT_TRUE(linear.has_value()) << linear.error().message;
  const auto expect = [&](std::size_t index, double raw, double black) {
    EXPECT_NEAR(linear->data[index], (raw - black) / (1010.0 - black), 1e-6)
        << index;
  };
  expect(0, 100, 10);
  expect(1, 200, 20);
  expect(2, 300, 15);
  expect(3, 400, 25);
  expect(4, 100, 20);
  expect(7, 400, 35);

  // A linearization table maps stored values before the black level.
  dng.metadata.linearization_table = {0, 1000};
  dng.image.samples = {0, 1, 5, 1, 0, 1, 0, 1};
  const auto mapped = ircore::linearize(dng);
  ASSERT_TRUE(mapped.has_value());
  EXPECT_NEAR(mapped->data[1], (1000.0 - 20.0) / (1010.0 - 20.0), 1e-6);
  // Values beyond the table use its last entry.
  EXPECT_NEAR(mapped->data[2], (1000.0 - 15.0) / (1010.0 - 15.0), 1e-6);
}

TEST(Linearize, RejectsInconsistentLevels) {
  auto dng = make_cfa_dng(8, 8, uniform_scene({0.5, 0.5, 0.5}));
  dng.metadata.white_level = {400.0};
  dng.metadata.black_level = {512.0};
  EXPECT_FALSE(ircore::linearize(dng).has_value());

  dng = make_cfa_dng(8, 8, uniform_scene({0.5, 0.5, 0.5}));
  dng.metadata.black_level = {1.0, 2.0, 3.0};
  EXPECT_FALSE(ircore::linearize(dng).has_value());
}

TEST(Statistics, SeparatesClippedBlocks) {
  // Left half of the frame is clipped in the red channel.
  const auto dng = make_cfa_dng(64, 32, [](std::uint32_t x, std::uint32_t) {
    return x < 32 ? Vector3{1.2, 0.5, 0.4} : Vector3{0.6, 0.3, 0.2};
  });
  const auto linear = ircore::linearize(dng);
  ASSERT_TRUE(linear.has_value());
  const auto stats = ircore::collect_statistics(*linear);
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->inspected, 32U * 16U);
  EXPECT_EQ(stats->clipped, 16U * 16U);
  ASSERT_EQ(stats->samples.size(), 16U * 16U);
  EXPECT_NEAR(stats->samples[0].red, 0.6, 1e-3);
  EXPECT_NEAR(stats->samples[0].green, 0.3, 1e-3);
  EXPECT_NEAR(stats->samples[0].blue, 0.2, 1e-3);
}

TEST(Statistics, RegionAndSubsampling) {
  const auto dng = make_cfa_dng(128, 128, [](std::uint32_t x, std::uint32_t y) {
    return x >= 64 && y >= 64 ? Vector3{0.2, 0.4, 0.8} : Vector3{0.5, 0.5, 0.5};
  });
  const auto linear = ircore::linearize(dng);
  ASSERT_TRUE(linear.has_value());
  const auto region =
      ircore::collect_statistics(*linear, ircore::PixelRect{64, 64, 64, 64});
  ASSERT_TRUE(region.has_value());
  EXPECT_EQ(region->inspected, 32U * 32U);
  const auto neutral = ircore::estimate_neutral(*region);
  ASSERT_TRUE(neutral.has_value());
  EXPECT_NEAR((*neutral)[0], 0.25, 1e-3);
  EXPECT_NEAR((*neutral)[1], 0.5, 1e-3);
  EXPECT_NEAR((*neutral)[2], 1.0, 1e-9);

  const auto sparse = ircore::collect_statistics(*linear, std::nullopt, 100);
  ASSERT_TRUE(sparse.has_value());
  EXPECT_LE(sparse->inspected, 100U);
  EXPECT_GT(sparse->inspected, 25U);

  EXPECT_FALSE(
      ircore::collect_statistics(*linear, ircore::PixelRect{100, 100, 64, 64})
          .has_value());
  EXPECT_FALSE(
      ircore::collect_statistics(*linear, ircore::PixelRect{0, 0, 1, 1})
          .has_value());
}

TEST(WhiteBalance, GrayWorldRecoversChannelRatios) {
  std::mt19937 random(5);
  std::uniform_real_distribution<double> brightness(0.05, 0.9);
  std::normal_distribution<double> noise(0.0, 0.003);
  const Vector3 response{1.0, 0.61, 0.52};
  const auto dng =
      make_cfa_dng(256, 256, [&](std::uint32_t, std::uint32_t) -> Vector3 {
        const double level = brightness(random);
        return {level * response[0] + noise(random),
                level * response[1] + noise(random),
                level * response[2] + noise(random)};
      });
  const auto linear = ircore::linearize(dng);
  ASSERT_TRUE(linear.has_value());
  const auto stats = ircore::collect_statistics(*linear);
  ASSERT_TRUE(stats.has_value());
  const auto neutral = ircore::estimate_neutral(*stats);
  ASSERT_TRUE(neutral.has_value());
  EXPECT_NEAR((*neutral)[0], 1.0, 1e-9);
  EXPECT_NEAR((*neutral)[1], 0.61, 0.01);
  EXPECT_NEAR((*neutral)[2], 0.52, 0.01);
}

TEST(WhiteBalance, FailsWithoutUsablePixels) {
  const auto dng = make_cfa_dng(16, 16, uniform_scene({1.5, 1.5, 1.5}));
  const auto linear = ircore::linearize(dng);
  ASSERT_TRUE(linear.has_value());
  const auto stats = ircore::collect_statistics(*linear);
  ASSERT_TRUE(stats.has_value());
  EXPECT_EQ(stats->clipped, stats->inspected);
  EXPECT_FALSE(ircore::estimate_neutral(*stats).has_value());
}

// Scene with a given mean channel response and a log-chromaticity spread.
ircore::SceneStatistics synthetic_scene(const Vector3 &response,
                                        double chroma_sigma) {
  std::mt19937 random(9);
  std::uniform_real_distribution<double> brightness(0.1, 0.6);
  std::normal_distribution<double> chroma(0.0, chroma_sigma);
  ircore::SceneStatistics stats;
  for (int i = 0; i < 20000; ++i) {
    const double level = brightness(random);
    const double r = std::exp(chroma(random));
    const double b = std::exp(chroma(random));
    stats.samples.push_back({static_cast<float>(level * response[0] * r),
                             static_cast<float>(level * response[1]),
                             static_cast<float>(level * response[2] * b)});
  }
  stats.inspected = stats.samples.size();
  return stats;
}

struct DetectionCase {
  Vector3 response;
  double chroma_sigma;
  ircore::FilterType expected;
};

class FilterDetectionTest : public ::testing::TestWithParam<DetectionCase> {};

TEST_P(FilterDetectionTest, ClassifiesBySpreadAndBalance) {
  const auto &param = GetParam();
  const auto stats = synthetic_scene(param.response, param.chroma_sigma);
  const auto neutral = ircore::estimate_neutral(stats);
  ASSERT_TRUE(neutral.has_value());
  const auto detection = ircore::detect_filter(stats, *neutral);
  EXPECT_EQ(detection.filter, param.expected)
      << "balance " << detection.channel_balance << " spread "
      << detection.chroma_spread;
  EXPECT_EQ(detection.confidence, ircore::DetectionConfidence::high);
  EXPECT_NEAR(detection.chroma_spread, param.chroma_sigma * std::sqrt(2.0),
              0.2 * param.chroma_sigma + 0.005);
}

INSTANTIATE_TEST_SUITE_P(
    Filters, FilterDetectionTest,
    ::testing::Values(
        DetectionCase{{1.0, 0.95, 0.97}, 0.01, ircore::FilterType::nm850},
        DetectionCase{{1.0, 0.95, 0.97}, 0.08, ircore::FilterType::nm720},
        DetectionCase{{1.0, 0.62, 0.55}, 0.08, ircore::FilterType::nm720},
        DetectionCase{{1.0, 0.36, 0.30}, 0.1675, ircore::FilterType::nm665},
        DetectionCase{{1.0, 0.20, 0.12}, 0.30, ircore::FilterType::nm590}));

TEST(FilterDetection, LowConfidenceNearThresholds) {
  const auto stats = synthetic_scene({1.0, 0.46, 0.45}, 0.143);
  const auto neutral = ircore::estimate_neutral(stats);
  ASSERT_TRUE(neutral.has_value());
  const auto detection = ircore::detect_filter(stats, *neutral);
  EXPECT_EQ(detection.confidence, ircore::DetectionConfidence::low);
}

} // namespace
