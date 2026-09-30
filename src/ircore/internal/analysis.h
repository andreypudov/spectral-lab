#pragma once

#include "ircore/analysis.h"

#include <cstddef>
#include <vector>

namespace ircore {
namespace detail {

// Samples darker than this (in linear units) are dominated by noise.
inline constexpr float noise_floor = 0.002F;
inline constexpr std::size_t min_usable_samples = 16;

// Unclipped samples above the noise floor and the darkest 5 percent.
[[nodiscard]] std::vector<ChannelSample>
usable_samples(const SceneStatistics &stats);

// Initial thresholds of the filter classifier, fitted to only two real
// samples (590 nm: spread 0.33; 720 nm: spread 0.17). See
// docs/processing-pipeline.org for the rationale and the calibration plan.
namespace filter_thresholds {
// Near-monochrome scenes with similar channel responses: 850 nm and longer.
inline constexpr double balance_850 = 0.60;
inline constexpr double spread_850 = 0.05;
// Longer-wavelength filters pass less visible light, so less color remains.
inline constexpr double spread_720 = 0.20;
inline constexpr double spread_665 = 0.28;
// Relative distance (natural log) from a threshold for high confidence.
inline constexpr double confident_margin = 0.15;
} // namespace filter_thresholds

} // namespace detail
} // namespace ircore
