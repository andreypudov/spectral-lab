#include "support/synthetic.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <random>

namespace test_support {

namespace {

std::uint16_t encode(double value, const SyntheticCamera &camera) {
  const double raw = camera.black + value * (camera.white - camera.black);
  return static_cast<std::uint16_t>(
      std::clamp(std::round(raw), 0.0, camera.white));
}

} // namespace

ircore::DngMetadata camera_metadata(const SyntheticCamera &camera) {
  ircore::DngMetadata meta;
  meta.make = "Synthetic";
  meta.model = "IR Test Camera";
  meta.unique_camera_model = "Synthetic IR Test Camera";
  meta.bits_per_sample = camera.bits;
  meta.cfa_pattern = camera.pattern;
  meta.black_level = {camera.black};
  meta.white_level = {camera.white};

  ircore::ColorCalibration tungsten;
  tungsten.illuminant = ircore::light_source::standard_a;
  tungsten.color_matrix = ircore::Matrix3::from_row_major(
      {0.7229, -0.2786, -0.0306, -0.3970, 1.1627, 0.2653, -0.0520, 0.1289,
       0.6536});
  ircore::ColorCalibration daylight;
  daylight.illuminant = ircore::light_source::d65;
  daylight.color_matrix = ircore::Matrix3::from_row_major(
      {0.7662, -0.3686, -0.0183, -0.5051, 1.2914, 0.2433, -0.1003, 0.2025,
       0.6010});
  meta.calibrations = {tungsten, daylight};
  meta.as_shot_neutral = ircore::Vector3{1.0, 0.62, 0.51};
  meta.baseline_exposure = 0.35;
  return meta;
}

ircore::DngImage make_cfa_dng(std::uint32_t width, std::uint32_t height,
                              const Scene &scene,
                              const SyntheticCamera &camera) {
  ircore::DngImage dng;
  dng.metadata = camera_metadata(camera);
  dng.metadata.photometric = ircore::Photometric::cfa;
  dng.image.width = width;
  dng.image.height = height;
  dng.image.samples_per_pixel = 1;
  dng.image.samples.resize(dng.image.sample_count());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto rgb = scene(x, y);
      const auto color = static_cast<std::size_t>(camera.pattern.at(y, x));
      dng.image.samples[std::size_t{y} * width + x] = encode(rgb[color], camera);
    }
  }
  return dng;
}

ircore::DngImage make_linear_dng(std::uint32_t width, std::uint32_t height,
                                 const Scene &scene,
                                 const SyntheticCamera &camera) {
  ircore::DngImage dng;
  dng.metadata = camera_metadata(camera);
  dng.metadata.photometric = ircore::Photometric::linear_raw;
  dng.metadata.black_level = {camera.black, camera.black, camera.black};
  dng.metadata.white_level = {camera.white, camera.white, camera.white};
  dng.image.width = width;
  dng.image.height = height;
  dng.image.samples_per_pixel = 3;
  dng.image.samples.resize(dng.image.sample_count());
  for (std::uint32_t y = 0; y < height; ++y) {
    for (std::uint32_t x = 0; x < width; ++x) {
      const auto rgb = scene(x, y);
      for (std::size_t c = 0; c < 3; ++c) {
        dng.image.samples[(std::size_t{y} * width + x) * 3 + c] =
            encode(rgb[c], camera);
      }
    }
  }
  return dng;
}

Scene uniform_scene(ircore::Vector3 camera_rgb) {
  return [camera_rgb](std::uint32_t, std::uint32_t) { return camera_rgb; };
}

// ---------------------------------------------------------------------------

std::vector<std::uint8_t> TiffBuilder::u16(std::uint16_t value) const {
  if (big_endian_) {
    return {static_cast<std::uint8_t>(value >> 8U),
            static_cast<std::uint8_t>(value)};
  }
  return {static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8U)};
}

std::vector<std::uint8_t> TiffBuilder::u32(std::uint32_t value) const {
  std::vector<std::uint8_t> bytes(4);
  for (std::size_t i = 0; i < 4; ++i) {
    const std::size_t shift = big_endian_ ? (3 - i) * 8 : i * 8;
    bytes[i] = static_cast<std::uint8_t>(value >> shift);
  }
  return bytes;
}

void TiffBuilder::add(std::uint16_t tag, std::uint16_t type, std::uint32_t count,
                      std::vector<std::uint8_t> bytes) {
  entries_.push_back({tag, type, count, std::move(bytes)});
}

void TiffBuilder::add_short(std::uint16_t tag,
                            std::vector<std::uint16_t> values) {
  std::vector<std::uint8_t> bytes;
  for (const auto value : values) {
    const auto encoded = u16(value);
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
  }
  add(tag, 3, static_cast<std::uint32_t>(values.size()), std::move(bytes));
}

