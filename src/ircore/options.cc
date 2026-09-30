#include "ircore/options.h"

namespace ircore {

std::string_view to_string(WhiteBalanceMode mode) {
  switch (mode) {
  case WhiteBalanceMode::auto_ir:
    return "auto";
  case WhiteBalanceMode::as_shot:
    return "as-shot";
  case WhiteBalanceMode::reference_area:
    return "area";
  case WhiteBalanceMode::explicit_neutral:
    return "neutral";
  }
  return "unknown";
}

std::string_view to_string(FilterType filter) {
  switch (filter) {
  case FilterType::auto_detect:
    return "auto";
  case FilterType::nm590:
    return "590";
  case FilterType::nm665:
    return "665";
  case FilterType::nm720:
    return "720";
  case FilterType::nm850:
    return "850";
  case FilterType::full_spectrum:
    return "full-spectrum";
  }
  return "unknown";
}

std::string_view to_string(ChannelMapping mapping) {
  switch (mapping) {
  case ChannelMapping::auto_select:
    return "auto";
  case ChannelMapping::native:
    return "native";
  case ChannelMapping::false_color:
    return "false-color";
  case ChannelMapping::monochrome:
    return "monochrome";
  }
  return "unknown";
}

std::string_view to_string(DngCompression compression) {
  return compression == DngCompression::lossless_jpeg ? "lossless-jpeg"
                                                       : "none";
}

std::optional<FilterType> parse_filter_type(std::string_view text) {
  for (const FilterType filter :
       {FilterType::auto_detect, FilterType::nm590, FilterType::nm665,
        FilterType::nm720, FilterType::nm850, FilterType::full_spectrum}) {
    if (text == to_string(filter)) {
      return filter;
    }
  }
  if (text == "590nm" || text == "665nm" || text == "720nm" ||
      text == "850nm") {
    return parse_filter_type(text.substr(0, 3));
  }
  if (text == "full") {
    return FilterType::full_spectrum;
  }
  return std::nullopt;
}

std::optional<ChannelMapping> parse_channel_mapping(std::string_view text) {
  for (const ChannelMapping mapping :
       {ChannelMapping::auto_select, ChannelMapping::native,
        ChannelMapping::false_color, ChannelMapping::monochrome}) {
    if (text == to_string(mapping)) {
      return mapping;
    }
  }
  if (text == "mono") {
    return ChannelMapping::monochrome;
  }
  return std::nullopt;
}

std::optional<DngCompression> parse_compression(std::string_view text) {
  if (text == "lossless-jpeg" || text == "ljpeg") {
    return DngCompression::lossless_jpeg;
  }
  if (text == "none") {
    return DngCompression::uncompressed;
  }
  return std::nullopt;
}

} // namespace ircore
