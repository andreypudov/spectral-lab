#pragma once

#include "ircore/analysis.h"
#include "ircore/color.h"
#include "ircore/dng.h"
#include "ircore/error.h"
#include "ircore/image.h"
#include "ircore/options.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace ircore {

struct ProcessingReport {
  std::string camera;
  std::uint32_t width{};
  std::uint32_t height{};
  Photometric input_photometric{Photometric::cfa};

  WhiteBalanceMode white_balance_mode{WhiteBalanceMode::auto_ir};
  // Camera RGB neutral that was mapped to white, largest component 1.
  Vector3 camera_neutral{1.0, 1.0, 1.0};

  FilterType filter{FilterType::nm720};
  bool filter_detected{false};
  FilterDetection detection;

  ChannelMapping mapping{ChannelMapping::native};
  // Calibration illuminant whose camera matrices were used.
  std::uint16_t reference_illuminant{light_source::unknown};
  bool used_forward_matrix{false};

  // Share of inspected 2x2 blocks with a clipped channel.
  double clipped_fraction{};

  std::vector<std::string> warnings;
};

struct NormalizedDng {
  // Linear DNG ready to be written (16-bit linear ProPhoto RGB).
  DngImage dng;
  std::optional<Rgb8Image> thumbnail;
  ProcessingReport report;
};

// Runs white balance estimation and filter detection without rendering.
[[nodiscard]] Result<ProcessingReport>
analyze(const DngImage &input, const ProcessingOptions &options);

// Full normalization pipeline, in memory.
[[nodiscard]] Result<NormalizedDng> normalize(const DngImage &input,
                                              const ProcessingOptions &options);

[[nodiscard]] Result<ProcessingReport>
analyze_file(const std::filesystem::path &input,
             const ProcessingOptions &options);

// Reads `input`, normalizes it and writes the linear DNG to `output`.
// `output` must not refer to the same file as `input`.
[[nodiscard]] Result<ProcessingReport>
process_file(const std::filesystem::path &input,
             const std::filesystem::path &output,
             const ProcessingOptions &options);

// Mapping used for `mapping` when the filter type is `filter`.
[[nodiscard]] ChannelMapping resolve_mapping(ChannelMapping mapping,
                                             FilterType filter);

// Unique camera model written to output files. Editors use it to look up
// camera profiles, so it must never match a real camera.
inline constexpr std::string_view output_camera_model =
    "Spectral Lab Linear ProPhoto";
inline constexpr std::string_view output_profile_name = "Spectral Lab IR";

} // namespace ircore
