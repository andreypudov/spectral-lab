#pragma once

#include "ircore/color.h"
#include "ircore/dng.h"

#include <cstdint>
#include <optional>
#include <string_view>

namespace ircore {

enum class WhiteBalanceMode {
  // Robust gray-world estimate over the whole frame.
  auto_ir,
  // AsShotNeutral recorded by the camera (custom white balance in camera).
  as_shot,
  // Gray-world estimate over a user-selected reference area.
  reference_area,
  // Neutral given explicitly as camera channel ratios.
  explicit_neutral,
};

enum class FilterType {
  auto_detect,
  nm590,
  nm665,
  nm720,
  nm850,
  full_spectrum,
};

enum class ChannelMapping {
  // Resolved from the filter type.
  auto_select,
  native,
  false_color,
  monochrome,
};

// Rectangle in pixels of the processed (active area) image.
struct PixelRect {
  std::uint32_t x{};
  std::uint32_t y{};
  std::uint32_t width{};
  std::uint32_t height{};
};

struct WhiteBalanceSettings {
  WhiteBalanceMode mode{WhiteBalanceMode::auto_ir};
  // Used by WhiteBalanceMode::reference_area.
  PixelRect area;
  // Used by WhiteBalanceMode::explicit_neutral, camera RGB.
  Vector3 neutral{1.0, 1.0, 1.0};
};

struct ProcessingOptions {
  WhiteBalanceSettings white_balance;
  FilterType filter{FilterType::auto_detect};
  ChannelMapping mapping{ChannelMapping::auto_select};
  DngCompression compression{DngCompression::lossless_jpeg};
  bool embed_thumbnail{true};
  // Worker threads; 0 selects the hardware concurrency.
  unsigned threads{0};
};

[[nodiscard]] std::string_view to_string(WhiteBalanceMode mode);
[[nodiscard]] std::string_view to_string(FilterType filter);
[[nodiscard]] std::string_view to_string(ChannelMapping mapping);
[[nodiscard]] std::string_view to_string(DngCompression compression);

[[nodiscard]] std::optional<FilterType> parse_filter_type(std::string_view text);
[[nodiscard]] std::optional<ChannelMapping>
parse_channel_mapping(std::string_view text);
[[nodiscard]] std::optional<DngCompression>
parse_compression(std::string_view text);

} // namespace ircore
