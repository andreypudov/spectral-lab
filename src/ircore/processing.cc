#include "ircore/processing.h"
#include "ircore/internal/demosaic.h"
#include "ircore/internal/normalization.h"
#include "ircore/internal/parallel.h"
#include "ircore/preview.h"
#include "ircore/version.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <new>

namespace ircore {

namespace detail {

const ColorCalibration *reference_calibration(const DngMetadata &meta) {
  const ColorCalibration *best = nullptr;
  double best_distance = 0.0;
  constexpr double d65_mired = 1.0e6 / 6504.0;
  for (const ColorCalibration &calibration : meta.calibrations) {
    if (!calibration.color_matrix) {
      continue;
    }
    const double distance = std::abs(
        1.0e6 / illuminant_temperature(calibration.illuminant) - d65_mired);
    if (best == nullptr || distance < best_distance) {
      best = &calibration;
      best_distance = distance;
    }
  }
  return best;
}

Result<CameraTransform> camera_transform(const DngMetadata &meta,
                                         const Vector3 &camera_neutral) {
  CameraTransform result;
  const ColorCalibration *calibration = reference_calibration(meta);
  if (calibration == nullptr) {
    // Without a color matrix the camera is assumed to record linear sRGB.
    result.balanced_to_xyz = srgb_to_xyz_d50();
    result.warnings.emplace_back(
        "the DNG has no color matrix; camera RGB is treated as linear sRGB");
    return result;
  }
  result.illuminant = calibration->illuminant;

  // Camera space = AnalogBalance * CameraCalibration * reference space.
  Matrix3 calibration_matrix = Matrix3::diagonal(
      meta.analog_balance.value_or(Vector3{1.0, 1.0, 1.0}));
  if (calibration->camera_calibration) {
    calibration_matrix = calibration_matrix * *calibration->camera_calibration;
  }
  auto to_reference = inverse(calibration_matrix);
  Vector3 reference_neutral{};
  if (to_reference) {
    reference_neutral = *to_reference * camera_neutral;
  }
  if (!to_reference || min_component(reference_neutral) <= 0.0) {
    result.warnings.emplace_back(
        "AnalogBalance/CameraCalibration are unusable and were ignored");
    to_reference = Matrix3::identity();
    reference_neutral = camera_neutral;
  }
  const Matrix3 unbalance =
      Matrix3::diagonal({1.0 / reference_neutral[0], 1.0 / reference_neutral[1],
                         1.0 / reference_neutral[2]});

  // camera_to_xyz maps camera RGB (not balanced) to XYZ D50 such that the
  // camera neutral becomes D50 white.
  Matrix3 camera_to_xyz;
  if (calibration->forward_matrix) {
    camera_to_xyz = *calibration->forward_matrix * unbalance * *to_reference;
    result.used_forward_matrix = true;
  } else {
    const auto xyz_to_camera = *calibration->color_matrix;
    const auto camera_to_reference_xyz = inverse(xyz_to_camera);
    const Vector3 white = xy_to_xyz(illuminant_white(calibration->illuminant));
    const Vector3 illuminant_neutral = xyz_to_camera * white;
    if (!camera_to_reference_xyz || min_component(illuminant_neutral) <= 0.0) {
      return make_error(ErrorCode::processing_failed,
                        "the DNG color matrix is not invertible");
    }
    camera_to_xyz = bradford_adaptation(
                        illuminant_white(calibration->illuminant), white::d50) *
                    *camera_to_reference_xyz *
                    Matrix3::diagonal(illuminant_neutral) * unbalance *
                    *to_reference;
  }
  result.balanced_to_xyz = camera_to_xyz * Matrix3::diagonal(camera_neutral);
  return result;
}

Result<Matrix3> output_transform(const Matrix3 &balanced_to_xyz,
                                 ChannelMapping mapping) {
  Matrix3 transform;
  switch (mapping) {
  case ChannelMapping::native:
    transform = xyz_d50_to_prophoto() * balanced_to_xyz;
    break;
  case ChannelMapping::false_color: {
    // Red/blue swap in sRGB primaries, as the classic Photoshop channel
    // mixer swap does; the tone curve commutes with the swap.
    const Matrix3 swap =
        Matrix3::from_row_major({0.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 0.0});
    transform = xyz_d50_to_prophoto() * srgb_to_xyz_d50() * swap *
                xyz_d50_to_srgb() * balanced_to_xyz;
    break;
  }
  case ChannelMapping::monochrome:
    // Equal weights of the balanced channels: the noise-optimal gray for IR,
    // where all channels carry similar signal.
    for (auto &row : transform.m) {
      row = {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
    }
    break;
  case ChannelMapping::auto_select:
    return make_error(ErrorCode::invalid_argument,
                      "channel mapping must be resolved first");
  }

  // Remove numeric drift so that the IR neutral is exactly white.
  const Vector3 white = transform * Vector3{1.0, 1.0, 1.0};
  for (std::size_t row = 0; row < 3; ++row) {
    if (!(white[row] > 0.5 && white[row] < 2.0)) {
      return make_error(ErrorCode::processing_failed,
                        "camera matrices do not map the neutral to white");
    }
    for (double &value : transform.m[row]) {
      value /= white[row];
    }
  }
  return transform;
}

void gamut_map(std::array<float, 3> &rgb) {
  // Luminance row of ProPhoto RGB to XYZ D50.
  constexpr float wr = 0.2880402F;
  constexpr float wg = 0.7118741F;
  constexpr float wb = 0.0000857F;
  const float y = wr * rgb[0] + wg * rgb[1] + wb * rgb[2];
  if (!(y > 0.0F)) {
    rgb = {0.0F, 0.0F, 0.0F};
    return;
  }
  if (y >= 1.0F) {
    rgb = {1.0F, 1.0F, 1.0F};
    return;
  }
  const float low = std::min({rgb[0], rgb[1], rgb[2]});
  const float high = std::max({rgb[0], rgb[1], rgb[2]});
  float t = 1.0F;
  if (low < 0.0F) {
    t = std::min(t, y / (y - low));
  }
  if (high > 1.0F) {
    t = std::min(t, (1.0F - y) / (high - y));
  }
  for (float &value : rgb) {
    value = std::clamp(y + t * (value - y), 0.0F, 1.0F);
  }
}

Result<Vector3> neutral_from_white_xy(const DngMetadata &meta,
                                      Chromaticity xy) {
  const ColorCalibration *calibration = reference_calibration(meta);
  if (calibration == nullptr || xy.y <= 0.0) {
    return make_error(ErrorCode::processing_failed,
                      "cannot convert AsShotWhiteXY without a color matrix");
  }
  Matrix3 xyz_to_camera =
      Matrix3::diagonal(meta.analog_balance.value_or(Vector3{1.0, 1.0, 1.0}));
  if (calibration->camera_calibration) {
    xyz_to_camera = xyz_to_camera * *calibration->camera_calibration;
  }
  xyz_to_camera = xyz_to_camera * *calibration->color_matrix;
  return xyz_to_camera * xy_to_xyz(xy);
}

} // namespace detail

namespace {

constexpr std::uint32_t min_dimension = 8;
constexpr std::uint32_t thumbnail_size = 256;
// Largest white balance gain accepted between channels.
constexpr double max_channel_gain = 1000.0;

struct Analysis {
  LinearImage linear;
  Vector3 neutral{1.0, 1.0, 1.0};
  ProcessingReport report;
};

Result<Vector3> normalized_neutral(const Vector3 &neutral,
                                   std::string_view source) {
  for (const double value : neutral) {
    if (!std::isfinite(value) || value <= 0.0) {
      return make_error(ErrorCode::invalid_argument,
                        std::format("{} white balance neutral must be positive",
                                    source));
    }
  }
  const double largest = max_component(neutral);
  const Vector3 result{neutral[0] / largest, neutral[1] / largest,
                       neutral[2] / largest};
  if (min_component(result) < 1.0 / max_channel_gain) {
    return make_error(ErrorCode::processing_failed,
                      std::format("{} white balance needs a channel gain above "
                                  "{}x",
                                  source, max_channel_gain));
  }
  return result;
}

std::string camera_name(const DngMetadata &meta) {
  if (meta.model.empty()) {
    return meta.unique_camera_model.empty() ? meta.make
                                            : meta.unique_camera_model;
  }
  if (meta.make.empty() || meta.model.starts_with(meta.make)) {
    return meta.model;
  }
  return meta.make + " " + meta.model;
}

Result<Analysis> analyze_image(const DngImage &input,
                               const ProcessingOptions &options) {
  const DngMetadata &meta = input.metadata;
  if (input.image.width < min_dimension || input.image.height < min_dimension) {
    return make_error(ErrorCode::unsupported_format,
                      std::format("image must be at least {0}x{0} pixels",
                                  min_dimension));
  }
  if (meta.photometric == Photometric::cfa && !meta.cfa_pattern.is_bayer()) {
    return make_error(ErrorCode::unsupported_format,
                      "only Bayer CFA images are supported");
  }

  Analysis analysis;
  ProcessingReport &report = analysis.report;
  report.camera = camera_name(meta);
  report.width = input.image.width;
  report.height = input.image.height;
  report.input_photometric = meta.photometric;
  report.white_balance_mode = options.white_balance.mode;

  auto linear = linearize(input, options.threads);
  if (!linear) {
    return std::unexpected(linear.error());
  }
  analysis.linear = std::move(*linear);

  auto stats = collect_statistics(analysis.linear);
  if (!stats) {
    return std::unexpected(stats.error());
  }
  report.clipped_fraction =
      stats->inspected == 0 ? 0.0
                            : static_cast<double>(stats->clipped) /
                                  static_cast<double>(stats->inspected);
  auto gray_world = estimate_neutral(*stats);
  if (!gray_world) {
    return std::unexpected(gray_world.error());
  }

  Result<Vector3> neutral = *gray_world;
  switch (options.white_balance.mode) {
  case WhiteBalanceMode::auto_ir:
    break;
  case WhiteBalanceMode::as_shot:
    if (meta.as_shot_neutral) {
      neutral = normalized_neutral(*meta.as_shot_neutral, "as-shot");
    } else if (meta.as_shot_white_xy) {
      neutral = detail::neutral_from_white_xy(meta, *meta.as_shot_white_xy);
      if (neutral) {
        neutral = normalized_neutral(*neutral, "as-shot");
      }
    } else {
      return make_error(ErrorCode::processing_failed,
                        "the DNG has no as-shot white balance; use the "
                        "automatic white balance instead");
    }
    break;
  case WhiteBalanceMode::reference_area: {
    auto area = collect_statistics(analysis.linear, options.white_balance.area);
    if (!area) {
      return std::unexpected(area.error());
    }
    neutral = estimate_neutral(*area);
    break;
  }
  case WhiteBalanceMode::explicit_neutral:
    neutral = normalized_neutral(options.white_balance.neutral, "explicit");
    break;
  }
  if (neutral) {
    neutral = normalized_neutral(*neutral, to_string(options.white_balance.mode));
  }
  if (!neutral) {
    return std::unexpected(neutral.error());
  }
  analysis.neutral = *neutral;
  report.camera_neutral = *neutral;

  report.detection = detect_filter(*stats, *gray_world);
  report.filter_detected = options.filter == FilterType::auto_detect;
  report.filter =
      report.filter_detected ? report.detection.filter : options.filter;
  report.mapping = resolve_mapping(options.mapping, report.filter);

  if (report.filter_detected &&
      report.detection.confidence == DetectionConfidence::low) {
    report.warnings.push_back(std::format(
        "filter detection is uncertain (detected {} nm); set the filter "
        "explicitly if the result looks wrong",
        to_string(report.filter)));
  }
  if (report.clipped_fraction > 0.05) {
    report.warnings.push_back(
        std::format("{:.1f}% of the frame is clipped; clipped areas are "
                    "rendered white",
                    report.clipped_fraction * 100.0));
  }
  if (!meta.opcode_list1.empty() || !meta.opcode_list2.empty()) {
    report.warnings.emplace_back(
        "raw-stage opcodes (OpcodeList1/2, e.g. lens shading gain maps) are "
        "not applied yet");
  }
  return analysis;
}

DngMetadata output_metadata(const DngMetadata &in, const RawImage &image,
                            ProcessingReport &report) {
  DngMetadata out;
  out.dng_version = {1, 4, 0, 0};
  // LinearRaw with ForwardMatrix needs DNG 1.2; opcode lists need DNG 1.3.
  out.dng_backward_version = {1, in.opcode_list3.empty() ? std::uint8_t{2}
                                                          : std::uint8_t{3},
                              0, 0};
  out.make = in.make;
  out.model = in.model;
  out.unique_camera_model = std::string(output_camera_model);
  out.software = std::format("Spectral Lab {}", version());
  out.date_time = in.date_time;
  out.artist = in.artist;
  out.copyright = in.copyright;
  out.orientation = in.orientation;

  out.photometric = Photometric::linear_raw;
  out.bits_per_sample = 16;
  out.white_level = {65535.0, 65535.0, 65535.0};

  out.default_scale = in.default_scale;
  out.best_quality_scale = in.best_quality_scale;
  if (in.default_crop_origin && in.default_crop_size) {
    const auto &origin = *in.default_crop_origin;
    const auto &size = *in.default_crop_size;
    if (origin[0] >= 0.0 && origin[1] >= 0.0 && size[0] > 0.0 &&
        size[1] > 0.0 && origin[0] + size[0] <= image.width &&
        origin[1] + size[1] <= image.height) {
      out.default_crop_origin = origin;
      out.default_crop_size = size;
    } else {
      report.warnings.emplace_back(
          "the default crop exceeds the image and was dropped");
    }
  }

  // The output camera space is linear ProPhoto RGB.
  ColorCalibration calibration;
  calibration.illuminant = light_source::d50;
  calibration.color_matrix = xyz_d50_to_prophoto();
  calibration.forward_matrix = prophoto_to_xyz_d50();
  out.calibrations.push_back(calibration);
  out.as_shot_neutral = Vector3{1.0, 1.0, 1.0};

  out.baseline_exposure = in.baseline_exposure;
  out.baseline_noise = in.baseline_noise;
  out.baseline_sharpness = in.baseline_sharpness;
  out.linear_response_limit = 1.0;
  out.profile_name = std::string(output_profile_name);
  out.profile_embed_policy = 0; // Allow copying.

  // Stage 3 opcodes (lens distortion, vignetting) apply to linear data in the
  // same geometry, so they stay valid.
  out.opcode_list3 = in.opcode_list3;
  out.exif = in.exif;
  return out;
}

Result<NormalizedDng> render(Analysis analysis, const DngMetadata &input_meta,
                             const ProcessingOptions &options) {
  ProcessingReport report = std::move(analysis.report);
  auto camera = detail::camera_transform(input_meta, analysis.neutral);
  if (!camera) {
    return std::unexpected(camera.error());
  }
  report.reference_illuminant = camera->illuminant;
  report.used_forward_matrix = camera->used_forward_matrix;
  report.warnings.insert(report.warnings.end(), camera->warnings.begin(),
                         camera->warnings.end());
  auto transform = detail::output_transform(camera->balanced_to_xyz,
                                            report.mapping);
  if (!transform) {
    return std::unexpected(transform.error());
  }
  std::array<std::array<float, 3>, 3> m{};
  for (std::size_t row = 0; row < 3; ++row) {
    for (std::size_t col = 0; col < 3; ++col) {
      m[row][col] = static_cast<float>(transform->m[row][col]);
    }
  }

  LinearImage &linear = analysis.linear;
  const std::size_t width = linear.width;
  const std::array<float, 3> inverse_neutral{
      static_cast<float>(1.0 / analysis.neutral[0]),
      static_cast<float>(1.0 / analysis.neutral[1]),
      static_cast<float>(1.0 / analysis.neutral[2])};

  // White balance, then clip every channel at the level where the neutral
  // saturates so that clipped highlights stay neutral.
  detail::parallel_for(linear.height, options.threads, [&](std::size_t begin,
                                                           std::size_t end) {
    for (std::size_t row = begin; row < end; ++row) {
      for (std::size_t col = 0; col < width; ++col) {
        for (std::size_t c = 0; c < linear.channels; ++c) {
          const std::size_t color =
              linear.is_cfa()
                  ? static_cast<std::size_t>(linear.cfa_pattern.at(row, col))
                  : c;
          float &value = linear.data[(row * width + col) * linear.channels + c];
          value = std::min(value * inverse_neutral[color], 1.0F);
        }
      }
    }
  });

  RawImage output;
  output.width = linear.width;
  output.height = linear.height;
  output.samples_per_pixel = 3;
  output.samples.resize(output.sample_count());
  detail::parallel_for(linear.height, options.threads, [&](std::size_t begin,
                                                           std::size_t end) {
    std::vector<float> rgb(width * 3);
    for (std::size_t row = begin; row < end; ++row) {
      if (linear.is_cfa()) {
        detail::demosaic_row(linear, row, rgb);
      } else {
        std::copy_n(linear.data.begin() +
                        static_cast<std::ptrdiff_t>(row * width * 3),
                    width * 3, rgb.begin());
      }
      std::uint16_t *target = output.samples.data() + row * width * 3;
      for (std::size_t col = 0; col < width; ++col) {
        const float *in = rgb.data() + col * 3;
        std::array<float, 3> color{};
        for (std::size_t c = 0; c < 3; ++c) {
          color[c] = m[c][0] * in[0] + m[c][1] * in[1] + m[c][2] * in[2];
        }
        detail::gamut_map(color);
        for (std::size_t c = 0; c < 3; ++c) {
          target[col * 3 + c] =
              static_cast<std::uint16_t>(std::lround(color[c] * 65535.0F));
        }
      }
    }
  });
  linear.data = {};

  NormalizedDng result;
  result.dng.metadata = output_metadata(input_meta, output, report);
  if (options.embed_thumbnail) {
    result.thumbnail = render_preview(output, thumbnail_size,
                                      result.dng.metadata.baseline_exposure);
  }
  result.dng.image = std::move(output);
  result.report = std::move(report);
  return result;
}

template <typename Function>
auto guarded(Function &&function) -> decltype(function()) {
  try {
    return function();
  } catch (const std::bad_alloc &) {
    return make_error(ErrorCode::out_of_memory,
                      "not enough memory to process the image");
  } catch (const std::exception &exception) {
    return make_error(ErrorCode::processing_failed, exception.what());
  }
}

} // namespace

ChannelMapping resolve_mapping(ChannelMapping mapping, FilterType filter) {
  if (mapping != ChannelMapping::auto_select) {
    return mapping;
  }
  switch (filter) {
  case FilterType::nm850:
    return ChannelMapping::monochrome;
  case FilterType::full_spectrum:
    return ChannelMapping::native;
  case FilterType::auto_detect:
  case FilterType::nm590:
  case FilterType::nm665:
  case FilterType::nm720:
    return ChannelMapping::false_color;
  }
  return ChannelMapping::native;
}

Result<ProcessingReport> analyze(const DngImage &input,
                                 const ProcessingOptions &options) {
  return guarded([&]() -> Result<ProcessingReport> {
    auto analysis = analyze_image(input, options);
    if (!analysis) {
      return std::unexpected(analysis.error());
    }
    auto camera = detail::camera_transform(input.metadata, analysis->neutral);
    if (!camera) {
      return std::unexpected(camera.error());
    }
    ProcessingReport report = std::move(analysis->report);
    report.reference_illuminant = camera->illuminant;
    report.used_forward_matrix = camera->used_forward_matrix;
    report.warnings.insert(report.warnings.end(), camera->warnings.begin(),
                           camera->warnings.end());
    return report;
  });
}

Result<NormalizedDng> normalize(const DngImage &input,
                                const ProcessingOptions &options) {
  return guarded([&]() -> Result<NormalizedDng> {
    auto analysis = analyze_image(input, options);
    if (!analysis) {
      return std::unexpected(analysis.error());
    }
    return render(std::move(*analysis), input.metadata, options);
  });
}

Result<ProcessingReport> analyze_file(const std::filesystem::path &input,
                                      const ProcessingOptions &options) {
  auto dng = read_dng(input);
  if (!dng) {
    return std::unexpected(dng.error());
  }
  return analyze(*dng, options);
}

Result<ProcessingReport> process_file(const std::filesystem::path &input,
                                      const std::filesystem::path &output,
                                      const ProcessingOptions &options) {
  return guarded([&]() -> Result<ProcessingReport> {
    std::error_code error;
    bool same_file = std::filesystem::exists(output, error) &&
                     std::filesystem::equivalent(input, output, error);
    std::error_code input_error;
    std::error_code output_error;
    const auto input_path = std::filesystem::weakly_canonical(input, input_error);
    const auto output_path =
        std::filesystem::weakly_canonical(output, output_error);
    if (!input_error && !output_error && input_path == output_path) {
      same_file = true;
    }
    if (same_file) {
      return make_error(ErrorCode::invalid_argument,
                        std::format("refusing to overwrite the input file {}",
                                    input.string()));
    }

    auto dng = read_dng(input);
    if (!dng) {
      return std::unexpected(dng.error());
    }
    auto analysis = analyze_image(*dng, options);
    if (!analysis) {
      return std::unexpected(analysis.error());
    }
    // The stored samples are no longer needed; release them before rendering.
    dng->image.samples = {};
    auto normalized = render(std::move(*analysis), dng->metadata, options);
    if (!normalized) {
      return std::unexpected(normalized.error());
    }

    DngWriteOptions write_options;
    write_options.compression = options.compression;
    write_options.thumbnail = std::move(normalized->thumbnail);
    if (auto written = write_dng(output, normalized->dng, write_options);
        !written) {
      return std::unexpected(written.error());
    }
    return std::move(normalized->report);
  });
}

} // namespace ircore
