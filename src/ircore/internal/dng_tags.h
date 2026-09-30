#pragma once

#include <cstdint>

namespace ircore {
namespace detail {
namespace tag {

// TIFF baseline and extensions.
inline constexpr std::uint16_t new_subfile_type = 254;
inline constexpr std::uint16_t image_width = 256;
inline constexpr std::uint16_t image_length = 257;
inline constexpr std::uint16_t bits_per_sample = 258;
inline constexpr std::uint16_t compression = 259;
inline constexpr std::uint16_t photometric_interpretation = 262;
inline constexpr std::uint16_t make = 271;
inline constexpr std::uint16_t model = 272;
inline constexpr std::uint16_t strip_offsets = 273;
inline constexpr std::uint16_t orientation = 274;
inline constexpr std::uint16_t samples_per_pixel = 277;
inline constexpr std::uint16_t rows_per_strip = 278;
inline constexpr std::uint16_t strip_byte_counts = 279;
inline constexpr std::uint16_t planar_configuration = 284;
inline constexpr std::uint16_t software = 305;
inline constexpr std::uint16_t date_time = 306;
inline constexpr std::uint16_t artist = 315;
inline constexpr std::uint16_t tile_width = 322;
inline constexpr std::uint16_t tile_length = 323;
inline constexpr std::uint16_t tile_offsets = 324;
inline constexpr std::uint16_t tile_byte_counts = 325;
inline constexpr std::uint16_t sub_ifds = 330;
inline constexpr std::uint16_t sample_format = 339;
inline constexpr std::uint16_t cfa_repeat_pattern_dim = 33421;
inline constexpr std::uint16_t cfa_pattern = 33422;
inline constexpr std::uint16_t copyright = 33432;
inline constexpr std::uint16_t exif_ifd = 34665;

// EXIF entries that contain offsets and cannot be copied verbatim.
inline constexpr std::uint16_t exif_maker_note = 37500;
inline constexpr std::uint16_t exif_interoperability_ifd = 40965;

// DNG.
inline constexpr std::uint16_t dng_version = 50706;
inline constexpr std::uint16_t dng_backward_version = 50707;
inline constexpr std::uint16_t unique_camera_model = 50708;
inline constexpr std::uint16_t cfa_plane_color = 50710;
inline constexpr std::uint16_t cfa_layout = 50711;
inline constexpr std::uint16_t linearization_table = 50712;
inline constexpr std::uint16_t black_level_repeat_dim = 50713;
inline constexpr std::uint16_t black_level = 50714;
inline constexpr std::uint16_t black_level_delta_h = 50715;
inline constexpr std::uint16_t black_level_delta_v = 50716;
inline constexpr std::uint16_t white_level = 50717;
inline constexpr std::uint16_t default_scale = 50718;
inline constexpr std::uint16_t default_crop_origin = 50719;
inline constexpr std::uint16_t default_crop_size = 50720;
inline constexpr std::uint16_t color_matrix1 = 50721;
inline constexpr std::uint16_t color_matrix2 = 50722;
inline constexpr std::uint16_t camera_calibration1 = 50723;
inline constexpr std::uint16_t camera_calibration2 = 50724;
inline constexpr std::uint16_t analog_balance = 50727;
inline constexpr std::uint16_t as_shot_neutral = 50728;
inline constexpr std::uint16_t as_shot_white_xy = 50729;
inline constexpr std::uint16_t baseline_exposure = 50730;
inline constexpr std::uint16_t baseline_noise = 50731;
inline constexpr std::uint16_t baseline_sharpness = 50732;
inline constexpr std::uint16_t linear_response_limit = 50734;
inline constexpr std::uint16_t calibration_illuminant1 = 50778;
inline constexpr std::uint16_t calibration_illuminant2 = 50779;
inline constexpr std::uint16_t best_quality_scale = 50780;
inline constexpr std::uint16_t active_area = 50829;
inline constexpr std::uint16_t profile_name = 50936;
inline constexpr std::uint16_t profile_embed_policy = 50941;
inline constexpr std::uint16_t forward_matrix1 = 50964;
inline constexpr std::uint16_t forward_matrix2 = 50965;
inline constexpr std::uint16_t preview_color_space = 50970;
inline constexpr std::uint16_t opcode_list1 = 51008;
inline constexpr std::uint16_t opcode_list2 = 51009;
inline constexpr std::uint16_t opcode_list3 = 51022;

} // namespace tag

namespace photometric {
inline constexpr std::uint16_t rgb = 2;
inline constexpr std::uint16_t cfa = 32803;
inline constexpr std::uint16_t linear_raw = 34892;
} // namespace photometric

namespace compression {
inline constexpr std::uint16_t none = 1;
inline constexpr std::uint16_t lossless_jpeg = 7;
} // namespace compression

} // namespace detail
} // namespace ircore
