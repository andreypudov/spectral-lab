#include "ircore/internal/tiff.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <set>

namespace ircore {
namespace detail {

namespace {

// Limits that keep malformed files from causing excessive work.
constexpr std::size_t max_ifd_count = 64;
constexpr std::size_t max_entries_per_ifd = 4096;
constexpr int max_sub_ifd_depth = 3;

std::uint64_t read_unsigned(std::span<const std::uint8_t> bytes,
                            std::size_t offset, std::size_t size,
                            bool big_endian) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < size; ++i) {
    const std::size_t index = big_endian ? offset + i : offset + size - 1 - i;
    value = (value << 8U) | bytes[index];
  }
  return value;
}

class Parser {
public:
  Parser(std::span<const std::uint8_t> bytes, bool big_endian)
      : bytes_(bytes), big_endian_(big_endian) {}

  Result<void> parse_chain(std::uint64_t offset, TiffFile &file) {
    while (offset != 0) {
      auto next = parse_ifd(offset, 0, false, file);
      if (!next) {
        return std::unexpected(next.error());
      }
      offset = *next;
    }
    return {};
  }

private:
  std::uint64_t unsigned_at(std::size_t offset, std::size_t size) const {
    return read_unsigned(bytes_, offset, size, big_endian_);
  }

  // Parses one IFD and the IFDs it references; returns the next IFD offset.
  Result<std::uint64_t> parse_ifd(std::uint64_t offset, int depth,
                                  bool is_exif, TiffFile &file) {
    if (file.ifds.size() >= max_ifd_count) {
      return make_error(ErrorCode::invalid_format, "too many TIFF IFDs");
    }
    if (!visited_.insert(offset).second) {
      return make_error(ErrorCode::invalid_format, "TIFF IFD loop detected");
    }
    if (offset > bytes_.size() || bytes_.size() - offset < 2) {
      return make_error(ErrorCode::invalid_format,
                        "TIFF IFD offset is outside of the file");
    }
    const auto start = static_cast<std::size_t>(offset);
    const auto count = static_cast<std::size_t>(unsigned_at(start, 2));
    if (count > max_entries_per_ifd) {
      return make_error(ErrorCode::invalid_format,
                        "TIFF IFD has too many entries");
    }
    const std::size_t table_size = 2 + count * 12 + 4;
    if (bytes_.size() - start < table_size) {
      return make_error(ErrorCode::invalid_format, "TIFF IFD is truncated");
    }

    TiffIfd ifd;
    ifd.is_exif = is_exif;
    ifd.entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      const std::size_t at = start + 2 + i * 12;
      TiffEntry entry;
      entry.tag = static_cast<std::uint16_t>(unsigned_at(at, 2));
      entry.type = static_cast<std::uint16_t>(unsigned_at(at + 2, 2));
      entry.count = static_cast<std::uint32_t>(unsigned_at(at + 4, 4));
      entry.big_endian = big_endian_;
      const std::size_t type_size = tiff_type_size(entry.type);
      if (type_size == 0) {
        continue; // Unknown types must be ignored by readers.
      }
      const std::uint64_t data_size =
          static_cast<std::uint64_t>(entry.count) * type_size;
      if (data_size <= 4) {
        entry.data = bytes_.subspan(at + 8, static_cast<std::size_t>(data_size));
      } else {
        const std::uint64_t data_offset = unsigned_at(at + 8, 4);
        if (data_offset > bytes_.size() ||
            bytes_.size() - data_offset < data_size) {
          // Skip entries pointing outside the file instead of rejecting the
          // whole file; required entries are validated by their users.
          continue;
        }
        entry.data = bytes_.subspan(static_cast<std::size_t>(data_offset),
                                    static_cast<std::size_t>(data_size));
      }
      ifd.entries.push_back(entry);
    }
    std::ranges::stable_sort(ifd.entries, {}, &TiffEntry::tag);
    const std::uint64_t next = unsigned_at(start + 2 + count * 12, 4);

    const std::size_t index = file.ifds.size();
    file.ifds.push_back(std::move(ifd));

