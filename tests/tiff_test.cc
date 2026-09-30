#include "ircore/internal/tiff.h"

#include "support/synthetic.h"

#include <gtest/gtest.h>

#include <array>

namespace {

using namespace ircore::detail;

TEST(Tiff, WriterOutputParsesBack) {
  TiffWriter writer;
  const std::size_t blob = writer.add_blob({1, 2, 3, 4, 5});
  IfdBuilder root;
  root.add_short(256, 640);
  root.add_long(257, 480);
  root.add_ascii(271, "Maker");
  const std::array<double, 2> rationals{0.5, 3.0};
  root.add_rational(282, rationals);
  const std::array<double, 3> signed_values{-1.25, 0.0, 2.5};
  root.add_srational(50730, signed_values);
  root.add_blob_offsets(273, {blob});
  root.add_ifd_offsets(330, {1});
  IfdBuilder child;
  child.add_short(254, 0);
  writer.add_ifd(std::move(root));
  writer.add_ifd(std::move(child));

  const auto bytes = writer.serialize();
  ASSERT_TRUE(bytes.has_value());
  const auto file = parse_tiff(*bytes);
  ASSERT_TRUE(file.has_value()) << file.error().message;
  ASSERT_EQ(file->ifds.size(), 2U);
  const TiffIfd &ifd = file->ifds[0];
  EXPECT_EQ(ifd.find(256)->integer(0), 640);
  EXPECT_EQ(ifd.find(257)->integer(0), 480);
  EXPECT_EQ(ifd.find(271)->text(), "Maker");
  EXPECT_DOUBLE_EQ(*ifd.find(282)->number(0), 0.5);
  EXPECT_DOUBLE_EQ(*ifd.find(282)->number(1), 3.0);
  EXPECT_DOUBLE_EQ(*ifd.find(50730)->number(0), -1.25);
  EXPECT_DOUBLE_EQ(*ifd.find(50730)->number(2), 2.5);
  const auto offset = ifd.find(273)->integer(0);
  ASSERT_TRUE(offset.has_value());
  EXPECT_EQ(*offset % 2, 0);
  EXPECT_EQ((*bytes)[static_cast<std::size_t>(*offset) + 4], 5);
  EXPECT_EQ(file->ifds[1].find(254)->integer(0), 0);
}

TEST(Tiff, ParsesBigEndianFiles) {
  test_support::TiffBuilder builder(true);
  builder.add_short(256, {1234});
  builder.add_long(257, {99999});
  builder.add_srational(50721, {-0.5, 1.0, 0.25});
  builder.add_ascii(272, "Big Endian Model");
  const auto bytes = builder.build();

  const auto file = parse_tiff(bytes);
  ASSERT_TRUE(file.has_value()) << file.error().message;
  EXPECT_TRUE(file->big_endian);
  const TiffIfd &ifd = file->ifds[0];
  EXPECT_EQ(ifd.find(256)->integer(0), 1234);
  EXPECT_EQ(ifd.find(257)->integer(0), 99999);
  EXPECT_DOUBLE_EQ(*ifd.find(50721)->number(0), -0.5);
  EXPECT_EQ(ifd.find(272)->text(), "Big Endian Model");

  // Little-endian conversion swaps each element (and rational half).
  const auto le = ifd.find(256)->little_endian_bytes();
  EXPECT_EQ(le, (std::vector<std::uint8_t>{0xD2, 0x04}));
  const auto le_rational = ifd.find(50721)->little_endian_bytes();
  ASSERT_EQ(le_rational.size(), 24U);
  EXPECT_EQ(le_rational[4], 0x10); // Denominator 10000 = 0x2710.
  EXPECT_EQ(le_rational[5], 0x27);
}

TEST(Tiff, RejectsInvalidHeaders) {
  EXPECT_FALSE(parse_tiff(std::vector<std::uint8_t>{}).has_value());
  EXPECT_FALSE(
      parse_tiff(std::vector<std::uint8_t>{'X', 'X', 42, 0, 8, 0, 0, 0})
          .has_value());
  EXPECT_FALSE(
      parse_tiff(std::vector<std::uint8_t>{'I', 'I', 42, 0, 200, 0, 0, 0})
          .has_value());
  const auto bigtiff =
      parse_tiff(std::vector<std::uint8_t>{'I', 'I', 43, 0, 8, 0, 0, 0});
  ASSERT_FALSE(bigtiff.has_value());
  EXPECT_EQ(bigtiff.error().code, ircore::ErrorCode::unsupported_format);
}

TEST(Tiff, DetectsIfdLoops) {
  // IFD at offset 8 with zero entries whose next IFD is itself.
  const std::vector<std::uint8_t> bytes{'I', 'I', 42, 0, 8, 0, 0, 0,
                                        0,   0,   8,  0, 0, 0};
  const auto file = parse_tiff(bytes);
  ASSERT_FALSE(file.has_value());
  EXPECT_NE(file.error().message.find("loop"), std::string::npos);
}

TEST(Tiff, SkipsEntriesPointingOutsideTheFile) {
  test_support::TiffBuilder builder(false);
  builder.add_short(256, {10});
  // A RATIONAL (8 bytes) whose value offset points far beyond the file end.
  builder.add(282, 5, 1, builder.u32(0x7FFFFFF0));
  const auto file = parse_tiff(builder.build());
  ASSERT_TRUE(file.has_value());
  EXPECT_NE(file->ifds[0].find(256), nullptr);
  EXPECT_EQ(file->ifds[0].find(282), nullptr);
}

TEST(Tiff, RationalConversion) {
  EXPECT_EQ(to_rational(3.0, 10000), (std::pair<std::int64_t, std::int64_t>{3, 1}));
  EXPECT_EQ(to_rational(0.5, 10000),
            (std::pair<std::int64_t, std::int64_t>{5000, 10000}));
  EXPECT_EQ(to_rational(-1.23456, 10000),
            (std::pair<std::int64_t, std::int64_t>{-12346, 10000}));
  const auto big = to_rational(123456.7, 10000);
  EXPECT_NEAR(static_cast<double>(big.first) / static_cast<double>(big.second),
              123456.7, 0.1);
}

} // namespace