void TiffBuilder::add_long(std::uint16_t tag, std::vector<std::uint32_t> values) {
  std::vector<std::uint8_t> bytes;
  for (const auto value : values) {
    const auto encoded = u32(value);
    bytes.insert(bytes.end(), encoded.begin(), encoded.end());
  }
  add(tag, 4, static_cast<std::uint32_t>(values.size()), std::move(bytes));
}

void TiffBuilder::add_bytes(std::uint16_t tag, std::vector<std::uint8_t> values) {
  const auto count = static_cast<std::uint32_t>(values.size());
  add(tag, 1, count, std::move(values));
}

void TiffBuilder::add_srational(std::uint16_t tag, std::vector<double> values) {
  std::vector<std::uint8_t> bytes;
  for (const double value : values) {
    const auto num = u32(static_cast<std::uint32_t>(
        static_cast<std::int32_t>(std::lround(value * 10000.0))));
    const auto den = u32(10000);
    bytes.insert(bytes.end(), num.begin(), num.end());
    bytes.insert(bytes.end(), den.begin(), den.end());
  }
  add(tag, 10, static_cast<std::uint32_t>(values.size()), std::move(bytes));
}

void TiffBuilder::add_rational(std::uint16_t tag, std::vector<double> values) {
  std::vector<std::uint8_t> bytes;
  for (const double value : values) {
    const auto num = u32(static_cast<std::uint32_t>(std::lround(value * 10000.0)));
    const auto den = u32(10000);
    bytes.insert(bytes.end(), num.begin(), num.end());
    bytes.insert(bytes.end(), den.begin(), den.end());
  }
  add(tag, 5, static_cast<std::uint32_t>(values.size()), std::move(bytes));
}

void TiffBuilder::add_ascii(std::uint16_t tag, const std::string &text) {
  std::vector<std::uint8_t> bytes(text.begin(), text.end());
  bytes.push_back(0);
  const auto count = static_cast<std::uint32_t>(bytes.size());
  add(tag, 2, count, std::move(bytes));
}

void TiffBuilder::set_image_data(std::uint16_t offset_tag,
                                 std::uint16_t count_tag,
                                 std::vector<std::uint8_t> data) {
  offset_tag_ = offset_tag;
  count_tag_ = count_tag;
  data_ = std::move(data);
}

std::vector<std::uint8_t> TiffBuilder::build() const {
  auto entries = entries_;
  if (offset_tag_ != 0) {
    entries.push_back({offset_tag_, 4, 1, u32(0)}); // Patched below.
    entries.push_back(
        {count_tag_, 4, 1, u32(static_cast<std::uint32_t>(data_.size()))});
  }
  std::ranges::sort(entries, {}, &Entry::tag);

  std::vector<std::uint8_t> out;
  if (big_endian_) {
    out = {'M', 'M', 0, 42};
  } else {
    out = {'I', 'I', 42, 0};
  }
  const auto first = u32(8);
  out.insert(out.end(), first.begin(), first.end());

  const std::size_t table = 8 + 2 + entries.size() * 12 + 4;
  std::size_t values_size = 0;
  for (const auto &entry : entries) {
    if (entry.bytes.size() > 4) {
      values_size += entry.bytes.size() + (entry.bytes.size() & 1U);
    }
  }
  const std::size_t data_offset = table + values_size;

  std::vector<std::uint8_t> values;
  const auto count = u16(static_cast<std::uint16_t>(entries.size()));
  out.insert(out.end(), count.begin(), count.end());
  for (const auto &entry : entries) {
    auto bytes = entry.bytes;
    if (entry.tag == offset_tag_ && offset_tag_ != 0) {
      bytes = u32(static_cast<std::uint32_t>(data_offset));
    }
    const auto tag = u16(entry.tag);
    const auto type = u16(entry.type);
    const auto n = u32(entry.count);
    out.insert(out.end(), tag.begin(), tag.end());
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), n.begin(), n.end());
    if (bytes.size() <= 4) {
      bytes.resize(4, 0);
      out.insert(out.end(), bytes.begin(), bytes.end());
    } else {
      const auto offset = u32(static_cast<std::uint32_t>(table + values.size()));
      out.insert(out.end(), offset.begin(), offset.end());
      values.insert(values.end(), bytes.begin(), bytes.end());
      if (values.size() & 1U) {
        values.push_back(0);
      }
    }
  }
  const auto next = u32(0);
  out.insert(out.end(), next.begin(), next.end());
  out.insert(out.end(), values.begin(), values.end());
  out.insert(out.end(), data_.begin(), data_.end());
  return out;
}

// ---------------------------------------------------------------------------

TemporaryDirectory::TemporaryDirectory() {
  static std::atomic<unsigned> counter{0};
  std::random_device random;
  path_ = std::filesystem::temp_directory_path() /
          std::format("spectral-lab-test-{:08x}-{}", random(), counter++);
  std::filesystem::create_directories(path_);
}

TemporaryDirectory::~TemporaryDirectory() {
  std::error_code ignored;
  std::filesystem::remove_all(path_, ignored);
}

} // namespace test_support
