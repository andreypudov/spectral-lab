#include "ircore/internal/ljpeg.h"

#include <algorithm>
#include <array>
#include <bit>
#include <format>
#include <optional>

namespace ircore {
namespace detail {

namespace {

constexpr std::uint8_t marker_soi = 0xD8;
constexpr std::uint8_t marker_eoi = 0xD9;
constexpr std::uint8_t marker_sof3 = 0xC3;
constexpr std::uint8_t marker_dht = 0xC4;
constexpr std::uint8_t marker_sos = 0xDA;
constexpr std::uint8_t marker_dri = 0xDD;
constexpr std::uint8_t marker_rst0 = 0xD0;
constexpr std::uint8_t marker_rst7 = 0xD7;

// Number of Huffman symbols of lossless coding: difference categories 0-16.
constexpr std::size_t symbol_count = 17;
constexpr std::size_t max_components = 4;

bool is_non_lossless_sof(std::uint8_t marker) {
  return (marker >= 0xC0 && marker <= 0xCF) && marker != marker_sof3 &&
         marker != marker_dht && marker != 0xC8 && marker != 0xCC;
}

std::int32_t predict(std::uint32_t predictor, std::int32_t ra, std::int32_t rb,
                     std::int32_t rc) {
  switch (predictor) {
  case 1:
    return ra;
  case 2:
    return rb;
  case 3:
    return rc;
  case 4:
    return ra + rb - rc;
  case 5:
    return ra + ((rb - rc) >> 1);
  case 6:
    return rb + ((ra - rc) >> 1);
  default:
    return (ra + rb) >> 1;
  }
}

// Prediction of sample (row, col, component). `first_row` is true for the
// first row of the scan and of each restart interval.
std::int32_t prediction(const std::uint16_t *out, std::size_t row,
                        std::size_t col, std::size_t component,
                        std::size_t width, std::size_t components,
                        std::uint32_t predictor, bool first_row,
                        std::int32_t initial) {
  const std::size_t stride = width * components;
  const std::uint16_t *here = out + row * stride + col * components + component;
  if (first_row) {
    return col == 0 ? initial : here[-static_cast<std::ptrdiff_t>(components)];
  }
  if (col == 0) {
    return here[-static_cast<std::ptrdiff_t>(stride)];
  }
  const std::int32_t ra = here[-static_cast<std::ptrdiff_t>(components)];
  const std::int32_t rb = here[-static_cast<std::ptrdiff_t>(stride)];
  const std::int32_t rc =
      here[-static_cast<std::ptrdiff_t>(stride + components)];
  return predict(predictor, ra, rb, rc);
}

// ---------------------------------------------------------------------------
// Decoding

struct HuffmanTable {
  // Indexed by the next 16 bits: (code length << 8) | symbol, 0 if invalid.
  std::vector<std::uint16_t> lookup;
};

Result<HuffmanTable> build_table(const std::array<std::uint8_t, 16> &bits,
                                 std::span<const std::uint8_t> values) {
  HuffmanTable table;
  table.lookup.assign(std::size_t{1} << 16U, 0);
  std::uint32_t code = 0;
  std::size_t k = 0;
  for (std::uint32_t length = 1; length <= 16; ++length) {
    for (std::uint32_t i = 0; i < bits[length - 1]; ++i, ++k) {
      if (code >= (1U << length)) {
        return make_error(ErrorCode::invalid_format,
                          "invalid lossless JPEG Huffman table");
      }
      const std::uint32_t first = code << (16 - length);
      const std::uint32_t last = (code + 1) << (16 - length);
      const auto entry = static_cast<std::uint16_t>((length << 8U) | values[k]);
      std::fill(table.lookup.begin() + first, table.lookup.begin() + last,
                entry);
      ++code;
    }
    code <<= 1U;
  }
  return table;
}

class BitReader {
public:
  BitReader(std::span<const std::uint8_t> data, std::size_t position)
      : data_(data), position_(position) {}

  std::uint32_t peek16() {
    fill();
    return static_cast<std::uint32_t>(buffer_ >> 48U);
  }

  void skip(std::uint32_t count) {
    buffer_ <<= count;
    bits_ -= static_cast<int>(count);
  }

