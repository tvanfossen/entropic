"""Generate a minimal test PNG image for unit tests.

Usage:
    .venv/bin/python scripts/generate_test_image.py [width] [height] [output_path]

Generates a solid-color PNG with no external dependencies using
raw zlib + PNG chunk encoding.
"""

import struct
import sys
import zlib


def create_png(width: int, height: int, r: int, g: int, b: int) -> bytes:
    """Create a minimal RGB PNG in memory."""

    def chunk(chunk_type: bytes, data: bytes) -> bytes:
        raw = chunk_type + data
        return struct.pack(">I", len(data)) + raw + struct.pack(">I", zlib.crc32(raw) & 0xFFFFFFFF)

    ihdr_data = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    row = bytes([0] + [r, g, b] * width)
    raw_data = row * height
    idat_data = zlib.compress(raw_data)

    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr_data)
        + chunk(b"IDAT", idat_data)
        + chunk(b"IEND", b"")
    )


def main() -> None:
    width = int(sys.argv[1]) if len(sys.argv) > 1 else 100
    height = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    output = sys.argv[3] if len(sys.argv) > 3 else "tests/data/test_image_100x100.png"

    data = create_png(width, height, 128, 64, 32)
    with open(output, "wb") as f:
        f.write(data)
    print(f"Generated {width}x{height} PNG: {output} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
