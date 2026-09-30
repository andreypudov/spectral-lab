#pragma once

#include "ircore/color.h"
#include "ircore/dng.h"
#include "ircore/image.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <vector>

namespace test_support {

// Camera RGB (linear, 0 = black, 1 = white level) of the scene at a pixel.
using Scene = std::function<ircore::Vector3(std::uint32_t x, std::uint32_t y)>;

struct SyntheticCamera {
  double black{512.0};
  double white{16383.0};
  std::uint16_t bits{14};
  ircore::CfaPattern pattern;
};

// Metadata of a plausible daylight-calibrated camera (Sony-like matrices,
// StdA and D65 calibrations, no forward matrix).
[[nodiscard]] ircore::DngMetadata camera_metadata(const SyntheticCamera &camera);

// CFA DNG whose mosaic samples the scene through the Bayer pattern.
[[nodiscard]] ircore::DngImage make_cfa_dng(std::uint32_t width,
                                            std::uint32_t height,
                                            const Scene &scene,
                                            const SyntheticCamera &camera = {});

// LinearRaw DNG with camera RGB samples of the scene.
[[nodiscard]] ircore::DngImage
make_linear_dng(std::uint32_t width, std::uint32_t height, const Scene &scene,
                const SyntheticCamera &camera = {});

// Scene of a uniform IR neutral with the given raw channel response.
[[nodiscard]] Scene uniform_scene(ircore::Vector3 camera_rgb);

// Minimal single-IFD TIFF builder with a selectable byte order, used to
// craft inputs the production writer never produces.
class TiffBuilder {
public:
  explicit TiffBuilder(bool big_endian) : big_endian_(big_endian) {}

  void add(std::uint16_t tag, std::uint16_t type, std::uint32_t count,
           std::vector<std::uint8_t> value_bytes_in_file_order);
  void add_short(std::uint16_t tag, std::vector<std::uint16_t> values);
  void add_long(std::uint16_t tag, std::vector<std::uint32_t> values);
  void add_bytes(std::uint16_t tag, std::vector<std::uint8_t> values);
  void add_srational(std::uint16_t tag, std::vector<double> values);
  void add_rational(std::uint16_t tag, std::vector<double> values);
  void add_ascii(std::uint16_t tag, const std::string &text);
  // Image data appended after the IFD; its offset is stored in `offset_tag`
  // (LONG, one value) and its size in `count_tag`.
  void set_image_data(std::uint16_t offset_tag, std::uint16_t count_tag,
                      std::vector<std::uint8_t> data);

  [[nodiscard]] std::vector<std::uint8_t> build() const;

  [[nodiscard]] std::vector<std::uint8_t> u16(std::uint16_t value) const;
  [[nodiscard]] std::vector<std::uint8_t> u32(std::uint32_t value) const;

private:
  struct Entry {
    std::uint16_t tag;
    std::uint16_t type;
    std::uint32_t count;
    std::vector<std::uint8_t> bytes;
  };
  bool big_endian_;
  std::vector<Entry> entries_;
  std::uint16_t offset_tag_{0};
  std::uint16_t count_tag_{0};
  std::vector<std::uint8_t> data_;
};

// Temporary directory removed on destruction.
class TemporaryDirectory {
public:
  TemporaryDirectory();
  ~TemporaryDirectory();
  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

} // namespace test_support