    if (!is_exif && depth < max_sub_ifd_depth) {
      if (const TiffEntry *sub = file.ifds[index].find(330)) {
        const TiffEntry sub_ifds = *sub;
        for (std::size_t i = 0; i < sub_ifds.count; ++i) {
          const auto sub_offset = sub_ifds.integer(i);
          if (!sub_offset || *sub_offset <= 0) {
            continue;
          }
          auto chained = parse_ifd(static_cast<std::uint64_t>(*sub_offset),
                                   depth + 1, false, file);
          if (!chained) {
            return std::unexpected(chained.error());
          }
        }
      }
      if (const TiffEntry *exif = file.ifds[index].find(34665)) {
        const auto exif_offset = exif->integer(0);
        if (exif_offset && *exif_offset > 0) {
          const std::size_t exif_index = file.ifds.size();
          // A broken EXIF IFD must not make the image unreadable.
          if (parse_ifd(static_cast<std::uint64_t>(*exif_offset), depth + 1,
                        true, file)) {
            file.ifds[index].exif_ifd = exif_index;
          }
        }
      }
    }
    return next;
  }

  std::span<const std::uint8_t> bytes_;
  bool big_endian_;
  std::set<std::uint64_t> visited_;
};

void append_le(std::vector<std::uint8_t> &out, std::uint64_t value,
               std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
  }
}

} // namespace

std::size_t tiff_type_size(std::uint16_t type) {
  switch (type) {
  case tiff_type::byte:
  case tiff_type::ascii:
  case tiff_type::sbyte:
  case tiff_type::undefined:
    return 1;
  case tiff_type::short_:
  case tiff_type::sshort:
    return 2;
  case tiff_type::long_:
  case tiff_type::slong:
  case tiff_type::float_:
  case tiff_type::ifd:
    return 4;
  case tiff_type::rational:
  case tiff_type::srational:
  case tiff_type::double_:
    return 8;
  default:
    return 0;
  }
}

std::optional<std::int64_t> TiffEntry::integer(std::size_t index) const {
  if (index >= count) {
    return std::nullopt;
  }
  switch (type) {
  case tiff_type::byte:
  case tiff_type::undefined:
    return data[index];
  case tiff_type::sbyte:
    return static_cast<std::int8_t>(data[index]);
  case tiff_type::short_:
    return static_cast<std::int64_t>(
        read_unsigned(data, index * 2, 2, big_endian));
  case tiff_type::sshort:
    return static_cast<std::int16_t>(
        read_unsigned(data, index * 2, 2, big_endian));
  case tiff_type::long_:
  case tiff_type::ifd:
    return static_cast<std::int64_t>(
        read_unsigned(data, index * 4, 4, big_endian));
  case tiff_type::slong:
    return static_cast<std::int32_t>(
        read_unsigned(data, index * 4, 4, big_endian));
  default:
    return std::nullopt;
  }
}

std::optional<double> TiffEntry::number(std::size_t index) const {
  if (index >= count) {
    return std::nullopt;
  }
  std::optional<double> result;
  switch (type) {
  case tiff_type::rational: {
    const auto num = read_unsigned(data, index * 8, 4, big_endian);
    const auto den = read_unsigned(data, index * 8 + 4, 4, big_endian);
    if (den != 0) {
      result = static_cast<double>(num) / static_cast<double>(den);
    }
    break;
  }
  case tiff_type::srational: {
    const auto num = static_cast<std::int32_t>(
        read_unsigned(data, index * 8, 4, big_endian));
    const auto den = static_cast<std::int32_t>(
        read_unsigned(data, index * 8 + 4, 4, big_endian));
    if (den != 0) {
      result = static_cast<double>(num) / static_cast<double>(den);
    }
    break;
  }
  case tiff_type::float_:
    result = static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(
        read_unsigned(data, index * 4, 4, big_endian))));
    break;
  case tiff_type::double_:
    result = std::bit_cast<double>(read_unsigned(data, index * 8, 8, big_endian));
    break;
  default:
    if (const auto value = integer(index)) {
      result = static_cast<double>(*value);
    }
    break;
  }
  if (result && !std::isfinite(*result)) {
    return std::nullopt;
  }
  return result;
}

