#!/usr/bin/env python3
"""Development tool: convert a camera raw file into a minimal CFA DNG.

Spectral Lab reads DNG only. This script produces test inputs from camera raw
files when Adobe DNG Converter is not at hand. It uses LibRaw (rawpy) to
decode and tifffile to write, so the result also cross-checks the DNG reader
against an independent TIFF writer.

Only Bayer sensors are converted faithfully; X-Trans files produce a DNG with
a 6x6 CFA pattern, which Spectral Lab rejects by design.

Usage: raw2dng.py <input.raw> <output.dng> [<|>]   (byte order, default <)
Requires: pip install rawpy numpy tifffile
"""
import sys
from fractions import Fraction

import numpy as np
import rawpy
import tifffile


def rationals(values):
    out = []
    for value in values:
        fraction = Fraction(float(value)).limit_denominator(10000)
        out += [fraction.numerator, fraction.denominator]
    return out


def main():
    source, target = sys.argv[1], sys.argv[2]
    byte_order = sys.argv[3] if len(sys.argv) > 3 else '<'
    raw = rawpy.imread(source)
    image = raw.raw_image_visible.copy().astype(np.uint16)
    pattern = raw.raw_pattern
    rows, cols = pattern.shape
    # LibRaw color indices: 0 red, 1 green, 2 blue, 3 second green.
    cfa = [int({0: 0, 1: 1, 2: 2, 3: 1}[int(v)]) for v in pattern.flatten()]
    # LibRaw's rgb_xyz_matrix is Adobe's D65 ColorMatrix (XYZ to camera).
    color_matrix = raw.rgb_xyz_matrix[:3].flatten()
    multipliers = np.array(raw.camera_whitebalance[:3], dtype=float)
    neutral = (1.0 / multipliers) / (1.0 / multipliers).max()
    name = source.split('/')[-1]
    tags = [
        (50706, 'B', 4, (1, 4, 0, 0), True),                # DNGVersion
        (50708, 's', 0, 'Converted ' + name, True),         # UniqueCameraModel
        (33421, 'H', 2, (rows, cols), True),                # CFARepeatPatternDim
        (33422, 'B', len(cfa), tuple(cfa), True),           # CFAPattern
        (50721, '2i', 9, tuple(rationals(color_matrix)), True),  # ColorMatrix1
        (50778, 'H', 1, 21, True),                          # CalibrationIlluminant1
        (50728, '2I', 3, tuple(rationals(neutral)), True),  # AsShotNeutral
        (50714, 'H', 1, int(raw.black_level_per_channel[0]), True),  # BlackLevel
        (50717, 'H', 1, int(raw.white_level), True),        # WhiteLevel
        (271, 's', 0, 'Converted', True),                   # Make
        (272, 's', 0, name, True),                          # Model
    ]
    tifffile.imwrite(target, image, photometric=32803, byteorder=byte_order,
                     extratags=tags, rowsperstrip=64, metadata=None,
                     software=False)
    print(target, image.shape, 'cfa', cfa, 'as-shot neutral', neutral.round(4))


if __name__ == '__main__':
    main()
