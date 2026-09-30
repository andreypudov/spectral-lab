#include "ircore/dng.h"
#include "ircore/internal/dng_tags.h"
#include "ircore/internal/tiff.h"

#include "support/synthetic.h"

#include <gtest/gtest.h>

#include <random>

namespace {

using test_support::make_cfa_dng;
using test_support::make_linear_dng;
using test_support::TiffBuilder;
namespace tag = ircore::detail::tag;

ircore::Vector3 gradient(std::uint32_t x, std::uint32_t y) {
  return {0.001 * x, 0.002 * y, 0.5 + 0.001 * (x + y)};
}

void expect_same_metadata(const ircore::DngMetadata &a,
                          const ircore::DngMetadata &b) {
  EXPECT_EQ(a.make, b.make);
  EXPECT_EQ(a.model, b.model);
  EXPECT_EQ(a.unique_camera_model, b.unique_camera_model);
  EXPECT_EQ(a.photometric, b.photometric);
  EXPECT_EQ(a.cfa_pattern, b.cfa_pattern);
  EXPECT_EQ(a.black_level, b.black_level);
  ASSERT_EQ(a.calibrations.size(), b.calibrations.size());
  for (std::size_t i = 0; i < a.calibrations.size(); ++i) {
    EXPECT_EQ(a.calibrations[i].illuminant, b.calibrations[i].illuminant);
    for (std::size_t r = 0; r < 3; ++r) {
      for (std::size_t c = 0; c < 3; ++c) {
        EXPECT_NEAR((*a.calibrations[i].color_matrix)(r, c),
                    (*b.calibrations[i].color_matrix)(r, c), 1e-4);
      }
    }
  }
  ASSERT_TRUE(b.as_shot_neutral.has_value());
  for (std::size_t i = 0; i < 3; ++i) {
    EXPECT_NEAR((*a.as_shot_neutral)[i], (*b.as_shot_neutral)[i], 1e-4);
  }
  EXPECT_NEAR(a.baseline_exposure, b.baseline_exposure, 1e-4);
}

class DngRoundtrip : public ::testing::TestWithParam<ircore::DngCompression> {};

TEST_P(DngRoundtrip, CfaImageAndMetadataSurvive) {
  test_support::SyntheticCamera camera;
  camera.pattern.colors = {{{ircore::CfaColor::green, ircore::CfaColor::blue},
                            {ircore::CfaColor::red, ircore::CfaColor::green}}};
  // Not a multiple of the tile size, to exercise edge tiles.
  auto dng = make_cfa_dng(300, 270, gradient, camera);
  dng.metadata.default_crop_origin = std::array<double, 2>{8.0, 8.0};
  dng.metadata.default_crop_size = std::array<double, 2>{284.0, 254.0};
  dng.metadata.opcode_list3 = {0, 0, 0, 0};
  dng.metadata.exif.push_back(
      {33434, ircore::detail::tiff_type::rational, 1, {1, 0, 0, 0, 125, 0, 0, 0}});

  ircore::DngWriteOptions options;
  options.compression = GetParam();
  const auto bytes = ircore::encode_dng(dng, options);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().message;
  const auto read = ircore::read_dng(*bytes);
  ASSERT_TRUE(read.has_value()) << read.error().message;

  EXPECT_EQ(read->image.width, 300U);
  EXPECT_EQ(read->image.height, 270U);
  EXPECT_EQ(read->image.samples, dng.image.samples);
  expect_same_metadata(dng.metadata, read->metadata);
  ASSERT_EQ(read->metadata.white_level.size(), 1U);
  EXPECT_EQ(read->metadata.white_level[0], 16383.0);
  EXPECT_EQ(read->metadata.default_crop_size, dng.metadata.default_crop_size);
  EXPECT_EQ(read->metadata.opcode_list3, dng.metadata.opcode_list3);
  ASSERT_EQ(read->metadata.exif.size(), 1U);
  EXPECT_EQ(read->metadata.exif[0].tag, 33434);
  EXPECT_EQ(read->metadata.exif[0].bytes, dng.metadata.exif[0].bytes);
}

TEST_P(DngRoundtrip, LinearRawWithThumbnail) {
  auto dng = make_linear_dng(257, 40, gradient);
  ircore::DngWriteOptions options;
  options.compression = GetParam();
  ircore::Rgb8Image thumbnail;
  thumbnail.width = 4;
  thumbnail.height = 2;
  thumbnail.pixels.assign(4 * 2 * 3, 200);
  options.thumbnail = thumbnail;

  const auto bytes = ircore::encode_dng(dng, options);
  ASSERT_TRUE(bytes.has_value()) << bytes.error().message;
  const auto read = ircore::read_dng(*bytes);
  ASSERT_TRUE(read.has_value()) << read.error().message;
  EXPECT_EQ(read->metadata.photometric, ircore::Photometric::linear_raw);
  EXPECT_EQ(read->image.samples_per_pixel, 3U);
  EXPECT_EQ(read->image.samples, dng.image.samples);
  expect_same_metadata(dng.metadata, read->metadata);
}

INSTANTIATE_TEST_SUITE_P(Compression, DngRoundtrip,
                         ::testing::Values(ircore::DngCompression::uncompressed,
                                           ircore::DngCompression::lossless_jpeg));

TEST(Dng, LosslessJpegIsSmallerThanUncompressed) {
  const auto dng = make_cfa_dng(512, 512, gradient);
  ircore::DngWriteOptions options;
  options.compression = ircore::DngCompression::uncompressed;
  const auto plain = ircore::encode_dng(dng, options);
  options.compression = ircore::DngCompression::lossless_jpeg;
  const auto compressed = ircore::encode_dng(dng, options);
  ASSERT_TRUE(plain.has_value());
  ASSERT_TRUE(compressed.has_value());
  EXPECT_LT(compressed->size(), plain->size() / 2);
}

// Big-endian DNG with 12-bit packed strips, an odd ActiveArea offset, a
// black level pattern and a linearization table.
std::vector<std::uint8_t> crafted_dng(bool big_endian) {
  constexpr std::uint32_t width = 6;
  constexpr std::uint32_t height = 5;
  TiffBuilder builder(big_endian);
  builder.add_bytes(tag::dng_version, {1, 4, 0, 0});
  builder.add_ascii(tag::make, "Crafted");
  builder.add_ascii(tag::unique_camera_model, "Crafted Camera");
  builder.add_long(tag::new_subfile_type, {0});
  builder.add_long(tag::image_width, {width});
  builder.add_long(tag::image_length, {height});
  builder.add_short(tag::bits_per_sample, {12});
  builder.add_short(tag::compression, {1});
  builder.add_short(tag::photometric_interpretation, {32803});
  builder.add_short(tag::samples_per_pixel, {1});
  builder.add_long(tag::rows_per_strip, {height});
  builder.add_short(tag::cfa_repeat_pattern_dim, {2, 2});
  builder.add_bytes(tag::cfa_pattern, {0, 1, 1, 2}); // RGGB
  builder.add_long(tag::active_area, {1, 1, 5, 6});   // top, left, bottom, right
  builder.add_short(tag::black_level_repeat_dim, {1, 2});
  builder.add_short(tag::black_level, {10, 20});
  builder.add_short(tag::white_level, {4000});
  builder.add_short(tag::linearization_table, {0, 100, 200, 300});
  builder.add_srational(tag::color_matrix1,
                        {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0});
  builder.add_short(tag::calibration_illuminant1, {21});

  // 12-bit samples, value = row * 16 + col (mapped through the table only
  // for values below 4), packed MSB first; rows padded to whole bytes.
  std::vector<std::uint8_t> data;
  for (std::uint32_t row = 0; row < height; ++row) {
    std::uint32_t buffer = 0;
    int bits = 0;
    for (std::uint32_t col = 0; col < width; ++col) {
      buffer = (buffer << 12U) | (row * 16 + col);
      bits += 12;
      while (bits >= 8) {
        data.push_back(static_cast<std::uint8_t>(buffer >> (bits - 8)));
        bits -= 8;
      }
    }
    if (bits > 0) {
      data.push_back(static_cast<std::uint8_t>(buffer << (8 - bits)));
    }
  }
  builder.set_image_data(tag::strip_offsets, tag::strip_byte_counts, data);
  return builder.build();
}

class CraftedDng : public ::testing::TestWithParam<bool> {};

TEST_P(CraftedDng, DecodesPackedBitsActiveAreaAndLevels) {
  const auto read = ircore::read_dng(crafted_dng(GetParam()));
  ASSERT_TRUE(read.has_value()) << read.error().message;
  const ircore::DngImage &dng = *read;
  // Active area 1..5 x 1..6 of the 6x5 stored image.
  EXPECT_EQ(dng.image.width, 5U);
  EXPECT_EQ(dng.image.height, 4U);
  for (std::uint32_t row = 0; row < 4; ++row) {
    for (std::uint32_t col = 0; col < 5; ++col) {
      EXPECT_EQ(dng.image.at(row, col), (row + 1) * 16 + col + 1);
    }
  }
  // RGGB shifted by one row and column becomes BGGR.
  EXPECT_EQ(dng.metadata.cfa_pattern.at(0, 0), ircore::CfaColor::blue);
  EXPECT_EQ(dng.metadata.cfa_pattern.at(1, 1), ircore::CfaColor::red);
  EXPECT_EQ(dng.metadata.black_level, (std::vector<double>{10.0, 20.0}));
  EXPECT_EQ(dng.metadata.white_level, (std::vector<double>{4000.0}));
  EXPECT_EQ(dng.metadata.linearization_table,
            (std::vector<std::uint16_t>{0, 100, 200, 300}));
  EXPECT_EQ(dng.metadata.bits_per_sample, 12);
  EXPECT_EQ(dng.metadata.make, "Crafted");
}

INSTANTIATE_TEST_SUITE_P(ByteOrder, CraftedDng, ::testing::Values(false, true));

TEST(Dng, RejectsNonDngTiff) {
  TiffBuilder builder(false);
  builder.add_long(tag::image_width, {4});
  const auto read = ircore::read_dng(builder.build());
  ASSERT_FALSE(read.has_value());
  EXPECT_NE(read.error().message.find("DNGVersion"), std::string::npos);
}

TEST(Dng, RejectsUnsupportedLayouts) {
  TiffBuilder builder(false);
  builder.add_bytes(tag::dng_version, {1, 4, 0, 0});
  builder.add_long(tag::image_width, {4});
  builder.add_long(tag::image_length, {4});
  builder.add_short(tag::bits_per_sample, {16});
  builder.add_short(tag::compression, {34892}); // Lossy DNG.
  builder.add_short(tag::photometric_interpretation, {32803});
  builder.set_image_data(tag::strip_offsets, tag::strip_byte_counts,
                         std::vector<std::uint8_t>(32, 0));
  const auto lossy = ircore::read_dng(builder.build());
  ASSERT_FALSE(lossy.has_value());
  EXPECT_EQ(lossy.error().code, ircore::ErrorCode::unsupported_format);

  // Truncated strip data.
  auto bytes = crafted_dng(false);
  bytes.resize(bytes.size() - 4);
  const auto truncated = ircore::read_dng(bytes);
  ASSERT_FALSE(truncated.has_value());
}

TEST(Dng, CorruptFilesNeverCrash) {
  const auto dng = make_cfa_dng(64, 48, gradient);
  ircore::DngWriteOptions options;
  ircore::Rgb8Image thumbnail{2, 2, std::vector<std::uint8_t>(12, 1)};
  options.thumbnail = thumbnail;
  const auto valid = ircore::encode_dng(dng, options);
  ASSERT_TRUE(valid.has_value());

  std::mt19937 random(2024);
  int readable = 0;
  for (int iteration = 0; iteration < 1500; ++iteration) {
    auto corrupt = *valid;
    if (iteration % 5 == 0) {
      corrupt.resize(random() % corrupt.size());
    } else {
      // Corrupt the metadata area more often than the pixel data.
      const std::size_t range =
          iteration % 2 == 0 ? std::min<std::size_t>(corrupt.size(), 1500)
                             : corrupt.size();
      const int flips = 1 + static_cast<int>(random() % 6);
      for (int i = 0; i < flips; ++i) {
        corrupt[random() % range] = static_cast<std::uint8_t>(random());
      }
    }
    const auto read = ircore::read_dng(corrupt);
    if (read) {
      ++readable;
      EXPECT_EQ(read->image.samples.size(), read->image.sample_count());
    }
  }
  // Some corruptions hit unused bytes, so a part of the files stays readable.
  EXPECT_GT(readable, 0);
}

TEST(Dng, WriteIsAtomicAndReadable) {
  test_support::TemporaryDirectory directory;
  const auto path = directory.path() / "out.dng";
  const auto dng = make_cfa_dng(32, 32, gradient);
  ASSERT_TRUE(ircore::write_dng(path, dng, {}).has_value());
  const auto read = ircore::read_dng(path);
  ASSERT_TRUE(read.has_value()) << read.error().message;
  EXPECT_EQ(read->image.samples, dng.image.samples);
  // Only the destination remains; the temporary file was renamed.
  std::size_t files = 0;
  for (const auto &entry : std::filesystem::directory_iterator(directory.path())) {
    (void)entry;
    ++files;
  }
  EXPECT_EQ(files, 1U);

  const auto missing = ircore::read_dng(directory.path() / "missing.dng");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code, ircore::ErrorCode::io_error);
}

} // namespace