std::optional<std::vector<double>> TiffEntry::numbers() const {
  std::vector<double> values;
  values.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    const auto value = number(i);
    if (!value) {
      return std::nullopt;
    }
    values.push_back(*value);
  }
  return values;
}

std::string TiffEntry::text() const {
  if (type != tiff_type::ascii && type != tiff_type::byte &&
      type != tiff_type::undefined) {
    return {};
  }
  std::string result;
  for (const std::uint8_t byte : data) {
    if (byte == 0) {
      break;
    }
    result.push_back(static_cast<char>(byte));
  }
  while (!result.empty() && result.back() == ' ') {
    result.pop_back();
  }
  return result;
}

std::vector<std::uint8_t> TiffEntry::little_endian_bytes() const {
  std::vector<std::uint8_t> result(data.begin(), data.end());
  if (!big_endian) {
    return result;
  }
  std::size_t element = tiff_type_size(type);
  if (type == tiff_type::rational || type == tiff_type::srational) {
    element = 4; // Numerator and denominator are swapped separately.
  }
  if (element > 1) {
    for (std::size_t at = 0; at + element <= result.size(); at += element) {
      std::reverse(result.begin() + static_cast<std::ptrdiff_t>(at),
                   result.begin() + static_cast<std::ptrdiff_t>(at + element));
    }
  }
  return result;
}

const TiffEntry *TiffIfd::find(std::uint16_t tag) const {
  const auto it = std::ranges::lower_bound(entries, tag, {}, &TiffEntry::tag);
  if (it == entries.end() || it->tag != tag) {
    return nullptr;
  }
  return &*it;
}

Result<TiffFile> parse_tiff(std::span<const std::uint8_t> bytes) {
  if (bytes.size() < 8) {
    return make_error(ErrorCode::invalid_format, "file is too small for TIFF");
  }
  TiffFile file;
  if (bytes[0] == 'I' && bytes[1] == 'I') {
    file.big_endian = false;
  } else if (bytes[0] == 'M' && bytes[1] == 'M') {
    file.big_endian = true;
  } else {
    return make_error(ErrorCode::invalid_format, "missing TIFF byte order mark");
  }
  const auto magic = read_unsigned(bytes, 2, 2, file.big_endian);
  if (magic == 43) {
    return make_error(ErrorCode::unsupported_format,
                      "BigTIFF files are not supported");
  }
  if (magic != 42) {
    return make_error(ErrorCode::invalid_format, "invalid TIFF magic number");
  }
  const auto first = read_unsigned(bytes, 4, 4, file.big_endian);
  if (first == 0) {
    return make_error(ErrorCode::invalid_format, "TIFF file has no IFD");
  }
  Parser parser(bytes, file.big_endian);
  if (auto parsed = parser.parse_chain(first, file); !parsed) {
    return std::unexpected(parsed.error());
  }
  return file;
}

// ---------------------------------------------------------------------------

std::pair<std::int64_t, std::int64_t> to_rational(double value,
                                                  std::int64_t denominator) {
  if (!std::isfinite(value)) {
    return {0, 1};
  }
  const double rounded = std::round(value);
  if (std::abs(value - rounded) < 1e-9 &&
      std::abs(rounded) <= std::numeric_limits<std::int32_t>::max()) {
    return {static_cast<std::int64_t>(rounded), 1};
  }
  while (denominator > 1 &&
         std::abs(value * static_cast<double>(denominator)) >
             std::numeric_limits<std::int32_t>::max()) {
    denominator /= 10;
  }
  const double clamped = std::clamp(
      std::round(value * static_cast<double>(denominator)),
      static_cast<double>(std::numeric_limits<std::int32_t>::min()),
      static_cast<double>(std::numeric_limits<std::int32_t>::max()));
  return {static_cast<std::int64_t>(clamped), denominator};
}

void IfdBuilder::add_entry(Entry entry) {
  const auto it = std::ranges::find(entries_, entry.tag, &Entry::tag);
  if (it != entries_.end()) {
    *it = std::move(entry);
  } else {
    entries_.push_back(std::move(entry));
  }
}

