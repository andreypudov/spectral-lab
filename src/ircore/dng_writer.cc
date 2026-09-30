#include "ircore/dng.h"
#include "ircore/internal/dng_tags.h"
#include "ircore/internal/ljpeg.h"
#include "ircore/internal/parallel.h"
#include "ircore/internal/tiff.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <mutex>
#include <new>
#include <random>

namespace ircore {

namespace {

using detail::IfdBuilder;
using detail::TiffWriter;
namespace tag = detail::tag;
namespace tiff_type = detail::tiff_type;

constexpr std::uint32_t tile_size = 256;

std::vector<double> matrix_values(const Matrix3 &matrix) {
  std::vector<double> values;
  for (const auto &row : matrix.m) {
    values.insert(values.end(), row.begin(), row.end());
  }
  return values;
}

Result<void> validate(const DngImage &dng) {
  const RawImage &image = dng.image;
  const std::uint32_t expected_spp =
      dng.metadata.photometric == Photometric::cfa ? 1 : 3;
  if (image.width == 0 || image.height == 0 ||
      image.samples_per_pixel != expected_spp ||
      image.samples.size() != image.sample_count()) {
    return make_error(ErrorCode::invalid_argument,
                      "raw image geometry is inconsistent");
  }
  if (dng.metadata.photometric == Photometric::cfa &&
      !dng.metadata.cfa_pattern.is_bayer()) {
    return make_error(ErrorCode::invalid_argument,
                      "CFA images need a Bayer pattern");
  }
  if (dng.metadata.calibrations.empty()) {
    return make_error(ErrorCode::invalid_argument,
                      "DNG color images need ColorMatrix1");
  }
  return {};
}

// Samples of one tile; pixels beyond the image repeat the edge pixels.
std::vector<std::uint16_t> extract_tile(const RawImage &image, std::uint32_t x0,
                                        std::uint32_t y0) {
  const std::size_t spp = image.samples_per_pixel;
  std::vector<std::uint16_t> tile(std::size_t{tile_size} * tile_size * spp);
  for (std::uint32_t row = 0; row < tile_size; ++row) {
    const std::uint32_t y = std::min(y0 + row, image.height - 1);
    for (std::uint32_t col = 0; col < tile_size; ++col) {
      const std::uint32_t x = std::min(x0 + col, image.width - 1);
      std::copy_n(image.samples.begin() +
                      static_cast<std::ptrdiff_t>((std::size_t{y} * image.width +
                                                   x) *
                                                  spp),
                  spp,
                  tile.begin() + static_cast<std::ptrdiff_t>(
                                     (std::size_t{row} * tile_size + col) * spp));
    }
  }
  return tile;
}

Result<std::vector<std::vector<std::uint8_t>>>
encode_tiles(const RawImage &image, DngCompression compression,
             Photometric photometric) {
  const std::uint32_t across = (image.width + tile_size - 1) / tile_size;
  const std::uint32_t down = (image.height + tile_size - 1) / tile_size;
  std::vector<std::vector<std::uint8_t>> tiles(std::size_t{across} * down);

  std::optional<Error> failure;
  std::mutex failure_mutex;
  detail::parallel_for(tiles.size(), 0, [&](std::size_t begin,
                                            std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
      const auto x0 = static_cast<std::uint32_t>(i % across) * tile_size;
      const auto y0 = static_cast<std::uint32_t>(i / across) * tile_size;
      const auto samples = extract_tile(image, x0, y0);
      if (compression == DngCompression::uncompressed) {
        auto &bytes = tiles[i];
        bytes.reserve(samples.size() * 2);
        for (const std::uint16_t sample : samples) {
          bytes.push_back(static_cast<std::uint8_t>(sample));
          bytes.push_back(static_cast<std::uint8_t>(sample >> 8U));
        }
        continue;
      }
      // CFA rows are coded as two interleaved components of half width, as
      // the DNG SDK does, so each color is predicted from its own kind.
      detail::LosslessJpegEncodeParams params;
      params.components =
          photometric == Photometric::cfa ? 2 : image.samples_per_pixel;
      params.width = tile_size * image.samples_per_pixel / params.components;
      params.height = tile_size;
      auto encoded = detail::encode_lossless_jpeg(samples, params);
      if (!encoded) {
        const std::scoped_lock lock(failure_mutex);
        if (!failure) {
          failure = encoded.error();
        }
        return;
      }
      tiles[i] = std::move(*encoded);
    }
  });
  if (failure) {
    return std::unexpected(*failure);
  }
  return tiles;
}

void add_camera_tags(IfdBuilder &ifd, const DngMetadata &meta) {
  ifd.add_bytes(tag::dng_version, tiff_type::byte, meta.dng_version);
  ifd.add_bytes(tag::dng_backward_version, tiff_type::byte,
                meta.dng_backward_version);
  const auto add_text = [&](std::uint16_t id, const std::string &text) {
    if (!text.empty()) {
      ifd.add_ascii(id, text);
    }
  };
  add_text(tag::make, meta.make);
  add_text(tag::model, meta.model);
  add_text(tag::software, meta.software);
  add_text(tag::date_time, meta.date_time);
  add_text(tag::artist, meta.artist);
  add_text(tag::copyright, meta.copyright);
  ifd.add_ascii(tag::unique_camera_model, meta.unique_camera_model.empty()
                                              ? std::string{"Unknown"}
                                              : meta.unique_camera_model);
  ifd.add_short(tag::orientation, meta.orientation);

  const std::array<std::array<std::uint16_t, 4>, 2> calibration_tags{
      {{tag::calibration_illuminant1, tag::color_matrix1,
        tag::camera_calibration1, tag::forward_matrix1},
       {tag::calibration_illuminant2, tag::color_matrix2,
        tag::camera_calibration2, tag::forward_matrix2}}};
  for (std::size_t i = 0; i < std::min<std::size_t>(2, meta.calibrations.size());
       ++i) {
    const ColorCalibration &calibration = meta.calibrations[i];
    const auto &ids = calibration_tags[i];
    ifd.add_short(ids[0], calibration.illuminant);
    if (calibration.color_matrix) {
      ifd.add_srational(ids[1], matrix_values(*calibration.color_matrix));
    }
    if (calibration.camera_calibration) {
      ifd.add_srational(ids[2], matrix_values(*calibration.camera_calibration));
    }
    if (calibration.forward_matrix) {
      ifd.add_srational(ids[3], matrix_values(*calibration.forward_matrix));
    }
  }
  if (meta.analog_balance) {
    ifd.add_rational(tag::analog_balance, *meta.analog_balance);
  }
  if (meta.as_shot_neutral) {
    ifd.add_rational(tag::as_shot_neutral, *meta.as_shot_neutral);
  } else if (meta.as_shot_white_xy) {
    const std::array<double, 2> xy{meta.as_shot_white_xy->x,
                                   meta.as_shot_white_xy->y};
    ifd.add_rational(tag::as_shot_white_xy, xy);
  }
  const double exposure = meta.baseline_exposure;
  ifd.add_srational(tag::baseline_exposure, std::span(&exposure, 1));
  ifd.add_rational(tag::baseline_noise, std::span(&meta.baseline_noise, 1));
  ifd.add_rational(tag::baseline_sharpness,
                   std::span(&meta.baseline_sharpness, 1));
  ifd.add_rational(tag::linear_response_limit,
                   std::span(&meta.linear_response_limit, 1));
  if (!meta.profile_name.empty()) {
    ifd.add_ascii(tag::profile_name, meta.profile_name);
  }
  if (meta.profile_embed_policy) {
    ifd.add_long(tag::profile_embed_policy, *meta.profile_embed_policy);
  }
}

void add_raw_image_tags(IfdBuilder &ifd, const DngImage &dng,
                        DngCompression compression,
                        std::vector<std::size_t> tile_blobs,
                        std::vector<std::uint32_t> tile_byte_counts) {
  const DngMetadata &meta = dng.metadata;
  const RawImage &image = dng.image;
  const std::size_t spp = image.samples_per_pixel;

  ifd.add_long(tag::new_subfile_type, 0);
  ifd.add_long(tag::image_width, image.width);
  ifd.add_long(tag::image_length, image.height);
  const std::vector<std::uint16_t> bits(spp, 16);
  ifd.add_short(tag::bits_per_sample, bits);
  ifd.add_short(tag::compression, compression == DngCompression::lossless_jpeg
                                      ? detail::compression::lossless_jpeg
                                      : detail::compression::none);
  ifd.add_short(tag::photometric_interpretation,
                meta.photometric == Photometric::cfa
                    ? detail::photometric::cfa
                    : detail::photometric::linear_raw);
  ifd.add_short(tag::samples_per_pixel, static_cast<std::uint16_t>(spp));
  ifd.add_short(tag::planar_configuration, 1);
  ifd.add_long(tag::tile_width, tile_size);
  ifd.add_long(tag::tile_length, tile_size);
  ifd.add_blob_offsets(tag::tile_offsets, std::move(tile_blobs));
  ifd.add_long(tag::tile_byte_counts, tile_byte_counts);

  if (meta.photometric == Photometric::cfa) {
    const std::array<std::uint16_t, 2> dim{2, 2};
    ifd.add_short(tag::cfa_repeat_pattern_dim, dim);
    std::array<std::uint8_t, 4> pattern{};
    for (std::size_t i = 0; i < 4; ++i) {
      pattern[i] =
          static_cast<std::uint8_t>(meta.cfa_pattern.colors[i / 2][i % 2]);
    }
    ifd.add_bytes(tag::cfa_pattern, tiff_type::byte, pattern);
    const std::array<std::uint8_t, 3> planes{0, 1, 2};
    ifd.add_bytes(tag::cfa_plane_color, tiff_type::byte, planes);
    ifd.add_short(tag::cfa_layout, 1);
  }

  if (!meta.linearization_table.empty()) {
    ifd.add_short(tag::linearization_table, meta.linearization_table);
  }
  if (!meta.black_level.empty()) {
    const std::array<std::uint16_t, 2> dim{
        static_cast<std::uint16_t>(meta.black_level_repeat_rows),
        static_cast<std::uint16_t>(meta.black_level_repeat_cols)};
    ifd.add_short(tag::black_level_repeat_dim, dim);
    ifd.add_rational(tag::black_level, meta.black_level);
  }
  if (!meta.black_level_delta_h.empty()) {
    ifd.add_srational(tag::black_level_delta_h, meta.black_level_delta_h);
  }
  if (!meta.black_level_delta_v.empty()) {
    ifd.add_srational(tag::black_level_delta_v, meta.black_level_delta_v);
  }
  // Samples are always written with 16 bits, so the white level is explicit.
  std::vector<std::uint32_t> white(spp);
  for (std::size_t i = 0; i < spp; ++i) {
    const double value =
        meta.white_level.empty()
            ? static_cast<double>((1U << meta.bits_per_sample) - 1U)
            : meta.white_level[std::min(i, meta.white_level.size() - 1)];
    white[i] = static_cast<std::uint32_t>(std::clamp(value, 1.0, 65535.0));
  }
  ifd.add_long(tag::white_level, white);

  if (meta.default_scale) {
    ifd.add_rational(tag::default_scale, *meta.default_scale);
  }
  if (meta.default_crop_origin && meta.default_crop_size) {
    ifd.add_rational(tag::default_crop_origin, *meta.default_crop_origin);
    ifd.add_rational(tag::default_crop_size, *meta.default_crop_size);
  }
  if (meta.best_quality_scale) {
    ifd.add_rational(tag::best_quality_scale,
                     std::span(&*meta.best_quality_scale, 1));
  }
  const auto add_opcodes = [&](std::uint16_t id,
                               const std::vector<std::uint8_t> &list) {
    if (!list.empty()) {
      ifd.add_bytes(id, tiff_type::undefined, list);
    }
  };
  add_opcodes(tag::opcode_list1, meta.opcode_list1);
  add_opcodes(tag::opcode_list2, meta.opcode_list2);
  add_opcodes(tag::opcode_list3, meta.opcode_list3);
}

Result<std::vector<std::uint8_t>> encode_dng_impl(const DngImage &dng,
                                                  const DngWriteOptions &options) {
  if (auto valid = validate(dng); !valid) {
    return std::unexpected(valid.error());
  }
  if (dng.metadata.bits_per_sample < 1 || dng.metadata.bits_per_sample > 16) {
    return make_error(ErrorCode::invalid_argument, "invalid bits per sample");
  }
  auto tiles = encode_tiles(dng.image, options.compression,
                            dng.metadata.photometric);
  if (!tiles) {
    return std::unexpected(tiles.error());
  }

  TiffWriter writer;
  std::vector<std::size_t> tile_blobs;
  std::vector<std::uint32_t> tile_byte_counts;
  for (auto &tile : *tiles) {
    tile_byte_counts.push_back(static_cast<std::uint32_t>(tile.size()));
    tile_blobs.push_back(writer.add_blob(std::move(tile)));
  }

  const bool has_thumbnail =
      options.thumbnail && options.thumbnail->width != 0 &&
      options.thumbnail->height != 0 &&
      options.thumbnail->pixels.size() ==
          std::size_t{options.thumbnail->width} * options.thumbnail->height * 3;

  IfdBuilder root;
  add_camera_tags(root, dng.metadata);
  IfdBuilder raw;
  add_raw_image_tags(has_thumbnail ? raw : root, dng, options.compression,
                     std::move(tile_blobs), std::move(tile_byte_counts));

  if (has_thumbnail) {
    const Rgb8Image &thumbnail = *options.thumbnail;
    const std::size_t blob = writer.add_blob(thumbnail.pixels);
    root.add_long(tag::new_subfile_type, 1);
    root.add_long(tag::image_width, thumbnail.width);
    root.add_long(tag::image_length, thumbnail.height);
    const std::array<std::uint16_t, 3> bits{8, 8, 8};
    root.add_short(tag::bits_per_sample, bits);
    root.add_short(tag::compression, detail::compression::none);
    root.add_short(tag::photometric_interpretation, detail::photometric::rgb);
    root.add_blob_offsets(tag::strip_offsets, {blob});
    root.add_short(tag::samples_per_pixel, 3);
    root.add_long(tag::rows_per_strip, thumbnail.height);
    root.add_long(tag::strip_byte_counts,
                  static_cast<std::uint32_t>(thumbnail.pixels.size()));
    root.add_short(tag::planar_configuration, 1);
    root.add_long(tag::preview_color_space, 2); // sRGB
  }

  // IFD indices: 0 root, then the raw IFD and the EXIF IFD when present.
  std::size_t next_ifd = 1;
  if (has_thumbnail) {
    root.add_ifd_offsets(tag::sub_ifds, {next_ifd++});
  }
  IfdBuilder exif;
  for (const TiffTagValue &entry : dng.metadata.exif) {
    if (entry.bytes.size() ==
        std::size_t{entry.count} * detail::tiff_type_size(entry.type)) {
      exif.add_raw(entry.tag, entry.type, entry.count, entry.bytes);
    }
  }
  if (!exif.empty()) {
    root.add_ifd_offsets(tag::exif_ifd, {next_ifd});
  }

  writer.add_ifd(std::move(root));
  if (has_thumbnail) {
    writer.add_ifd(std::move(raw));
  }
  if (!exif.empty()) {
    writer.add_ifd(std::move(exif));
  }
  return writer.serialize();
}

} // namespace

