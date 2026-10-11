"""Draw the "Sync preview with editor" toolbar icon into every toolbar strip.

    python tools/toolbar/add_sync_icon.py

res/Toolbar{16,24,32,40,48}.bmp each hold one row of square icons, 32-bit with
straight (non-premultiplied) alpha, bottom-up, transparent background. Upstream
ships 27 icons. This draws icon 28 -- a chain link, the usual symbol for linked
scrolling -- at each size and writes it into slot 28.

Idempotent: it rewrites slot 28 whether or not it already exists, so the icon
can be redrawn after a tweak. Slot 28 must stay in step with tbbMainWnd in
src/Notepad4.cpp (iBitmap 27) and IDT_VIEW_MARKDOWN_SYNC in src/resource.h.
"""

import struct
from pathlib import Path

from PIL import Image, ImageDraw

SIZES = (16, 24, 32, 40, 48)
SLOT = 27                       # zero-based image index
UPSTREAM_ICONS = 27
BLUE = (0, 83, 156, 255)        # the exact blue of the stock save, undo and pin icons
SUPER = 512                     # drawing canvas; downsampled to each size

RES = Path(__file__).resolve().parents[2] / 'res'


def capsule(size, length, width, stroke, color):
    """An outlined rounded bar, horizontal, centred on a size x size canvas."""
    layer = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    x0 = (size - length) / 2
    y0 = (size - width) / 2
    draw.rounded_rectangle((x0, y0, x0 + length, y0 + width), radius=width / 2,
                           outline=color, width=stroke)
    return layer


def draw_icon(px):
    """Two interlocking links on a diagonal, drawn large, then downsampled."""
    canvas = Image.new('RGBA', (SUPER, SUPER), (0, 0, 0, 0))
    # Sized to fill the cell like the stock icons, with the links far enough
    # apart that they read as two interlocked links rather than one bar, and
    # strokes thick enough to survive the 16 px version.
    link = capsule(SUPER, length=330, width=170, stroke=52, color=BLUE)
    link = link.rotate(45, resample=Image.BICUBIC)
    offset = 84
    canvas.alpha_composite(link, (-offset, offset))
    canvas.alpha_composite(link, (offset, -offset))
    return canvas.resize((px, px), Image.LANCZOS)


def read_strip(path):
    data = path.read_bytes()
    pixel_offset = struct.unpack_from('<I', data, 10)[0]
    width, height = struct.unpack_from('<ii', data, 18)
    bpp = struct.unpack_from('<H', data, 28)[0]
    compression = struct.unpack_from('<I', data, 30)[0]
    if bpp != 32 or compression != 0 or height <= 0:
        raise SystemExit(f'{path.name}: expected a bottom-up 32-bit BI_RGB bitmap')
    raw = data[pixel_offset:pixel_offset + width * height * 4]
    image = Image.frombuffer('RGBA', (width, height), raw, 'raw', 'BGRA', 0, -1).copy()
    resolution = struct.unpack_from('<ii', data, 38)
    return image, resolution


def write_strip(path, image, resolution):
    width, height = image.size
    raw = image.tobytes('raw', 'BGRA', 0, -1)   # bottom-up, like the original
    info = struct.pack('<IiiHHIIiiII', 40, width, height, 1, 32, 0, len(raw),
                       resolution[0], resolution[1], 0, 0)
    header = struct.pack('<2sIHHI', b'BM', 14 + len(info) + len(raw), 0, 0, 14 + len(info))
    path.write_bytes(header + info + raw)


def main():
    for px in SIZES:
        path = RES / f'Toolbar{px}.bmp'
        strip, resolution = read_strip(path)
        icons = strip.width // px
        if icons < UPSTREAM_ICONS:
            raise SystemExit(f'{path.name}: only {icons} icons, expected at least {UPSTREAM_ICONS}')
        width = max(strip.width, (SLOT + 1) * px)
        out = Image.new('RGBA', (width, px), (0, 0, 0, 0))
        out.paste(strip.crop((0, 0, min(strip.width, SLOT * px), px)), (0, 0))
        out.paste(draw_icon(px), (SLOT * px, 0))
        write_strip(path, out, resolution)
        print(f'{path.name}: {width // px} icons')


if __name__ == '__main__':
    main()