void IfdBuilder::add_raw(std::uint16_t tag, std::uint16_t type,
                         std::uint32_t count,
                         std::vector<std::uint8_t> little_endian_bytes) {
  add_entry({tag, type, count, Kind::literal, std::move(little_endian_bytes),
             {}});
}

void IfdBuilder::add_bytes(std::uint16_t tag, std::uint16_t type,
                           std::span<const std::uint8_t> bytes) {
  add_raw(tag, type, static_cast<std::uint32_t>(bytes.size()),
          {bytes.begin(), bytes.end()});
}

void IfdBuilder::add_ascii(std::uint16_t tag, const std::string &text) {
  std::vector<std::uint8_t> bytes(text.begin(), text.end());
  bytes.push_back(0);
  const auto count = static_cast<std::uint32_t>(bytes.size());
  add_raw(tag, tiff_type::ascii, count, std::move(bytes));
}

void IfdBuilder::add_short(std::uint16_t tag,
                           std::span<const std::uint16_t> values) {
  std::vector<std::uint8_t> bytes;
  for (const auto value : values) {
    append_le(bytes, value, 2);
  }
  add_raw(tag, tiff_type::short_, static_cast<std::uint32_t>(values.size()),
          std::move(bytes));
}

void IfdBuilder::add_short(std::uint16_t tag, std::uint16_t value) {
  add_short(tag, std::span<const std::uint16_t>(&value, 1));
}

void IfdBuilder::add_long(std::uint16_t tag,
                          std::span<const std::uint32_t> values) {
  std::vector<std::uint8_t> bytes;
  for (const auto value : values) {
    append_le(bytes, value, 4);
  }
  add_raw(tag, tiff_type::long_, static_cast<std::uint32_t>(values.size()),
          std::move(bytes));
}

void IfdBuilder::add_long(std::uint16_t tag, std::uint32_t value) {
  add_long(tag, std::span<const std::uint32_t>(&value, 1));
}

void IfdBuilder::add_rational(std::uint16_t tag, std::span<const double> values) {
  std::vector<std::uint8_t> bytes;
  for (const double value : values) {
    const auto [num, den] = to_rational(std::max(0.0, value), 10000);
    append_le(bytes, static_cast<std::uint64_t>(num), 4);
    append_le(bytes, static_cast<std::uint64_t>(den), 4);
  }
  add_raw(tag, tiff_type::rational, static_cast<std::uint32_t>(values.size()),
          std::move(bytes));
}

void IfdBuilder::add_srational(std::uint16_t tag,
                               std::span<const double> values) {
  std::vector<std::uint8_t> bytes;
  for (const double value : values) {
    const auto [num, den] = to_rational(value, 10000);
    append_le(bytes, static_cast<std::uint32_t>(static_cast<std::int32_t>(num)),
              4);
    append_le(bytes, static_cast<std::uint64_t>(den), 4);
  }
  add_raw(tag, tiff_type::srational, static_cast<std::uint32_t>(values.size()),
          std::move(bytes));
}

void IfdBuilder::add_blob_offsets(std::uint16_t tag,
                                  std::vector<std::size_t> blobs) {
  const auto count = static_cast<std::uint32_t>(blobs.size());
  add_entry({tag, tiff_type::long_, count, Kind::blob_offsets, {},
             std::move(blobs)});
}

void IfdBuilder::add_ifd_offsets(std::uint16_t tag,
                                 std::vector<std::size_t> ifds) {
  const auto count = static_cast<std::uint32_t>(ifds.size());
  add_entry(
      {tag, tiff_type::long_, count, Kind::ifd_offsets, {}, std::move(ifds)});
}

std::size_t TiffWriter::add_ifd(IfdBuilder ifd) {
  ifds_.push_back(std::move(ifd));
  return ifds_.size() - 1;
}

std::size_t TiffWriter::add_blob(std::vector<std::uint8_t> bytes) {
  blobs_.push_back(std::move(bytes));
  return blobs_.size() - 1;
}

