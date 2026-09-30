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

namespace ircore {

namespace {

using detail::TiffEntry;
using detail::TiffFile;
using detail::TiffIfd;
namespace tag = detail::tag;

// Upper bound on decoded samples (2 GiB of 16-bit samples).
constexpr std::uint64_t max_sample_count = std::uint64_t{1} << 30U;

// Looks up raw-image tags in the raw IFD first and falls back to IFD 0,
// where DNG stores the camera-wide tags.
class TagSource {
public:
  TagSource(const TiffIfd &raw, const TiffIfd &root) : raw_(raw), root_(root) {}

  [[nodiscard]] const TiffEntry *find(std::uint16_t id) const {
    if (const TiffEntry *entry = raw_.find(id)) {
      return entry;
    }
    return root_.find(id);
  }

  [[nodiscard]] const TiffEntry *find_raw(std::uint16_t id) const {
    return raw_.find(id);
  }

private:
  const TiffIfd &raw_;
  const TiffIfd &root_;
};

std::optional<std::uint64_t> unsigned_value(const TiffEntry *entry,
                                            std::size_t index = 0) {
  if (entry == nullptr) {
    return std::nullopt;
  }
  const auto value = entry->integer(index);
  if (!value || *value < 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(*value);
}

std::optional<std::vector<double>> numbers(const TiffEntry *entry) {
  if (entry == nullptr) {
    return std::nullopt;
  }
  return entry->numbers();
}

std::optional<Matrix3> matrix(const TiffEntry *entry) {
  const auto values = numbers(entry);
  if (!values || values->size() != 9) {
    return std::nullopt;
  }
  std::array<double, 9> m{};
  std::ranges::copy(*values, m.begin());
  return Matrix3::from_row_major(m);
}

std::optional<Vector3> vector3(const TiffEntry *entry) {
  const auto values = numbers(entry);
  if (!values || values->size() != 3) {
    return std::nullopt;
  }
  return Vector3{(*values)[0], (*values)[1], (*values)[2]};
}

std::optional<std::array<double, 2>> pair_value(const TiffEntry *entry) {
  const auto values = numbers(entry);
  if (!values || values->size() != 2) {
    return std::nullopt;
  }
  return std::array<double, 2>{(*values)[0], (*values)[1]};
}

std::vector<std::uint8_t> blob(const TiffEntry *entry) {
  if (entry == nullptr) {
    return {};
  }
  return {entry->data.begin(), entry->data.end()};
}

bool is_main_raw_ifd(const TiffIfd &ifd) {
  if (ifd.is_exif) {
    return false;
  }
  const auto subfile = unsigned_value(ifd.find(tag::new_subfile_type));
  if (subfile.value_or(0) != 0) {
    return false;
  }
  const auto photometric =
      unsigned_value(ifd.find(tag::photometric_interpretation));
  return photometric == detail::photometric::cfa ||
         photometric == detail::photometric::linear_raw;
}

struct Chunk {
  std::uint32_t x{};
  std::uint32_t y{};
  std::uint32_t width{};
  std::uint32_t height{};
  std::span<const std::uint8_t> data;
};

struct Layout {
  std::uint32_t width{};
  std::uint32_t height{};
  std::uint32_t samples_per_pixel{};
  std::uint32_t bits_per_sample{};
  std::uint16_t compression{};
  bool big_endian{};
};

Result<std::vector<Chunk>> collect_chunks(const TiffIfd &ifd,
                                          std::span<const std::uint8_t> bytes,
                                          const Layout &layout) {
  const TiffEntry *offsets = ifd.find(tag::tile_offsets);
  const TiffEntry *counts = ifd.find(tag::tile_byte_counts);
  std::uint64_t chunk_width = layout.width;
  std::uint64_t chunk_height = layout.height;
  if (offsets != nullptr) {
    chunk_width = unsigned_value(ifd.find(tag::tile_width)).value_or(0);
    chunk_height = unsigned_value(ifd.find(tag::tile_length)).value_or(0);
  } else {
    offsets = ifd.find(tag::strip_offsets);
    counts = ifd.find(tag::strip_byte_counts);
    chunk_height = std::min<std::uint64_t>(
        unsigned_value(ifd.find(tag::rows_per_strip)).value_or(layout.height),
        layout.height);
  }
  if (offsets == nullptr || counts == nullptr || chunk_width == 0 ||
      chunk_height == 0 || chunk_width > 65536 || chunk_height > 65536 * 16) {
    return make_error(ErrorCode::invalid_format,
                      "raw image has no valid strip or tile layout");
  }
  const std::uint64_t across = (layout.width + chunk_width - 1) / chunk_width;
  const std::uint64_t down = (layout.height + chunk_height - 1) / chunk_height;
  const std::uint64_t count = across * down;
  if (offsets->count < count || counts->count < count) {
    return make_error(ErrorCode::invalid_format,
                      "raw image is missing strips or tiles");
  }

  std::vector<Chunk> chunks;
  chunks.reserve(static_cast<std::size_t>(count));
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto offset = unsigned_value(offsets, static_cast<std::size_t>(i));
    const auto size = unsigned_value(counts, static_cast<std::size_t>(i));
    if (!offset || !size || *offset > bytes.size() ||
        bytes.size() - *offset < *size) {
      return make_error(ErrorCode::invalid_format,
                        "raw image data is outside of the file");
    }
    Chunk chunk;
    chunk.x = static_cast<std::uint32_t>((i % across) * chunk_width);
    chunk.y = static_cast<std::uint32_t>((i / across) * chunk_height);
    chunk.width = static_cast<std::uint32_t>(chunk_width);
    chunk.height = static_cast<std::uint32_t>(chunk_height);
    if (offsets == ifd.find(tag::strip_offsets)) {
      chunk.height = std::min(chunk.height, layout.height - chunk.y);
    }
    chunk.data = bytes.subspan(static_cast<std::size_t>(*offset),
                               static_cast<std::size_t>(*size));
    chunks.push_back(chunk);
  }
  return chunks;
}

// Copies a decoded chunk (rows of `stride` samples) into the image.
void store_chunk(const Chunk &chunk, const std::uint16_t *samples,
                 std::size_t stride, RawImage &image) {
  const std::size_t spp = image.samples_per_pixel;
  const std::size_t columns =
      std::min<std::size_t>(chunk.width, image.width - chunk.x);
  const std::size_t rows =
      std::min<std::size_t>(chunk.height, image.height - chunk.y);
  for (std::size_t row = 0; row < rows; ++row) {
    const std::uint16_t *source = samples + row * stride;
    std::uint16_t *target =
        image.samples.data() + ((chunk.y + row) * image.width + chunk.x) * spp;
    std::copy_n(source, columns * spp, target);
  }
}

Result<void> decode_uncompressed(const Chunk &chunk, const Layout &layout,
                                 RawImage &image) {
  const std::size_t row_samples =
      std::size_t{chunk.width} * layout.samples_per_pixel;
  const std::size_t row_bytes =
      (row_samples * layout.bits_per_sample + 7) / 8;
  if (chunk.data.size() / row_bytes < chunk.height) {
    return make_error(ErrorCode::invalid_format,
                      "uncompressed raw data is truncated");
  }
  std::vector<std::uint16_t> samples(row_samples * chunk.height);
  for (std::size_t row = 0; row < chunk.height; ++row) {
    const std::uint8_t *source = chunk.data.data() + row * row_bytes;
    std::uint16_t *target = samples.data() + row * row_samples;
    if (layout.bits_per_sample == 16) {
      for (std::size_t i = 0; i < row_samples; ++i) {
        const std::uint16_t a = source[i * 2];
        const std::uint16_t b = source[i * 2 + 1];
        target[i] = static_cast<std::uint16_t>(
            layout.big_endian ? (a << 8U) | b : (b << 8U) | a);
      }
    } else if (layout.bits_per_sample == 8) {
      std::copy_n(source, row_samples, target);
    } else {
      // Bit-packed samples, most significant bit first; rows start on a byte
      // boundary.
      std::uint64_t buffer = 0;
      std::uint32_t available = 0;
      std::size_t at = 0;
      for (std::size_t i = 0; i < row_samples; ++i) {
        while (available < layout.bits_per_sample) {
          buffer = (buffer << 8U) | source[at++];
          available += 8;
        }
        available -= layout.bits_per_sample;
        target[i] = static_cast<std::uint16_t>(
            (buffer >> available) & ((1U << layout.bits_per_sample) - 1U));
      }
    }
  }
  store_chunk(chunk, samples.data(), row_samples, image);
  return {};
}

Result<void> decode_lossless_jpeg_chunk(const Chunk &chunk,
                                        const Layout &layout, RawImage &image) {
  auto decoded = detail::decode_lossless_jpeg(chunk.data);
  if (!decoded) {
    return std::unexpected(decoded.error());
  }
  const std::size_t row_samples =
      std::size_t{chunk.width} * layout.samples_per_pixel;
  const std::size_t frame_row =
      std::size_t{decoded->width} * decoded->components;
  const std::size_t total = decoded->samples.size();
  // DNG writers may split rows into several JPEG components (for example two
  // components of half width); the sample order stays row-major.
  const std::size_t stride =
      total == row_samples * chunk.height ? row_samples : frame_row;
  if (stride < row_samples ||
      total < (std::size_t{chunk.height} - 1) * stride + row_samples) {
    return make_error(ErrorCode::invalid_format,
                      "lossless JPEG tile size does not match the image");
  }
  store_chunk(chunk, decoded->samples.data(), stride, image);
  return {};
}

Result<RawImage> decode_raw_image(const TiffIfd &ifd,
                                  std::span<const std::uint8_t> bytes,
                                  bool big_endian, Photometric photometric) {
  Layout layout;
  layout.big_endian = big_endian;
  const auto width = unsigned_value(ifd.find(tag::image_width));
  const auto height = unsigned_value(ifd.find(tag::image_length));
  const auto spp = unsigned_value(ifd.find(tag::samples_per_pixel)).value_or(1);
  const auto bps = unsigned_value(ifd.find(tag::bits_per_sample)).value_or(1);
  const auto compression =
      unsigned_value(ifd.find(tag::compression)).value_or(1);
  const auto planar =
      unsigned_value(ifd.find(tag::planar_configuration)).value_or(1);
  const auto format = unsigned_value(ifd.find(tag::sample_format)).value_or(1);

  if (!width || !height || *width == 0 || *height == 0 || *width > 65535 * 4 ||
      *height > 65535 * 4) {
    return make_error(ErrorCode::invalid_format, "invalid raw image size");
  }
  if (format != 1) {
    return make_error(ErrorCode::unsupported_format,
                      "floating point raw data is not supported");
  }
  if (planar != 1) {
    return make_error(ErrorCode::unsupported_format,
                      "planar raw data is not supported");
  }
  const std::uint64_t expected_spp = photometric == Photometric::cfa ? 1 : 3;
  if (spp != expected_spp) {
    return make_error(
        ErrorCode::unsupported_format,
        std::format("{} samples per pixel are not supported for {} images", spp,
                    photometric == Photometric::cfa ? "CFA" : "LinearRaw"));
  }
  if (bps < 8 || bps > 16) {
    return make_error(ErrorCode::unsupported_format,
                      std::format("{} bits per sample are not supported", bps));
  }
  if (compression != detail::compression::none &&
      compression != detail::compression::lossless_jpeg) {
    return make_error(
        ErrorCode::unsupported_format,
        std::format("raw compression {} is not supported (only uncompressed "
                    "and lossless JPEG; JPEG XL and lossy DNGs are not)",
                    compression));
  }
  if (*width * *height * spp > max_sample_count) {
    return make_error(ErrorCode::unsupported_format, "raw image is too large");
  }
  layout.width = static_cast<std::uint32_t>(*width);
  layout.height = static_cast<std::uint32_t>(*height);
  layout.samples_per_pixel = static_cast<std::uint32_t>(spp);
  layout.bits_per_sample = static_cast<std::uint32_t>(bps);
  layout.compression = static_cast<std::uint16_t>(compression);

  auto chunks = collect_chunks(ifd, bytes, layout);
  if (!chunks) {
    return std::unexpected(chunks.error());
  }

  RawImage image;
  image.width = layout.width;
  image.height = layout.height;
  image.samples_per_pixel = layout.samples_per_pixel;
  image.samples.assign(image.sample_count(), 0);

  std::optional<Error> failure;
  std::mutex failure_mutex;
  detail::parallel_for(chunks->size(), 0, [&](std::size_t begin,
                                              std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
      const Chunk &chunk = (*chunks)[i];
      auto decoded = layout.compression == detail::compression::none
                         ? decode_uncompressed(chunk, layout, image)
                         : decode_lossless_jpeg_chunk(chunk, layout, image);
      if (!decoded) {
        const std::scoped_lock lock(failure_mutex);
        if (!failure) {
          failure = decoded.error();
        }
        return;
      }
    }
  });
  if (failure) {
    return std::unexpected(*failure);
  }
  return image;
}

Result<CfaPattern> read_cfa_pattern(const TagSource &tags) {
  const TiffEntry *dim = tags.find_raw(tag::cfa_repeat_pattern_dim);
  const TiffEntry *pattern = tags.find_raw(tag::cfa_pattern);
  if (dim == nullptr || pattern == nullptr) {
    return make_error(ErrorCode::invalid_format, "CFA pattern is missing");
  }
  if (unsigned_value(dim, 0) != 2 || unsigned_value(dim, 1) != 2 ||
      pattern->count != 4) {
    return make_error(ErrorCode::unsupported_format,
                      "only 2x2 Bayer CFA patterns are supported");
  }
  if (const auto layout = unsigned_value(tags.find_raw(tag::cfa_layout))) {
    if (*layout != 1) {
      return make_error(ErrorCode::unsupported_format,
                        "non-rectangular CFA layouts are not supported");
    }
  }
  // Pattern values index CFAPlaneColor, which defaults to red, green, blue.
  std::array<std::uint64_t, 3> planes{0, 1, 2};
  if (const TiffEntry *plane_color = tags.find_raw(tag::cfa_plane_color)) {
    if (plane_color->count != 3) {
      return make_error(ErrorCode::unsupported_format,
                        "only three-color CFA sensors are supported");
    }
    for (std::size_t i = 0; i < 3; ++i) {
      planes[i] = unsigned_value(plane_color, i).value_or(99);
    }
  }
  CfaPattern cfa;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto value = unsigned_value(pattern, i);
    if (!value || *value > 2 || planes[*value] > 2) {
      return make_error(ErrorCode::unsupported_format,
                        "CFA pattern contains non-RGB colors");
    }
    cfa.colors[i / 2][i % 2] =
        static_cast<CfaColor>(planes[static_cast<std::size_t>(*value)]);
  }
  if (!cfa.is_bayer()) {
    return make_error(ErrorCode::unsupported_format,
                      "only Bayer CFA patterns are supported");
  }
  return cfa;
}

