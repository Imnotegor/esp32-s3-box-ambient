"""Convert the headless LVGL RGB565 design snapshots to PNG using stdlib only."""
import struct
import zlib
from pathlib import Path

root = Path(__file__).resolve().parents[1] / "artifacts/dashboard-preview"


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


for index, name in enumerate(["overview", "server", "weather"]):
    raw = (root / ("page%d.raw" % index)).read_bytes()
    assert len(raw) == 320 * 240 * 2
    rows = bytearray()
    for y in range(240):
        rows.append(0)
        for x in range(320):
            value = struct.unpack_from("<H", raw, (y * 320 + x) * 2)[0]
            rows.extend((((value >> 11) & 31) * 255 // 31,
                         ((value >> 5) & 63) * 255 // 63,
                         (value & 31) * 255 // 31))
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 240, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(rows))
    png += chunk(b"IEND", b"")
    (root / (name + ".png")).write_bytes(png)
print("Rendered three LVGL design previews (synthetic fixtures, not live device evidence)")
