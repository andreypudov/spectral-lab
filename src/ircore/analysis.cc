#include "ircore/analysis.h"
#include "ircore/internal/analysis.h"
#include "ircore/internal/parallel.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace ircore {

namespace {

float brightness(const ChannelSample &sample) {
  return std::max({sample.red, sample.green, sample.blue});
}

// Value at quantile q (0..1) of `values`; reorders the vector.
double quantile(std::vector<double> &values, double q) {
  const auto index = static_cast<std::ptrdiff_t>(
      std::floor(q * static_cast<double>(values.size() - 1)));
  std::nth_element(values.begin(), values.begin() + index, values.end());
  return values[static_cast<std::size_t>(index)];
}

} // namespace

namespace detail {

std::vector<ChannelSample> usable_samples(const SceneStatistics &stats) {
  std::vector<double> levels;
  levels.reserve(stats.samples.size());
  for (const auto &sample : stats.samples) {
    levels.push_back(brightness(sample));
  }
  double floor = noise_floor;
  if (!levels.empty()) {
    floor = std::max(floor, quantile(levels, 0.05));
  }
  std::vector<ChannelSample> usable;
  usable.reserve(stats.samples.size());
  for (const auto &sample : stats.samples) {
    if (brightness(sample) >= floor) {
      usable.push_back(sample);
    }
  }
  if (usable.size() < min_usable_samples) {
    // Very dark frames: use everything with some signal.
    usable.clear();
    for (const auto &sample : stats.samples) {
      if (brightness(sample) > 0.0F) {
        usable.push_back(sample);
      }
    }
  }
  return usable;
}

} // namespace detail

Result<LinearImage> linearize(const DngImage &dng, unsigned threads) {
  const DngMetadata &meta = dng.metadata;
  const RawImage &image = dng.image;
  const std::size_t spp = image.samples_per_pixel;
  const std::size_t expected_spp = meta.photometric == Photometric::cfa ? 1 : 3;
  if (spp != expected_spp || image.width == 0 || image.height == 0 ||
      image.samples.size() != image.sample_count()) {
    return make_error(ErrorCode::invalid_argument,
                      "raw image geometry is inconsistent");
  }
  const std::size_t repeat_rows = meta.black_level_repeat_rows;
  const std::size_t repeat_cols = meta.black_level_repeat_cols;
  if (repeat_rows == 0 || repeat_cols == 0 ||
      (!meta.black_level.empty() &&
       meta.black_level.size() != repeat_rows * repeat_cols * spp) ||
      (!meta.black_level_delta_h.empty() &&
       meta.black_level_delta_h.size() != image.width) ||
      (!meta.black_level_delta_v.empty() &&
       meta.black_level_delta_v.size() != image.height)) {
    return make_error(ErrorCode::invalid_format,
                      "black level tags do not match the image");
  }

  std::vector<double> black_pattern(repeat_rows * repeat_cols * spp, 0.0);
  if (!meta.black_level.empty()) {
    black_pattern = meta.black_level;
  }
  std::vector<double> white(spp);
  for (std::size_t s = 0; s < spp; ++s) {
    white[s] =
        meta.white_level.empty()
            ? std::ldexp(1.0, std::clamp<int>(meta.bits_per_sample, 1, 16)) - 1.0
            : meta.white_level[std::min(s, meta.white_level.size() - 1)];
  }
  const auto max_delta = [](const std::vector<double> &deltas) {
    return deltas.empty() ? 0.0 : *std::ranges::max_element(deltas);
  };
  const double max_black = *std::ranges::max_element(black_pattern) +
                           max_delta(meta.black_level_delta_h) +
                           max_delta(meta.black_level_delta_v);
  if (*std::ranges::min_element(white) <= max_black) {
    return make_error(ErrorCode::invalid_format,
                      "white level is not above the black level");
  }

  const std::vector<std::uint16_t> &table = meta.linearization_table;
  LinearImage result;
  result.width = image.width;
  result.height = image.height;
  result.channels = static_cast<std::uint32_t>(spp);
  result.cfa_pattern = meta.cfa_pattern;
  result.data.resize(image.sample_count());

  detail::parallel_for(image.height, threads, [&](std::size_t begin,
                                                  std::size_t end) {
    for (std::size_t row = begin; row < end; ++row) {
      const double delta_v = meta.black_level_delta_v.empty()
                                 ? 0.0
                                 : meta.black_level_delta_v[row];
      const std::size_t pattern_row = (row % repeat_rows) * repeat_cols * spp;
      for (std::size_t col = 0; col < image.width; ++col) {
        const double delta_h = meta.black_level_delta_h.empty()
                                   ? 0.0
                                   : meta.black_level_delta_h[col];
        const std::size_t pattern = pattern_row + (col % repeat_cols) * spp;
        for (std::size_t s = 0; s < spp; ++s) {
          const std::size_t index = (row * image.width + col) * spp + s;
          double value = image.samples[index];
          if (!table.empty()) {
            value = table[std::min<std::size_t>(image.samples[index],
                                                table.size() - 1)];
          }
          const double black = black_pattern[pattern + s] + delta_h + delta_v;
          result.data[index] =
              static_cast<float>((value - black) / (white[s] - black));
        }
      }
    }
  });
  return result;
}

