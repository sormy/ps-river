"""A shape from a PS4 texture (GNF: BC7, micro tiled) as a white picture (PNG): white where the
texture is opaque, see-through where it is not. For flat play's pictures taken from a game's own
files when it starts (see play.sh). Only the texture's alpha is read: BC7's modes that carry it
in one subset (4 to 6) or not at all (0 to 3, opaque); mode 7 is refused.

    gnf_picture.py TEXTURE.gnf OUT.png
"""

import struct
import sys
import zlib
from pathlib import Path

BLOCK = 16  # bytes in a BC7 block, 4 x 4 pixels
TILE = 8  # blocks across a micro tile (Thin_1dThin)
MICRO_TILED = 13
BC7 = 41
# Where each of a micro tile's 64 elements lies in it (shadPS4's detilers/micro_128bpp.comp):
# nibble pairs, column high, row low.
INVERSE_MORTON = [
    0x11011000, 0x31213020, 0x13031202, 0x33233222, 0x51415040, 0x71617060, 0x53435242, 0x73637262,
    0x15051404, 0x35253424, 0x17071606, 0x37273626, 0x55455444, 0x75657464, 0x57475646, 0x77677666,
]  # fmt: skip
WEIGHTS = {
    2: [0, 21, 43, 64],
    3: [0, 9, 18, 27, 37, 46, 55, 64],
    4: [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64],
}


class Bits:
    """A BC7 block's bits, read from the lowest."""

    def __init__(self, block: bytes) -> None:
        self.value = int.from_bytes(block, "little")
        self.at = 0

    def take(self, count: int) -> int:
        taken = (self.value >> self.at) & ((1 << count) - 1)
        self.at += count
        return taken


def widen(value: int, bits: int) -> int:
    """An endpoint of so many bits to 8."""
    value <<= 8 - bits
    return value | (value >> bits)


def mix(low: int, high: int, weight: int) -> int:
    return ((64 - weight) * low + weight * high + 32) >> 6


def indices(bits: Bits, width: int) -> list[int]:
    """Sixteen indices of `width` bits, the first (the anchor) one bit shorter."""
    return [bits.take(width - 1 if pixel == 0 else width) for pixel in range(16)]


def block_alpha(block: bytes) -> list[int]:
    """The 16 alphas (0 to 255) of a BC7 block."""
    mode = next((m for m in range(8) if block[0] >> m & 1), 8)
    if mode < 4:
        return [255] * 16
    if mode == 7 or mode == 8:
        raise ValueError(f"BC7 mode {mode} is not read")
    bits = Bits(block)
    bits.take(mode + 1)
    if mode == 6:
        bits.take(6 * 7)  # the colour's endpoints
        alpha = [bits.take(7) for _ in range(2)]
        alpha = [a << 1 | bits.take(1) for a in alpha]  # each with its own last bit
        return [mix(alpha[0], alpha[1], WEIGHTS[4][i]) for i in indices(bits, 4)]
    rotation = bits.take(2)
    swapped = mode == 4 and bits.take(1) == 1
    colour_bits, alpha_bits = (5, 6) if mode == 4 else (7, 8)
    colour = [[widen(bits.take(colour_bits), colour_bits) for _ in range(2)] for _ in range(3)]
    alpha = [widen(bits.take(alpha_bits), alpha_bits) for _ in range(2)]
    second_width = 3 if mode == 4 else 2
    colour_at, alpha_at = (2, indices(bits, 2)), (second_width, indices(bits, second_width))
    if swapped:
        colour_at, alpha_at = alpha_at, colour_at
    # Rotated: what is stored as one colour channel is the alpha.
    (width, at), ends = (alpha_at, alpha) if rotation == 0 else (colour_at, colour[rotation - 1])
    return [mix(ends[0], ends[1], WEIGHTS[width][i]) for i in at]


def read_alpha(path: Path) -> tuple[int, int, bytearray]:
    data = path.read_bytes()
    if data[:4] != b"GNF ":
        raise ValueError(f"{path}: not a GNF texture")
    content = struct.unpack_from("<I", data, 4)[0]
    word1, word2, word3 = struct.unpack_from("<3I", data, 20)
    if (word1 >> 20) & 0x3F != BC7 or (word3 >> 20) & 0x1F != MICRO_TILED:
        raise ValueError(f"{path}: not a micro tiled BC7 texture")
    width, height = (word2 & 0x3FFF) + 1, ((word2 >> 14) & 0x3FFF) + 1
    pixels = data[8 + content :]
    across, down = width // 4, height // 4
    tiles_across = max(across // TILE, 1)
    alpha = bytearray(width * height)
    for element in range(across * down):
        tile, k = divmod(element, TILE * TILE)
        place = (INVERSE_MORTON[k >> 2] >> (8 * (k % 4))) & 0xFF
        x = (tile % tiles_across) * TILE + (place >> 4)
        y = (tile // tiles_across) * TILE + (place & 0xF)
        block = pixels[element * BLOCK : (element + 1) * BLOCK]
        for pixel, value in enumerate(block_alpha(block)):
            alpha[(y * 4 + pixel // 4) * width + x * 4 + pixel % 4] = value
    return width, height, alpha


def write_png(path: Path, width: int, height: int, alpha: bytearray) -> None:
    """White, as opaque as `alpha` says."""
    rows = b"".join(
        b"\x00" + b"".join(b"\xff\xff\xff" + bytes([a]) for a in alpha[y * width : (y + 1) * width])
        for y in range(height)
    )

    def chunk(kind: bytes, body: bytes) -> bytes:
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body))

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
    )


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    width, height, alpha = read_alpha(Path(sys.argv[1]))
    write_png(Path(sys.argv[2]), width, height, alpha)


if __name__ == "__main__":
    main()
