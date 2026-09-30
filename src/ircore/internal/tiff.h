#pragma once

#include "ircore/error.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace ircore {
namespace detail {

namespace tiff_type {
inline constexpr std::uint16_t byte = 1;
inline constexpr std::uint16_t ascii = 2;
inline constexpr std::uint16_t short_ = 3;
inline constexpr std::uint16_t long_ = 4;
inline constexpr std::uint16_t rational = 5;
inline constexpr std::uint16_t sbyte = 6;
inline constexpr std::uint16_t undefined = 7;
inline constexpr std::uint16_t sshort = 8;
inline constexpr std::uint16_t slong = 9;
inline constexpr std::uint16_t srational = 10;
inline constexpr std::uint16_t float_ = 11;
inline constexpr std::uint16_t double_ = 12;
inline constexpr std::uint16_t ifd = 13;
} // namespace tiff_type

// Size in bytes of one value of the type, 0 for unknown types.
[[nodiscard]] std::size_t tiff_type_size(std::uint16_t type);

// ---------------------------------------------------------------------------
// Reading

struct TiffEntry {
  std::uint16_t tag{};
  std::uint16_t type{};
  std::uint32_t count{};
  bool big_endian{false};
  // Value bytes inside the parsed buffer, in file byte order.
  std::span<const std::uint8_t> data;

  // Integer value of element `index` for integer types.
  [[nodiscard]] std::optional<std::int64_t> integer(std::size_t index) const;
  // Numeric value of element `index` for any numeric type.
  [[nodiscard]] std::optional<double> number(std::size_t index) const;
  // All values as numbers; nullopt if any is not numeric or not finite.
  [[nodiscard]] std::optional<std::vector<double>> numbers() const;
  // ASCII (or BYTE) string up to the first NUL, trailing spaces removed.
  [[nodiscard]] std::string text() const;
  // Value bytes converted to little-endian.
  [[nodiscard]] std::vector<std::uint8_t> little_endian_bytes() const;
};

struct TiffIfd {
  std::vector<TiffEntry> entries;
  // Index into TiffFile::ifds of the EXIF IFD referenced by this IFD.
  std::optional<std::size_t> exif_ifd;
  bool is_exif{false};

  [[nodiscard]] const TiffEntry *find(std::uint16_t tag) const;
};

struct TiffFile {
  bool big_endian{false};
  // IFD 0 first, then the rest of the main chain, SubIFDs and EXIF IFDs.
  std::vector<TiffIfd> ifds;
};

[[nodiscard]] Result<TiffFile> parse_tiff(std::span<const std::uint8_t> bytes);

// ---------------------------------------------------------------------------
// Writing (little-endian, classic TIFF)

class IfdBuilder {
public:
  void add_raw(std::uint16_t tag, std::uint16_t type, std::uint32_t count,
               std::vector<std::uint8_t> little_endian_bytes);
  void add_bytes(std::uint16_t tag, std::uint16_t type,
                 std::span<const std::uint8_t> bytes);
  void add_ascii(std::uint16_t tag, const std::string &text);
  void add_short(std::uint16_t tag, std::span<const std::uint16_t> values);
  void add_short(std::uint16_t tag, std::uint16_t value);
  void add_long(std::uint16_t tag, std::span<const std::uint32_t> values);
  void add_long(std::uint16_t tag, std::uint32_t value);
  void add_rational(std::uint16_t tag, std::span<const double> values);
  void add_srational(std::uint16_t tag, std::span<const double> values);
  // LONG entry whose values are the file offsets of the given blobs.
  void add_blob_offsets(std::uint16_t tag, std::vector<std::size_t> blobs);
  // LONG entry whose values are the file offsets of the given IFDs.
  void add_ifd_offsets(std::uint16_t tag, std::vector<std::size_t> ifds);

  [[nodiscard]] bool empty() const { return entries_.empty(); }

private:
  friend class TiffWriter;

  enum class Kind { literal, blob_offsets, ifd_offsets };
  struct Entry {
    std::uint16_t tag{};
    std::uint16_t type{};
    std::uint32_t count{};
    Kind kind{Kind::literal};
    std::vector<std::uint8_t> bytes;
    std::vector<std::size_t> targets;
  };

  void add_entry(Entry entry);

  std::vector<Entry> entries_;
};

class TiffWriter {
public:
  // IFD 0 is the first IFD added. Returns the IFD index.
  std::size_t add_ifd(IfdBuilder ifd);
  // Returns the blob index.
  std::size_t add_blob(std::vector<std::uint8_t> bytes);

  [[nodiscard]] Result<std::vector<std::uint8_t>> serialize() const;

private:
  std::vector<IfdBuilder> ifds_;
  std::vector<std::vector<std::uint8_t>> blobs_;
};

// Converts a real number to a TIFF rational with the given denominator.
[[nodiscard]] std::pair<std::int64_t, std::int64_t>
to_rational(double value, std::int64_t denominator);

} // namespace detail
} // namespace ircore
