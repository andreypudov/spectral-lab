#pragma once

#include "ircore/color.h"
#include "ircore/error.h"
#include "ircore/image.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ircore {

// One camera calibration set (ColorMatrixN, CameraCalibrationN,
// ForwardMatrixN, CalibrationIlluminantN).
struct ColorCalibration {
  std::uint16_t illuminant{light_source::unknown};
  std::optional<Matrix3> color_matrix;
  std::optional<Matrix3> camera_calibration;
  std::optional<Matrix3> forward_matrix;
};

// A TIFF entry copied verbatim between files. Value bytes are little-endian.
struct TiffTagValue {
  std::uint16_t tag{};
  std::uint16_t type{};
  std::uint32_t count{};
  std::vector<std::uint8_t> bytes;
};

// Metadata of the main raw image. Geometry-related values (CFA pattern, black
// level pattern, default crop) are relative to the image stored in
// DngImage::image, which the reader crops to the ActiveArea.
struct DngMetadata {
  std::array<std::uint8_t, 4> dng_version{1, 4, 0, 0};
  std::array<std::uint8_t, 4> dng_backward_version{1, 1, 0, 0};

  std::string make;
  std::string model;
  std::string unique_camera_model;
  std::string software;
  std::string date_time;
  std::string artist;
  std::string copyright;
  std::uint16_t orientation{1};

  Photometric photometric{Photometric::cfa};
  std::uint16_t bits_per_sample{16};
  CfaPattern cfa_pattern;

  std::vector<std::uint16_t> linearization_table;
  std::uint32_t black_level_repeat_rows{1};
  std::uint32_t black_level_repeat_cols{1};
  // repeat_rows * repeat_cols * samples_per_pixel values; empty means zero.
  std::vector<double> black_level;
  std::vector<double> black_level_delta_h;
  std::vector<double> black_level_delta_v;
  // One value per sample; empty means 2^bits_per_sample - 1.
  std::vector<double> white_level;

  std::optional<std::array<double, 2>> default_scale;
  std::optional<std::array<double, 2>> default_crop_origin;
  std::optional<std::array<double, 2>> default_crop_size;
  std::optional<double> best_quality_scale;

  std::vector<ColorCalibration> calibrations;
  std::optional<Vector3> analog_balance;
  std::optional<Vector3> as_shot_neutral;
  std::optional<Chromaticity> as_shot_white_xy;

  double baseline_exposure{0.0};
  double baseline_noise{1.0};
  double baseline_sharpness{1.0};
  double linear_response_limit{1.0};

  std::string profile_name;
  std::optional<std::uint32_t> profile_embed_policy;

  // Raw opcode list blobs (big-endian, as defined by the DNG specification).
  std::vector<std::uint8_t> opcode_list1;
  std::vector<std::uint8_t> opcode_list2;
  std::vector<std::uint8_t> opcode_list3;

  // Entries of the EXIF IFD that are safe to copy (no embedded offsets).
  std::vector<TiffTagValue> exif;
};

struct DngImage {
  DngMetadata metadata;
  RawImage image;
};

enum class DngCompression { uncompressed, lossless_jpeg };

struct DngWriteOptions {
  DngCompression compression{DngCompression::lossless_jpeg};
  // Optional 8-bit sRGB preview stored in IFD 0.
  std::optional<Rgb8Image> thumbnail;
};

// Reads the main raw image and its metadata. Supported: CFA (2x2 Bayer) and
// LinearRaw images, uncompressed (8 to 16 bits per sample) or lossless JPEG
// compressed, stored in strips or tiles.
[[nodiscard]] Result<DngImage> read_dng(const std::filesystem::path &path);
[[nodiscard]] Result<DngImage> read_dng(std::span<const std::uint8_t> bytes);

// Serializes a DNG file to memory. Samples are written with 16 bits.
[[nodiscard]] Result<std::vector<std::uint8_t>>
encode_dng(const DngImage &dng, const DngWriteOptions &options);

// Writes a DNG file atomically: the data is written to a temporary file in the
// destination directory which then replaces the destination.
[[nodiscard]] Result<void> write_dng(const std::filesystem::path &path,
                                     const DngImage &dng,
                                     const DngWriteOptions &options);

} // namespace ircore