std::vector<TiffTagValue> read_exif(const TiffFile &file, const TiffIfd &root) {
  std::vector<TiffTagValue> result;
  if (!root.exif_ifd) {
    return result;
  }
  constexpr std::size_t max_exif_value = std::size_t{1} << 20U;
  for (const TiffEntry &entry : file.ifds[*root.exif_ifd].entries) {
    if (entry.tag == tag::exif_maker_note ||
        entry.tag == tag::exif_interoperability_ifd ||
        entry.type == detail::tiff_type::ifd ||
        entry.data.size() > max_exif_value) {
      continue;
    }
    result.push_back(
        {entry.tag, entry.type, entry.count, entry.little_endian_bytes()});
  }
  return result;
}

Result<DngMetadata> read_metadata(const TiffFile &file, const TagSource &tags,
                                  const TiffIfd &root, Photometric photometric,
                                  const RawImage &image) {
  DngMetadata meta;
  const TiffEntry *version = root.find(tag::dng_version);
  if (version == nullptr || version->count != 4) {
    return make_error(ErrorCode::invalid_format,
                      "not a DNG file (DNGVersion is missing)");
  }
  for (std::size_t i = 0; i < 4; ++i) {
    meta.dng_version[i] =
        static_cast<std::uint8_t>(unsigned_value(version, i).value_or(0));
  }
  if (const TiffEntry *backward = root.find(tag::dng_backward_version);
      backward != nullptr && backward->count == 4) {
    for (std::size_t i = 0; i < 4; ++i) {
      meta.dng_backward_version[i] =
          static_cast<std::uint8_t>(unsigned_value(backward, i).value_or(0));
    }
  }

  const auto text = [&](std::uint16_t id) {
    const TiffEntry *entry = tags.find(id);
    return entry == nullptr ? std::string{} : entry->text();
  };
  meta.make = text(tag::make);
  meta.model = text(tag::model);
  meta.unique_camera_model = text(tag::unique_camera_model);
  meta.software = text(tag::software);
  meta.date_time = text(tag::date_time);
  meta.artist = text(tag::artist);
  meta.copyright = text(tag::copyright);
  meta.profile_name = text(tag::profile_name);
  meta.orientation = static_cast<std::uint16_t>(
      unsigned_value(root.find(tag::orientation)).value_or(1));
  if (meta.orientation < 1 || meta.orientation > 8) {
    meta.orientation = 1;
  }

  meta.photometric = photometric;
  meta.bits_per_sample = static_cast<std::uint16_t>(
      unsigned_value(tags.find_raw(tag::bits_per_sample)).value_or(16));

  if (const TiffEntry *table = tags.find_raw(tag::linearization_table)) {
    for (std::size_t i = 0; i < table->count; ++i) {
      meta.linearization_table.push_back(
          static_cast<std::uint16_t>(unsigned_value(table, i).value_or(0)));
    }
  }

  const std::size_t spp = image.samples_per_pixel;
  if (const TiffEntry *dim = tags.find_raw(tag::black_level_repeat_dim)) {
    meta.black_level_repeat_rows =
        static_cast<std::uint32_t>(unsigned_value(dim, 0).value_or(1));
    meta.black_level_repeat_cols =
        static_cast<std::uint32_t>(unsigned_value(dim, 1).value_or(1));
  }
  if (meta.black_level_repeat_rows == 0 || meta.black_level_repeat_cols == 0 ||
      meta.black_level_repeat_rows > 64 || meta.black_level_repeat_cols > 64) {
    return make_error(ErrorCode::invalid_format,
                      "invalid BlackLevelRepeatDim");
  }
  if (const TiffEntry *black = tags.find_raw(tag::black_level)) {
    const auto values = black->numbers();
    const std::size_t expected = std::size_t{meta.black_level_repeat_rows} *
                                 meta.black_level_repeat_cols * spp;
    if (!values || values->size() != expected) {
      return make_error(ErrorCode::invalid_format,
                        "BlackLevel does not match BlackLevelRepeatDim");
    }
    meta.black_level = *values;
  }
  if (const auto delta = numbers(tags.find_raw(tag::black_level_delta_h))) {
    if (delta->size() != image.width) {
      return make_error(ErrorCode::invalid_format,
                        "BlackLevelDeltaH does not match the active width");
    }
    meta.black_level_delta_h = *delta;
  }
  if (const auto delta = numbers(tags.find_raw(tag::black_level_delta_v))) {
    if (delta->size() != image.height) {
      return make_error(ErrorCode::invalid_format,
                        "BlackLevelDeltaV does not match the active height");
    }
    meta.black_level_delta_v = *delta;
  }
  if (const auto white = numbers(tags.find_raw(tag::white_level))) {
    if (white->size() != spp && white->size() != 1) {
      return make_error(ErrorCode::invalid_format,
                        "WhiteLevel does not match SamplesPerPixel");
    }
    meta.white_level.assign(spp, white->front());
    if (white->size() == spp) {
      meta.white_level = *white;
    }
  }

  meta.default_scale = pair_value(tags.find_raw(tag::default_scale));
  meta.default_crop_origin = pair_value(tags.find_raw(tag::default_crop_origin));
  meta.default_crop_size = pair_value(tags.find_raw(tag::default_crop_size));
  if (const auto scale = numbers(tags.find_raw(tag::best_quality_scale));
      scale && scale->size() == 1) {
    meta.best_quality_scale = scale->front();
  }

  const std::array<std::array<std::uint16_t, 4>, 2> calibration_tags{
      {{tag::calibration_illuminant1, tag::color_matrix1,
        tag::camera_calibration1, tag::forward_matrix1},
       {tag::calibration_illuminant2, tag::color_matrix2,
        tag::camera_calibration2, tag::forward_matrix2}}};
  for (const auto &ids : calibration_tags) {
    ColorCalibration calibration;
    calibration.color_matrix = matrix(tags.find(ids[1]));
    if (!calibration.color_matrix) {
      continue;
    }
    calibration.illuminant = static_cast<std::uint16_t>(
        unsigned_value(tags.find(ids[0])).value_or(light_source::unknown));
    calibration.camera_calibration = matrix(tags.find(ids[2]));
    calibration.forward_matrix = matrix(tags.find(ids[3]));
    meta.calibrations.push_back(calibration);
  }
  meta.analog_balance = vector3(tags.find(tag::analog_balance));
  meta.as_shot_neutral = vector3(tags.find(tag::as_shot_neutral));
  if (const auto xy = pair_value(tags.find(tag::as_shot_white_xy))) {
    meta.as_shot_white_xy = Chromaticity{(*xy)[0], (*xy)[1]};
  }

  const auto scalar = [&](std::uint16_t id, double fallback) {
    const auto values = numbers(tags.find(id));
    return values && values->size() == 1 ? values->front() : fallback;
  };
  meta.baseline_exposure = scalar(tag::baseline_exposure, 0.0);
  meta.baseline_noise = scalar(tag::baseline_noise, 1.0);
  meta.baseline_sharpness = scalar(tag::baseline_sharpness, 1.0);
  meta.linear_response_limit = scalar(tag::linear_response_limit, 1.0);
  if (const auto policy = unsigned_value(tags.find(tag::profile_embed_policy))) {
    meta.profile_embed_policy = static_cast<std::uint32_t>(*policy);
  }

  meta.opcode_list1 = blob(tags.find_raw(tag::opcode_list1));
  meta.opcode_list2 = blob(tags.find_raw(tag::opcode_list2));
  meta.opcode_list3 = blob(tags.find_raw(tag::opcode_list3));
  meta.exif = read_exif(file, root);
  return meta;
}