  std::uint32_t get(std::uint32_t count) {
    if (count == 0) {
      return 0;
    }
    fill();
    const auto value = static_cast<std::uint32_t>(buffer_ >> (64U - count));
    skip(count);
    return value;
  }

  // Consumes a restart marker; the bit buffer is byte aligned by the marker.
  bool restart() {
    buffer_ = 0;
    bits_ = 0;
    padding_bytes_ = 0;
    if (!marker_hit_ || position_ + 1 >= data_.size() ||
        data_[position_] != 0xFF || data_[position_ + 1] < marker_rst0 ||
        data_[position_ + 1] > marker_rst7) {
      return false;
    }
    position_ += 2;
    marker_hit_ = false;
    return true;
  }

  // Bits consumed beyond the end of the entropy coded data.
  [[nodiscard]] std::int64_t overrun_bits() const {
    return std::max<std::int64_t>(0, padding_bytes_ * 8 - bits_);
  }

private:
  void fill() {
    while (bits_ <= 56) {
      std::uint8_t byte = 0;
      if (marker_hit_ || position_ >= data_.size()) {
        ++padding_bytes_;
      } else if (data_[position_] != 0xFF) {
        byte = data_[position_++];
      } else if (position_ + 1 < data_.size() && data_[position_ + 1] == 0) {
        byte = 0xFF;
        position_ += 2;
      } else {
        marker_hit_ = true;
        ++padding_bytes_;
      }
      buffer_ |= static_cast<std::uint64_t>(byte) << (56 - bits_);
      bits_ += 8;
    }
  }