Result<SceneStatistics> collect_statistics(const LinearImage &image,
                                           std::optional<PixelRect> region,
                                           std::size_t max_samples) {
  PixelRect rect{0, 0, image.width, image.height};
  if (region) {
    rect = *region;
    if (rect.width == 0 || rect.height == 0 || rect.x >= image.width ||
        rect.y >= image.height || image.width - rect.x < rect.width ||
        image.height - rect.y < rect.height) {
      return make_error(
          ErrorCode::invalid_argument,
          std::format("reference area {}x{}+{}+{} is outside of the {}x{} "
                      "image",
                      rect.width, rect.height, rect.x, rect.y, image.width,
                      image.height));
    }
  }
  const std::size_t across = rect.width / 2;
  const std::size_t down = rect.height / 2;
  if (across == 0 || down == 0) {
    return make_error(ErrorCode::invalid_argument,
                      "reference area must be at least 2x2 pixels");
  }
  std::size_t step = 1;
  while (max_samples != 0 && (across / step + 1) * (down / step + 1) > max_samples &&
         step < across + down) {
    ++step;
  }

  SceneStatistics stats;
  const std::size_t channels = image.channels;
  const auto value = [&](std::size_t row, std::size_t col, std::size_t c) {
    return image.data[(row * image.width + col) * channels + c];
  };
  for (std::size_t by = 0; by < down; by += step) {
    for (std::size_t bx = 0; bx < across; bx += step) {
      const std::size_t row0 = rect.y + by * 2;
      const std::size_t col0 = rect.x + bx * 2;
      std::array<float, 3> sum{};
      std::array<int, 3> count{};
      bool clipped = false;
      for (std::size_t dy = 0; dy < 2; ++dy) {
        for (std::size_t dx = 0; dx < 2; ++dx) {
          const std::size_t row = row0 + dy;
          const std::size_t col = col0 + dx;
          if (image.is_cfa()) {
            const float v = value(row, col, 0);
            const auto color =
                static_cast<std::size_t>(image.cfa_pattern.at(row, col));
            sum[color] += v;
            ++count[color];
            clipped = clipped || v >= clip_threshold;
          } else {
            for (std::size_t c = 0; c < 3; ++c) {
              const float v = value(row, col, c);
              sum[c] += v;
              ++count[c];
              clipped = clipped || v >= clip_threshold;
            }
          }
        }
      }
      ++stats.inspected;
      if (clipped) {
        ++stats.clipped;
        continue;
      }
      stats.samples.push_back({sum[0] / static_cast<float>(count[0]),
                               sum[1] / static_cast<float>(count[1]),
                               sum[2] / static_cast<float>(count[2])});
    }
  }
  return stats;
}

Result<Vector3> estimate_neutral(const SceneStatistics &stats) {
  const auto usable = detail::usable_samples(stats);
  if (usable.empty()) {
    return make_error(ErrorCode::processing_failed,
                      "no usable (unclipped, exposed) pixels to estimate the "
                      "white balance");
  }
  Vector3 sum{};
  for (const auto &sample : usable) {
    sum[0] += sample.red;
    sum[1] += sample.green;
    sum[2] += sample.blue;
  }
  const double largest = max_component(sum);
  if (!(largest > 0.0) || !(min_component(sum) > 0.0)) {
    return make_error(ErrorCode::processing_failed,
                      "a color channel has no signal; cannot estimate the "
                      "white balance");
  }
  return Vector3{sum[0] / largest, sum[1] / largest, sum[2] / largest};
}

FilterDetection detect_filter(const SceneStatistics &stats,
                              const Vector3 &neutral) {
  FilterDetection result;
  const double largest = max_component(neutral);
  result.channel_balance =
      largest > 0.0 ? min_component(neutral) / largest : 0.0;

  const auto usable = detail::usable_samples(stats);
  std::vector<double> red_green;
  std::vector<double> blue_green;
  for (const auto &sample : usable) {
    if (sample.red <= 0.0F || sample.green <= 0.0F || sample.blue <= 0.0F) {
      continue;
    }
    const double r = sample.red / neutral[0];
    const double g = sample.green / neutral[1];
    const double b = sample.blue / neutral[2];
    red_green.push_back(std::log(r / g));
    blue_green.push_back(std::log(b / g));
  }
  bool enough = red_green.size() >= detail::min_usable_samples;
  if (enough) {
    // p90 - p10 of a normal distribution is 2.563 standard deviations.
    const auto spread = [](std::vector<double> &values) {
      const double low = quantile(values, 0.10);
      const double high = quantile(values, 0.90);
      return (high - low) / 2.563;
    };
    const double sx = spread(red_green);
    const double sy = spread(blue_green);
    result.chroma_spread = std::sqrt(sx * sx + sy * sy);
  }

  using namespace detail::filter_thresholds;
  const double balance = result.channel_balance;
  const double spread = result.chroma_spread;
  if (balance >= balance_850 && spread <= spread_850) {
    result.filter = FilterType::nm850;
  } else if (spread <= spread_720) {
    result.filter = FilterType::nm720;
  } else if (spread <= spread_665) {
    result.filter = FilterType::nm665;
  } else {
    result.filter = FilterType::nm590;
  }

  const auto far = [](double value, double threshold) {
    return std::abs(std::log(std::max(value, 1e-9) / threshold)) >
           confident_margin;
  };
  const bool spread_clear = far(spread, spread_850) && far(spread, spread_720) &&
                            far(spread, spread_665);
  // Near-monochrome scenes are only 850 nm if the channels also agree.
  const bool balance_clear = spread > spread_850 || far(balance, balance_850);
  result.confidence = enough && balance_clear && spread_clear
                          ? DetectionConfidence::high
                          : DetectionConfidence::low;
  return result;
}

std::string_view to_string(DetectionConfidence confidence) {
  return confidence == DetectionConfidence::high ? "high" : "low";
}

} // namespace ircore