Result<std::vector<std::uint8_t>> encode_dng(const DngImage &dng,
                                             const DngWriteOptions &options) {
  try {
    return encode_dng_impl(dng, options);
  } catch (const std::bad_alloc &) {
    return make_error(ErrorCode::out_of_memory,
                      "not enough memory to encode the DNG");
  }
}

Result<void> write_dng(const std::filesystem::path &path, const DngImage &dng,
                       const DngWriteOptions &options) {
  auto bytes = encode_dng(dng, options);
  if (!bytes) {
    return std::unexpected(bytes.error());
  }

  std::random_device random;
  const auto temporary = path.parent_path() /
                         std::format(".{}.{:08x}.tmp", path.filename().string(),
                                     random());
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream) {
      return make_error(ErrorCode::io_error,
                        std::format("cannot create {}", temporary.string()));
    }
    stream.write(reinterpret_cast<const char *>(bytes->data()),
                 static_cast<std::streamsize>(bytes->size()));
    stream.close();
    if (!stream) {
      std::error_code ignored;
      std::filesystem::remove(temporary, ignored);
      return make_error(ErrorCode::io_error,
                        std::format("cannot write {}", temporary.string()));
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return make_error(ErrorCode::io_error,
                      std::format("cannot replace {}: {}", path.string(),
                                  error.message()));
  }
  return {};
}

} // namespace ircore
