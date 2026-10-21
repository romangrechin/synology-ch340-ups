import struct
import sys
import zlib

BG = (0x1F, 0x29, 0x33, 0xFF)
WHITE = (0xF5, 0xF7, 0xFA, 0xFF)
GREEN = (0x3D, 0xDC, 0x84, 0xFF)
CLEAR = (0, 0, 0, 0)


def rounded(size, radius):
    """Return a predicate telling whether a pixel is inside a rounded square."""
    def inside(x, y):
        cx = min(max(x, radius), size - 1 - radius)
        cy = min(max(y, radius), size - 1 - radius)
        dx, dy = x - cx, y - cy
        return dx * dx + dy * dy <= radius * radius
    return inside


def make(size, path):
    s = size
    inside = rounded(s, max(1, round(s * 0.22)))
    px = [[BG if inside(x, y) else CLEAR for x in range(s)] for y in range(s)]

    def fill(x0, y0, x1, y1, col):
        for y in range(max(0, round(y0)), min(s, round(y1))):
            for x in range(max(0, round(x0)), min(s, round(x1))):
                px[y][x] = col

    t = max(2, round(s / 32))

    # battery body, drawn as four white bars so the middle stays dark
    bx0, bx1 = s * 0.17, s * 0.76
    by0, by1 = s * 0.34, s * 0.66
    fill(bx0, by0, bx1, by0 + t, WHITE)
    fill(bx0, by1 - t, bx1, by1, WHITE)
    fill(bx0, by0, bx0 + t, by1, WHITE)
    fill(bx1 - t, by0, bx1, by1, WHITE)

    # positive terminal on the right
    fill(bx1, s * 0.44, s * 0.84, s * 0.56, WHITE)

    # charge level inside the body
    pad = t + max(1, round(s / 32))
    fill(bx0 + pad, by0 + pad, bx0 + pad + (bx1 - bx0 - 2 * pad) * 0.62, by1 - pad, GREEN)

    raw = b"".join(
        b"\x00" + bytes(c for pixel in row for c in pixel) for row in px
    )

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    out = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", s, s, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )

    with open(path, "wb") as fh:
        fh.write(out)

    return len(out)


root = sys.argv[1]
for size, name in ((64, "PACKAGE_ICON.PNG"), (256, "PACKAGE_ICON_256.PNG")):
    n = make(size, "%s/%s" % (root, name))
    print("%s  %dx%d  %d bytes" % (name, size, size, n))