  std::span<const std::uint8_t> data_;
  std::size_t position_;
  std::uint64_t buffer_{0};
  int bits_{0};
  std::int64_t padding_bytes_{0};
  bool marker_hit_{false};
};

struct FrameComponent {
  std::uint8_t id{};
  std::size_t table{};
};

struct DecoderState {
  std::optional<LosslessJpegImage> frame;
  std::array<FrameComponent, max_components> components{};
  std::array<std::optional<HuffmanTable>, 4> tables;
  std::uint32_t restart_interval{0};
};

Result<void> parse_frame(std::span<const std::uint8_t> segment,
                         DecoderState &state) {
  if (segment.size() < 6) {
    return make_error(ErrorCode::invalid_format, "truncated JPEG frame header");
  }
  LosslessJpegImage frame;
  frame.precision = segment[0];
  frame.height = (static_cast<std::uint32_t>(segment[1]) << 8U) | segment[2];
  frame.width = (static_cast<std::uint32_t>(segment[3]) << 8U) | segment[4];
  frame.components = segment[5];
  if (frame.precision < 2 || frame.precision > 16 || frame.width == 0 ||
      frame.height == 0 || frame.components == 0 ||
      frame.components > max_components ||
      segment.size() != 6 + 3 * std::size_t{frame.components}) {
    return make_error(ErrorCode::invalid_format,
                      "invalid lossless JPEG frame header");
  }
  for (std::size_t c = 0; c < frame.components; ++c) {
    state.components[c].id = segment[6 + c * 3];
    if (segment[7 + c * 3] != 0x11) {
      return make_error(ErrorCode::unsupported_format,
                        "subsampled lossless JPEG is not supported");
    }
  }
  state.frame = std::move(frame);
  return {};
}

Result<void> parse_tables(std::span<const std::uint8_t> segment,
                          DecoderState &state) {
  std::size_t at = 0;
  while (at < segment.size()) {
    if (segment.size() - at < 17) {
      return make_error(ErrorCode::invalid_format, "truncated JPEG DHT");
    }
    const std::uint8_t table_class = segment[at] >> 4U;
    const std::uint8_t table_id = segment[at] & 0x0FU;
    if (table_class != 0 || table_id >= 4) {
      return make_error(ErrorCode::invalid_format,
                        "invalid lossless JPEG Huffman table id");
    }
    std::array<std::uint8_t, 16> bits{};
    std::size_t total = 0;
    for (std::size_t i = 0; i < 16; ++i) {
      bits[i] = segment[at + 1 + i];
      total += bits[i];
    }
    at += 17;
    if (total > 256 || segment.size() - at < total) {
      return make_error(ErrorCode::invalid_format, "truncated JPEG DHT");
    }
    auto table = build_table(bits, segment.subspan(at, total));
    if (!table) {
      return std::unexpected(table.error());
    }
    state.tables[table_id] = std::move(*table);
    at += total;
  }
  return {};
}

Result<LosslessJpegImage> decode_scan(std::span<const std::uint8_t> stream,
                                      std::span<const std::uint8_t> header,
                                      std::size_t data_start,
                                      DecoderState &state) {
  if (!state.frame) {
    return make_error(ErrorCode::invalid_format, "JPEG scan before frame");
  }
  LosslessJpegImage image = std::move(*state.frame);
  state.frame.reset();
  const std::size_t nc = image.components;
  if (header.empty() || header[0] != nc || header.size() != 1 + 2 * nc + 3) {
    return make_error(ErrorCode::unsupported_format,
                      "lossless JPEG scans must contain all components");
  }
  std::array<const HuffmanTable *, max_components> tables{};
  for (std::size_t i = 0; i < nc; ++i) {
    const std::uint8_t id = header[1 + i * 2];
    const std::size_t table_id = header[2 + i * 2] >> 4U;
    if (state.components[i].id != id || table_id >= 4 ||
        !state.tables[table_id]) {
      return make_error(ErrorCode::invalid_format,
                        "invalid lossless JPEG scan header");
    }
    tables[i] = &*state.tables[table_id];
  }
  const std::uint32_t predictor = header[1 + 2 * nc];
  const std::uint32_t point_transform = header[3 + 2 * nc] & 0x0FU;
  if (predictor < 1 || predictor > 7 || point_transform >= image.precision) {
    return make_error(ErrorCode::invalid_format,
                      "invalid lossless JPEG predictor or point transform");
  }
  const std::size_t width = image.width;
  std::size_t rows_per_interval = 0;
  if (state.restart_interval != 0) {
    if (state.restart_interval % width != 0) {
      return make_error(ErrorCode::unsupported_format,
                        "lossless JPEG restart interval is not whole rows");
    }
    rows_per_interval = state.restart_interval / width;
  }

  image.samples.assign(width * image.height * nc, 0);
  std::uint16_t *out = image.samples.data();
  const std::int32_t initial = 1 << (image.precision - point_transform - 1);
  BitReader reader(stream, data_start);

  for (std::size_t row = 0; row < image.height; ++row) {
    bool first_row = row == 0;
    if (rows_per_interval != 0 && row != 0 && row % rows_per_interval == 0) {
      if (!reader.restart()) {
        return make_error(ErrorCode::invalid_format,
                          "missing lossless JPEG restart marker");
      }
      first_row = true;
    }
    for (std::size_t col = 0; col < width; ++col) {
      for (std::size_t c = 0; c < nc; ++c) {
        const std::uint16_t entry = tables[c]->lookup[reader.peek16()];
        const std::uint32_t length = entry >> 8U;
        const std::uint32_t category = entry & 0xFFU;
        if (length == 0 || category > 16) {
          return make_error(ErrorCode::invalid_format,
                            "corrupt lossless JPEG data");
        }
        reader.skip(length);
        std::int32_t diff = 0;
        if (category == 16) {
          diff = 32768;
        } else if (category != 0) {
          const auto bits = static_cast<std::int32_t>(reader.get(category));
          diff = bits < (1 << (category - 1)) ? bits - (1 << category) + 1
                                               : bits;
        }
        const std::int32_t pred = prediction(out, row, col, c, width, nc,
                                             predictor, first_row, initial);
        out[(row * width + col) * nc + c] =
            static_cast<std::uint16_t>((pred + diff) & 0xFFFF);
      }
    }
  }
  // Tolerate a few bytes of missing padding but reject truncated data.
  if (reader.overrun_bits() > 16) {
    return make_error(ErrorCode::invalid_format,
                      "lossless JPEG data is truncated");
  }
  if (point_transform != 0) {
    for (auto &sample : image.samples) {
      sample = static_cast<std::uint16_t>(sample << point_transform);
    }
  }
  return image;
}

// ---------------------------------------------------------------------------
// Encoding

struct HuffmanCode {
  std::uint16_t code{};
  std::uint8_t length{};
};

struct EncoderTable {
  std::array<std::uint8_t, 16> bits{};
  std::vector<std::uint8_t> values;
  std::array<HuffmanCode, symbol_count> codes{};
};

// Optimal code lengths limited to 16 bits (ITU T.81 Annex K.2, as in
// libjpeg's jpeg_gen_optimal_table). A reserved symbol keeps the all-ones
// code unused.
EncoderTable build_encoder_table(const std::array<std::uint64_t, symbol_count>
                                     &frequencies) {
  constexpr std::size_t reserved = symbol_count;
  std::array<std::uint64_t, symbol_count + 1> freq{};
  std::copy(frequencies.begin(), frequencies.end(), freq.begin());
  freq[reserved] = 1;
  std::array<int, symbol_count + 1> code_size{};
  std::array<int, symbol_count + 1> others{};
  others.fill(-1);

  for (;;) {
    int c1 = -1;
    int c2 = -1;
    std::uint64_t v1 = UINT64_MAX;
    std::uint64_t v2 = UINT64_MAX;
    for (std::size_t i = 0; i <= reserved; ++i) {
      if (freq[i] != 0 && freq[i] <= v1) {
        v1 = freq[i];
        c1 = static_cast<int>(i);
      }
    }
    for (std::size_t i = 0; i <= reserved; ++i) {
      if (freq[i] != 0 && freq[i] <= v2 && static_cast<int>(i) != c1) {
        v2 = freq[i];
        c2 = static_cast<int>(i);
      }
    }
    if (c2 < 0) {
      break;
    }
    freq[static_cast<std::size_t>(c1)] += freq[static_cast<std::size_t>(c2)];
    freq[static_cast<std::size_t>(c2)] = 0;
    ++code_size[static_cast<std::size_t>(c1)];
    while (others[static_cast<std::size_t>(c1)] >= 0) {
      c1 = others[static_cast<std::size_t>(c1)];
      ++code_size[static_cast<std::size_t>(c1)];
    }
    others[static_cast<std::size_t>(c1)] = c2;
    ++code_size[static_cast<std::size_t>(c2)];
    while (others[static_cast<std::size_t>(c2)] >= 0) {
      c2 = others[static_cast<std::size_t>(c2)];
      ++code_size[static_cast<std::size_t>(c2)];
    }
  }

  std::array<int, 33> bits{};
  for (const int size : code_size) {
    if (size != 0) {
      ++bits[static_cast<std::size_t>(size)];
    }
  }
  for (std::size_t i = 32; i > 16; --i) {
    while (bits[i] > 0) {
      std::size_t j = i - 2;
      while (bits[j] == 0) {
        --j;
      }
      bits[i] -= 2;
      bits[i - 1] += 1;
      bits[j + 1] += 2;
      bits[j] -= 1;
    }
  }
  std::size_t longest = 16;
  while (bits[longest] == 0) {
    --longest;
  }
  --bits[longest]; // Remove the reserved code point.

  EncoderTable table;
  for (std::size_t i = 0; i < 16; ++i) {
    table.bits[i] = static_cast<std::uint8_t>(bits[i + 1]);
  }
  for (int length = 1; length <= 32; ++length) {
    for (std::size_t symbol = 0; symbol < symbol_count; ++symbol) {
      if (code_size[symbol] == length) {
        table.values.push_back(static_cast<std::uint8_t>(symbol));
      }
    }
  }
  // Symbols sorted by length receive canonical codes; the adjusted `bits`
  // decides the final length of each symbol in that order.
  table.values.resize(std::min<std::size_t>(table.values.size(), 256));
  std::uint32_t code = 0;
  std::size_t k = 0;
  for (std::uint32_t length = 1; length <= 16; ++length) {
    for (std::uint32_t i = 0; i < table.bits[length - 1]; ++i, ++k) {
      table.codes[table.values[k]] = {static_cast<std::uint16_t>(code),
                                      static_cast<std::uint8_t>(length)};
      ++code;
    }
    code <<= 1U;
  }
  table.values.resize(k);
  return table;
}

class BitWriter {
public:
  explicit BitWriter(std::vector<std::uint8_t> &out) : out_(out) {}

