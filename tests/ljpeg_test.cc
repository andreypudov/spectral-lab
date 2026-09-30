#include "ircore/internal/ljpeg.h"

#include <gtest/gtest.h>

#include <random>
#include <vector>

namespace {

using namespace ircore::detail;

std::vector<std::uint16_t> random_samples(std::size_t count,
                                          std::uint32_t precision,
                                          std::uint32_t seed) {
  std::mt19937 random(seed);
  std::uniform_int_distribution<std::uint32_t> noise(0, 200);
  std::vector<std::uint16_t> samples(count);
  const std::uint32_t max_value = (1U << precision) - 1U;
  std::uint32_t level = max_value / 2;
  for (auto &sample : samples) {
    // Smooth random walk with noise, like image data.
    level = (level + noise(random) * 7) % (max_value + 1);
    sample = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(max_value, level + noise(random)));
  }
  return samples;
}

void expect_roundtrip(const std::vector<std::uint16_t> &samples,
                      const LosslessJpegEncodeParams &params) {
  const auto encoded = encode_lossless_jpeg(samples, params);
  ASSERT_TRUE(encoded.has_value()) << encoded.error().message;
  const auto decoded = decode_lossless_jpeg(*encoded);
  ASSERT_TRUE(decoded.has_value()) << decoded.error().message;
  EXPECT_EQ(decoded->width, params.width);
  EXPECT_EQ(decoded->height, params.height);
  EXPECT_EQ(decoded->components, params.components);
  EXPECT_EQ(decoded->precision, params.precision);
  EXPECT_EQ(decoded->samples, samples);
}

class LosslessJpegRoundtrip
    : public ::testing::TestWithParam<std::tuple<std::uint32_t, std::uint32_t>> {
};

TEST_P(LosslessJpegRoundtrip, EveryPredictorAndComponentCount) {
  const auto [components, predictor] = GetParam();
  LosslessJpegEncodeParams params;
  params.width = 37;
  params.height = 23;
  params.components = components;
  params.predictor = predictor;
  for (const std::uint32_t precision : {8U, 12U, 14U, 16U}) {
    params.precision = precision;
    expect_roundtrip(random_samples(std::size_t{37} * 23 * components,
                                    precision, components * 31 + predictor),
                     params);
  }
}

INSTANTIATE_TEST_SUITE_P(Codec, LosslessJpegRoundtrip,
                         ::testing::Combine(::testing::Values(1U, 2U, 3U, 4U),
                                            ::testing::Values(1U, 2U, 3U, 4U,
                                                              5U, 6U, 7U)));

TEST(LosslessJpeg, ExtremeDifferencesUseCategory16) {
  // Alternating 0 / 65535 / 32768 produces differences of +-32768.
  std::vector<std::uint16_t> samples;
  for (int i = 0; i < 64 * 4; ++i) {
    samples.push_back(i % 3 == 0 ? 0 : (i % 3 == 1 ? 65535 : 32768));
  }
  LosslessJpegEncodeParams params;
  params.width = 64;
  params.height = 4;
  expect_roundtrip(samples, params);
}

TEST(LosslessJpeg, ConstantImage) {
  LosslessJpegEncodeParams params;
  params.width = 256;
  params.height = 256;
  params.components = 3;
  const std::vector<std::uint16_t> samples(std::size_t{256} * 256 * 3, 4242);
  expect_roundtrip(samples, params);
  // A constant image compresses to almost nothing.
  const auto encoded = encode_lossless_jpeg(samples, params);
  ASSERT_TRUE(encoded.has_value());
  EXPECT_LT(encoded->size(), samples.size() / 4);
}

TEST(LosslessJpeg, SinglePixel) {
  LosslessJpegEncodeParams params;
  params.width = 1;
  params.height = 1;
  expect_roundtrip({12345}, params);
}

TEST(LosslessJpeg, RejectsInvalidEncoderInput) {
  LosslessJpegEncodeParams params;
  params.width = 2;
  params.height = 2;
  params.precision = 12;
  EXPECT_FALSE(encode_lossless_jpeg(std::vector<std::uint16_t>{1, 2, 3}, params)
                   .has_value());
  EXPECT_FALSE(
      encode_lossless_jpeg(std::vector<std::uint16_t>{1, 2, 3, 4096}, params)
          .has_value());
  params.predictor = 8;
  EXPECT_FALSE(encode_lossless_jpeg(std::vector<std::uint16_t>{1, 2, 3, 4}, params)
                   .has_value());
}

TEST(LosslessJpeg, RejectsTruncatedStreams) {
  LosslessJpegEncodeParams params;
  params.width = 64;
  params.height = 64;
  const auto samples = random_samples(64 * 64, 16, 7);
  const auto encoded = encode_lossless_jpeg(samples, params);
  ASSERT_TRUE(encoded.has_value());
  for (const std::size_t keep : {std::size_t{0}, std::size_t{3}, std::size_t{40},
                                 encoded->size() / 2}) {
    const std::vector<std::uint8_t> truncated(encoded->begin(),
                                              encoded->begin() +
                                                  static_cast<std::ptrdiff_t>(keep));
    EXPECT_FALSE(decode_lossless_jpeg(truncated).has_value()) << keep;
  }
}

TEST(LosslessJpeg, RejectsLossyJpeg) {
  // SOI followed by a baseline (SOF0) frame header.
  const std::vector<std::uint8_t> stream{0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B,
                                         0x08, 0x00, 0x10, 0x00, 0x10, 0x01,
                                         0x01, 0x11, 0x00};
  const auto decoded = decode_lossless_jpeg(stream);
  ASSERT_FALSE(decoded.has_value());
  EXPECT_EQ(decoded.error().code, ircore::ErrorCode::unsupported_format);
}

TEST(LosslessJpeg, CorruptDataNeverCrashes) {
  LosslessJpegEncodeParams params;
  params.width = 32;
  params.height = 32;
  params.components = 2;
  const auto samples = random_samples(32 * 32 * 2, 14, 3);
  const auto encoded = encode_lossless_jpeg(samples, params);
  ASSERT_TRUE(encoded.has_value());
  std::mt19937 random(11);
  for (int iteration = 0; iteration < 500; ++iteration) {
    auto corrupt = *encoded;
    const int flips = 1 + static_cast<int>(random() % 4);
    for (int i = 0; i < flips; ++i) {
      corrupt[random() % corrupt.size()] ^=
          static_cast<std::uint8_t>(1U << (random() % 8));
    }
    const auto decoded = decode_lossless_jpeg(corrupt);
    if (decoded) {
      EXPECT_EQ(decoded->samples.size(), std::size_t{decoded->width} *
                                             decoded->height *
                                             decoded->components);
    }
  }
}

} // namespace
