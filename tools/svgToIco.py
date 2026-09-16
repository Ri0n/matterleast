#!/usr/bin/env python3

import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path


def usage():
    print(f'Usage: {Path(sys.argv[0]).name} "16 32 48 256" input.svg', file=sys.stderr)


def dib_frame(raw_rgba, size):
    expected = size * size * 4
    if len(raw_rgba) != expected:
        raise RuntimeError(f'expected {expected} RGBA bytes for {size}x{size}, got {len(raw_rgba)}')

    xor = bytearray()
    mask_stride = ((size + 31) // 32) * 4
    and_mask = bytearray()

    for y in range(size - 1, -1, -1):
        mask_row = bytearray(mask_stride)
        for x in range(size):
            i = (y * size + x) * 4
            r, g, b, a = raw_rgba[i:i + 4]
            xor.extend((b, g, r, a))
            if a == 0:
                mask_row[x // 8] |= 0x80 >> (x % 8)
        and_mask.extend(mask_row)

    bitmap_info_header = struct.pack(
        '<IiiHHIIiiII',
        40,
        size,
        size * 2,
        1,
        32,
        0,
        len(xor),
        0,
        0,
        0,
        0,
    )
    return bitmap_info_header + xor + and_mask


def main():
    if len(sys.argv) != 3:
        usage()
        return 1

    try:
        sizes = [int(value) for value in sys.argv[1].split()]
    except ValueError:
        usage()
        return 1

    if not sizes or any(size < 1 or size > 256 for size in sizes):
        raise SystemExit('icon sizes must be between 1 and 256 pixels')

    src = Path(sys.argv[2])
    if not src.is_file():
        raise SystemExit(f"source file '{src}' does not exist")
    dst = src.with_suffix('.ico')

    inkscape = shutil.which('inkscape')
    converter = shutil.which('magick') or shutil.which('convert')
    if not inkscape:
        raise SystemExit('inkscape is required')
    if not converter:
        raise SystemExit('ImageMagick (magick or convert) is required')

    frames = []
    with tempfile.TemporaryDirectory(prefix='svg-to-ico-') as tmpdir:
        tmp = Path(tmpdir)
        for size in sizes:
            png = tmp / f'{size}.png'
            raw = tmp / f'{size}.rgba'
            subprocess.run(
                [inkscape, '-w', str(size), '-h', str(size), '-o', str(png), str(src)],
                check=True,
            )
            subprocess.run(
                [converter, str(png), '-depth', '8', f'rgba:{raw}'],
                check=True,
            )
            frames.append((size, dib_frame(raw.read_bytes(), size)))

    header = struct.pack('<HHH', 0, 1, len(frames))
    offset = len(header) + 16 * len(frames)
    entries = []
    for size, frame in frames:
        width = 0 if size == 256 else size
        entries.append(struct.pack('<BBBBHHII', width, width, 0, 0, 1, 32, len(frame), offset))
        offset += len(frame)

    with dst.open('wb') as output:
        output.write(header)
        for entry in entries:
            output.write(entry)
        for _, frame in frames:
            output.write(frame)

    print(f'created {dst} with sizes: {" ".join(map(str, sizes))}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
