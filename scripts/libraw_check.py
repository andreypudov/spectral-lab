#!/usr/bin/env python3
"""Development tool: decode Spectral Lab output with LibRaw and render a PNG.

LibRaw is an independent DNG decoder. A successful decode checks the tile
layout, lossless JPEG streams and tags; the PNG (neutral rendering, no tone
curve, as-shot white balance) is for visual inspection.

Usage: libraw_check.py <output.dng>...
Requires: pip install rawpy numpy imageio
"""
import sys

import imageio.v3 as iio
import rawpy


def main():
    for path in sys.argv[1:]:
        with rawpy.imread(path) as raw:
            print(path, 'type', raw.raw_type, 'colors', raw.num_colors,
                  'as-shot wb', raw.camera_whitebalance[:3],
                  'white', raw.white_level)
            rgb = raw.postprocess(use_camera_wb=True, no_auto_bright=True,
                                  output_bps=8, half_size=True)
        png = path.rsplit('.', 1)[0] + '.png'
        iio.imwrite(png, rgb)
        print('  wrote', png, rgb.shape,
              'mean rgb', rgb.reshape(-1, 3).mean(0).round(1))


if __name__ == '__main__':
    main()