Result<DngImage> read_dng_impl(std::span<const std::uint8_t> bytes) {
  auto file = detail::parse_tiff(bytes);
  if (!file) {
    return std::unexpected(file.error());
  }
  const TiffIfd &root = file->ifds.front();
  if (root.find(tag::dng_version) == nullptr) {
    return make_error(ErrorCode::invalid_format,
                      "not a DNG file (DNGVersion is missing)");
  }

  const TiffIfd *raw = nullptr;
  std::uint64_t best_area = 0;
  for (const TiffIfd &ifd : file->ifds) {
    if (!is_main_raw_ifd(ifd)) {
      continue;
    }
    const std::uint64_t area =
        unsigned_value(ifd.find(tag::image_width)).value_or(0) *
        unsigned_value(ifd.find(tag::image_length)).value_or(0);
    if (raw == nullptr || area > best_area) {
      raw = &ifd;
      best_area = area;
    }
  }
  if (raw == nullptr) {
    return make_error(ErrorCode::unsupported_format,
                      "DNG has no CFA or LinearRaw main image");
  }
  const TagSource tags(*raw, root);
  const Photometric photometric =
      unsigned_value(raw->find(tag::photometric_interpretation)) ==
              detail::photometric::cfa
          ? Photometric::cfa
          : Photometric::linear_raw;

  auto image = decode_raw_image(*raw, bytes, file->big_endian, photometric);
  if (!image) {
    return std::unexpected(image.error());
  }

  CfaPattern cfa;
  if (photometric == Photometric::cfa) {
    auto pattern = read_cfa_pattern(tags);
    if (!pattern) {
      return std::unexpected(pattern.error());
    }
    cfa = *pattern;
  }

  // Crop to the active area. The CFA pattern is relative to the stored image
  // origin, so its phase follows the crop offset.
  if (const TiffEntry *area = raw->find(tag::active_area)) {
    const auto top = unsigned_value(area, 0);
    const auto left = unsigned_value(area, 1);
    const auto bottom = unsigned_value(area, 2);
    const auto right = unsigned_value(area, 3);
    if (!top || !left || !bottom || !right || *bottom <= *top ||
        *right <= *left || *bottom > image->height || *right > image->width) {
      return make_error(ErrorCode::invalid_format, "invalid ActiveArea");
    }
    if (*top != 0 || *left != 0 || *bottom != image->height ||
        *right != image->width) {
      RawImage cropped;
      cropped.width = static_cast<std::uint32_t>(*right - *left);
      cropped.height = static_cast<std::uint32_t>(*bottom - *top);
      cropped.samples_per_pixel = image->samples_per_pixel;
      cropped.samples.resize(cropped.sample_count());
      const std::size_t spp = image->samples_per_pixel;
      for (std::size_t row = 0; row < cropped.height; ++row) {
        const auto source = image->samples.begin() +
                            static_cast<std::ptrdiff_t>(
                                ((row + *top) * image->width + *left) * spp);
        std::copy_n(source, std::size_t{cropped.width} * spp,
                    cropped.samples.begin() +
                        static_cast<std::ptrdiff_t>(row * cropped.width * spp));
      }
      *image = std::move(cropped);
      cfa = cfa.shifted(static_cast<std::size_t>(*top),
                        static_cast<std::size_t>(*left));
    }
  }

  auto metadata = read_metadata(*file, tags, root, photometric, *image);
  if (!metadata) {
    return std::unexpected(metadata.error());
  }
  metadata->cfa_pattern = cfa;
  return DngImage{std::move(*metadata), std::move(*image)};
}

} // namespace

Result<DngImage> read_dng(std::span<const std::uint8_t> bytes) {
  try {
    return read_dng_impl(bytes);
  } catch (const std::bad_alloc &) {
    return make_error(ErrorCode::out_of_memory,
                      "not enough memory to read the DNG");
  }
}

Result<DngImage> read_dng(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return make_error(ErrorCode::io_error,
                      std::format("cannot open {}", path.string()));
  }
  std::error_code error;
  const auto size = std::filesystem::file_size(path, error);
  if (error) {
    return make_error(ErrorCode::io_error,
                      std::format("cannot read size of {}", path.string()));
  }
  if (size > (std::uint64_t{1} << 32U)) {
    return make_error(ErrorCode::unsupported_format,
                      "files larger than 4 GiB are not supported");
  }
  std::vector<std::uint8_t> bytes;
  try {
    bytes.resize(static_cast<std::size_t>(size));
  } catch (const std::bad_alloc &) {
    return make_error(ErrorCode::out_of_memory,
                      "not enough memory to read the DNG");
  }
  if (!stream.read(reinterpret_cast<char *>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()))) {
    return make_error(ErrorCode::io_error,
                      std::format("cannot read {}", path.string()));
  }
  return read_dng(bytes);
}

} // namespace ircore