Result<std::vector<std::uint8_t>> TiffWriter::serialize() const {
  if (ifds_.empty()) {
    return make_error(ErrorCode::invalid_argument, "TIFF needs an IFD");
  }
  const auto word_align = [](std::uint64_t value) {
    return value + (value & 1U);
  };
  const auto entry_size = [](const IfdBuilder::Entry &entry) {
    return static_cast<std::uint64_t>(entry.count) * tiff_type_size(entry.type);
  };

  // Layout: header, then each IFD followed by its out-of-line values, then
  // the blobs. Every offset is word aligned as TIFF requires.
  std::vector<std::uint64_t> ifd_offsets(ifds_.size());
  std::uint64_t position = 8;
  for (std::size_t i = 0; i < ifds_.size(); ++i) {
    ifd_offsets[i] = position;
    position += 2 + ifds_[i].entries_.size() * 12 + 4;
    for (const auto &entry : ifds_[i].entries_) {
      const std::uint64_t size = entry_size(entry);
      if (size > 4) {
        position = word_align(position + size);
      }
    }
  }
  std::vector<std::uint64_t> blob_offsets(blobs_.size());
  for (std::size_t i = 0; i < blobs_.size(); ++i) {
    blob_offsets[i] = position;
    position = word_align(position + blobs_[i].size());
  }
  if (position > std::numeric_limits<std::uint32_t>::max()) {
    return make_error(ErrorCode::unsupported_format,
                      "output exceeds the 4 GiB TIFF limit");
  }

  std::vector<std::uint8_t> out;
  out.reserve(static_cast<std::size_t>(position));
  out.insert(out.end(), {'I', 'I', 42, 0});
  append_le(out, ifd_offsets[0], 4);

  for (std::size_t i = 0; i < ifds_.size(); ++i) {
    auto entries = ifds_[i].entries_;
    std::ranges::sort(entries, {}, &IfdBuilder::Entry::tag);

    // Resolve offset entries into literal values.
    for (auto &entry : entries) {
      if (entry.kind == IfdBuilder::Kind::literal) {
        continue;
      }
      const auto &offsets = entry.kind == IfdBuilder::Kind::blob_offsets
                                ? blob_offsets
                                : ifd_offsets;
      entry.bytes.clear();
      for (const std::size_t target : entry.targets) {
        if (target >= offsets.size()) {
          return make_error(ErrorCode::invalid_argument,
                            "TIFF offset entry references an unknown target");
        }
        append_le(entry.bytes, offsets[target], 4);
      }
    }

    std::uint64_t value_position =
        ifd_offsets[i] + 2 + entries.size() * 12 + 4;
    std::vector<std::uint8_t> values;
    append_le(out, entries.size(), 2);
    for (const auto &entry : entries) {
      const std::uint64_t size = entry_size(entry);
      if (entry.bytes.size() != size) {
        return make_error(ErrorCode::invalid_argument,
                          std::format("TIFF tag {} has {} bytes, expected {}",
                                      entry.tag, entry.bytes.size(), size));
      }
      append_le(out, entry.tag, 2);
      append_le(out, entry.type, 2);
      append_le(out, entry.count, 4);
      if (size <= 4) {
        out.insert(out.end(), entry.bytes.begin(), entry.bytes.end());
        out.insert(out.end(), 4 - static_cast<std::size_t>(size), 0);
      } else {
        append_le(out, value_position, 4);
        values.insert(values.end(), entry.bytes.begin(), entry.bytes.end());
        if (values.size() & 1U) {
          values.push_back(0);
        }
        value_position = word_align(value_position + size);
      }
    }
    append_le(out, 0, 4); // Separate IFDs are not chained.
    out.insert(out.end(), values.begin(), values.end());
  }
  for (std::size_t i = 0; i < blobs_.size(); ++i) {
    if (out.size() != blob_offsets[i]) {
      return make_error(ErrorCode::processing_failed, "TIFF layout mismatch");
    }
    out.insert(out.end(), blobs_[i].begin(), blobs_[i].end());
    if (out.size() & 1U) {
      out.push_back(0);
    }
  }
  return out;
}

} // namespace detail
} // namespace ircore
