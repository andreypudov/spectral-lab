#pragma once

#include "ircore/color.h"
#include "ircore/dng.h"
#include "ircore/error.h"
#include "ircore/image.h"
#include "ircore/options.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ircore {

// Linearized raw data: 0 is the black level and 1 the white level of every
// channel. Values may be slightly negative (noise below black).
struct LinearImage {
  std::uint32_t width{};
  std::uint32_t height{};
  // 1 for a CFA mosaic, 3 for camera RGB.
  std::uint32_t channels{1};
  CfaPattern cfa_pattern;
  std::vector<float> data;

  [[nodiscard]] bool is_cfa() const { return channels == 1; }
};

// Applies the linearization table, black level (including the per-row and
// per-column deltas) and white level.
[[nodiscard]] Result<LinearImage> linearize(const DngImage &dng,
                                            unsigned threads = 0);

// Camera RGB sample of one 2x2 block (or pixel of a camera RGB image).
struct ChannelSample {
  float red{};
  float green{};
  float blue{};
};

struct SceneStatistics {
  // Unclipped samples.
  std::vector<ChannelSample> samples;
  // Number of inspected blocks, including the clipped ones.
  std::size_t inspected{};
  std::size_t clipped{};
};

// Linear values at or above this level are treated as clipped.
inline constexpr float clip_threshold = 0.98F;

// Collects camera RGB samples, one per 2x2 block, optionally restricted to a
// region. Large images are subsampled on a regular grid to at most
// `max_samples` inspected blocks, which keeps the result deterministic.
[[nodiscard]] Result<SceneStatistics>
collect_statistics(const LinearImage &image,
                   std::optional<PixelRect> region = std::nullopt,
                   std::size_t max_samples = std::size_t{1} << 20);

// Gray-world neutral: the ratio of channel sums over unclipped samples that
// are above the noise floor, normalized so that the largest component is 1.
[[nodiscard]] Result<Vector3> estimate_neutral(const SceneStatistics &stats);

enum class DetectionConfidence { low, high };

struct FilterDetection {
  FilterType filter{FilterType::nm720};
  DetectionConfidence confidence{DetectionConfidence::low};
  // min(neutral) / max(neutral) of the gray-world neutral: how similar the
  // raw channel responses are.
  double channel_balance{};
  // Robust spread of the white balanced log-chromaticity: how much color the
  // scene has once the neutral is removed.
  double chroma_spread{};
};

// Heuristic, uncalibrated filter classification. Never returns
// FilterType::auto_detect or FilterType::full_spectrum.
[[nodiscard]] FilterDetection detect_filter(const SceneStatistics &stats,
                                            const Vector3 &neutral);

[[nodiscard]] std::string_view to_string(DetectionConfidence confidence);

} // namespace ircore