  void put(std::uint32_t value, std::uint32_t length) {
    if (length == 0) {
      return;
    }
    accumulator_ = (accumulator_ << length) | (value & ((1U << length) - 1U));
    count_ += length;
    while (count_ >= 8) {
      const auto byte = static_cast<std::uint8_t>(accumulator_ >> (count_ - 8));
      out_.push_back(byte);
      if (byte == 0xFF) {
        out_.push_back(0);
      }
      count_ -= 8;
    }
    accumulator_ &= (std::uint64_t{1} << count_) - 1U;
  }

  void flush() {
    if (count_ != 0) {
      put((1U << (8 - count_)) - 1U, 8 - count_);
    }
  }

private:
  std::vector<std::uint8_t> &out_;
  std::uint64_t accumulator_{0};
  std::uint32_t count_{0};
};

void put_marker(std::vector<std::uint8_t> &out, std::uint8_t marker) {
  out.push_back(0xFF);
  out.push_back(marker);
}

void put_u16(std::vector<std::uint8_t> &out, std::size_t value) {
  out.push_back(static_cast<std::uint8_t>(value >> 8U));
  out.push_back(static_cast<std::uint8_t>(value));
}

} // namespace

Result<LosslessJpegImage>
decode_lossless_jpeg(std::span<const std::uint8_t> stream) {
  if (stream.size() < 4 || stream[0] != 0xFF || stream[1] != marker_soi) {
    return make_error(ErrorCode::invalid_format, "missing JPEG SOI marker");
  }
  DecoderState state;
  std::size_t at = 2;
  for (;;) {
    while (at < stream.size() && stream[at] != 0xFF) {
      ++at; // Skip garbage between segments.
    }
    while (at < stream.size() && stream[at] == 0xFF) {
      ++at; // Fill bytes.
    }
    if (at >= stream.size()) {
      return make_error(ErrorCode::invalid_format, "JPEG stream has no scan");
    }
    const std::uint8_t marker = stream[at++];
    if (marker == marker_soi || marker == 0x01 ||
        (marker >= marker_rst0 && marker <= marker_rst7)) {
      continue;
    }
    if (marker == marker_eoi) {
      return make_error(ErrorCode::invalid_format, "JPEG stream has no scan");
    }
    if (stream.size() - at < 2) {
      return make_error(ErrorCode::invalid_format, "truncated JPEG segment");
    }
    const std::size_t length =
        (static_cast<std::size_t>(stream[at]) << 8U) | stream[at + 1];
    if (length < 2 || stream.size() - at < length) {
      return make_error(ErrorCode::invalid_format, "truncated JPEG segment");
    }
    const auto segment = stream.subspan(at + 2, length - 2);
    at += length;

    if (marker == marker_sof3) {
      if (auto parsed = parse_frame(segment, state); !parsed) {
        return std::unexpected(parsed.error());
      }
    } else if (is_non_lossless_sof(marker)) {
      return make_error(ErrorCode::unsupported_format,
                        std::format("unsupported JPEG process (marker 0x{:02X});"
                                    " only lossless Huffman JPEG is supported",
                                    marker));
    } else if (marker == marker_dht) {
      if (auto parsed = parse_tables(segment, state); !parsed) {
        return std::unexpected(parsed.error());
      }
    } else if (marker == marker_dri) {
      if (segment.size() != 2) {
        return make_error(ErrorCode::invalid_format, "invalid JPEG DRI");
      }
      state.restart_interval =
          (static_cast<std::uint32_t>(segment[0]) << 8U) | segment[1];
    } else if (marker == marker_sos) {
      return decode_scan(stream, segment, at, state);
    }
  }
}

Result<std::vector<std::uint8_t>>
encode_lossless_jpeg(std::span<const std::uint16_t> samples,
                     const LosslessJpegEncodeParams &params) {
  const std::size_t width = params.width;
  const std::size_t height = params.height;
  const std::size_t nc = params.components;
  if (width == 0 || width > 65535 || height == 0 || height > 65535 ||
      nc == 0 || nc > max_components || params.precision < 2 ||
      params.precision > 16 || params.predictor < 1 || params.predictor > 7) {
    return make_error(ErrorCode::invalid_argument,
                      "invalid lossless JPEG encoder parameters");
  }
  if (samples.size() != width * height * nc) {
    return make_error(ErrorCode::invalid_argument,
                      "lossless JPEG sample count does not match dimensions");
  }
  const std::uint32_t max_value = (1U << params.precision) - 1U;
  if (std::ranges::any_of(samples,
                          [&](std::uint16_t v) { return v > max_value; })) {
    return make_error(ErrorCode::invalid_argument,
                      "sample exceeds the lossless JPEG precision");
  }

  const std::int32_t initial = 1 << (params.precision - 1);
  std::vector<std::int32_t> diffs(samples.size());
  std::array<std::array<std::uint64_t, symbol_count>, max_components> freq{};
  for (std::size_t row = 0; row < height; ++row) {
    for (std::size_t col = 0; col < width; ++col) {
      for (std::size_t c = 0; c < nc; ++c) {
        const std::size_t index = (row * width + col) * nc + c;
        const std::int32_t pred =
            prediction(samples.data(), row, col, c, width, nc,
                       params.predictor, row == 0, initial);
        std::int32_t diff = (samples[index] - pred) & 0xFFFF;
        if (diff >= 32768) {
          diff -= 65536;
        }
        diffs[index] = diff;
        const auto magnitude = static_cast<std::uint32_t>(diff < 0 ? -diff : diff);
        ++freq[c][static_cast<std::size_t>(std::bit_width(magnitude))];
      }
    }
  }

  std::vector<EncoderTable> tables;
  for (std::size_t c = 0; c < nc; ++c) {
    tables.push_back(build_encoder_table(freq[c]));
  }

  std::vector<std::uint8_t> out;
  out.reserve(samples.size() + 1024);
  put_marker(out, marker_soi);

  put_marker(out, marker_sof3);
  put_u16(out, 8 + 3 * nc);
  out.push_back(static_cast<std::uint8_t>(params.precision));
  put_u16(out, height);
  put_u16(out, width);
  out.push_back(static_cast<std::uint8_t>(nc));
  for (std::size_t c = 0; c < nc; ++c) {
    out.push_back(static_cast<std::uint8_t>(c));
    out.push_back(0x11);
    out.push_back(0);
  }

  for (std::size_t c = 0; c < nc; ++c) {
    put_marker(out, marker_dht);
    put_u16(out, 2 + 1 + 16 + tables[c].values.size());
    out.push_back(static_cast<std::uint8_t>(c));
    out.insert(out.end(), tables[c].bits.begin(), tables[c].bits.end());
    out.insert(out.end(), tables[c].values.begin(), tables[c].values.end());
  }

  put_marker(out, marker_sos);
  put_u16(out, 6 + 2 * nc);
  out.push_back(static_cast<std::uint8_t>(nc));
  for (std::size_t c = 0; c < nc; ++c) {
    out.push_back(static_cast<std::uint8_t>(c));
    out.push_back(static_cast<std::uint8_t>(c << 4U));
  }
  out.push_back(static_cast<std::uint8_t>(params.predictor));
  out.push_back(0); // Se
  out.push_back(0); // Ah, Al (no point transform)

  BitWriter writer(out);
  for (std::size_t index = 0; index < diffs.size(); ++index) {
    const std::int32_t diff = diffs[index];
    const auto magnitude = static_cast<std::uint32_t>(diff < 0 ? -diff : diff);
    const auto category = static_cast<std::uint32_t>(std::bit_width(magnitude));
    const HuffmanCode code = tables[index % nc].codes[category];
    writer.put(code.code, code.length);
    if (category != 0 && category != 16) {
      const auto extra = static_cast<std::uint32_t>(
          diff >= 0 ? diff : diff + (1 << category) - 1);
      writer.put(extra, category);
    }
  }
  writer.flush();
  put_marker(out, marker_eoi);
  return out;
}

} // namespace detail
} // namespace ircore
