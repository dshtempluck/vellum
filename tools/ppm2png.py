"""Convert the probe's PPM output to PNG for inspection.

Usage: python ppm2png.py in.ppm out.png [max_width]
"""
import struct, sys, zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"P6":
        raise SystemExit("not a P6 PPM")
    # header: P6, width, height, maxval -- whitespace separated
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while data[i : i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _ = fields
    return w, h, data[i : i + w * h * 3]


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3 : (y + 1) * w * 3] for y in range(h))

    def chunk(tag, payload):
        return (
            struct.pack(">I", len(payload))
            + tag
            + payload
            + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 6))
    png += chunk(b"IEND", b"")
    open(path, "wb").write(png)


def box_downscale(w, h, rgb, max_w):
    if w <= max_w:
        return w, h, rgb
    n = (w + max_w - 1) // max_w          # integer box factor
    ow, oh = w // n, h // n
    out = bytearray(ow * oh * 3)
    for y in range(oh):
        for x in range(ow):
            acc = [0, 0, 0]
            for dy in range(n):
                base = ((y * n + dy) * w + x * n) * 3
                for dx in range(n):
                    p = base + dx * 3
                    acc[0] += rgb[p]
                    acc[1] += rgb[p + 1]
                    acc[2] += rgb[p + 2]
            k = n * n
            o = (y * ow + x) * 3
            out[o] = acc[0] // k
            out[o + 1] = acc[1] // k
            out[o + 2] = acc[2] // k
    return ow, oh, bytes(out)


src, dst = sys.argv[1], sys.argv[2]
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 0
w, h, rgb = read_ppm(src)
if limit:
    w, h, rgb = box_downscale(w, h, rgb, limit)
write_png(dst, w, h, rgb)
print(f"{dst}: {w}x{h}")
