"""Generate Mario's Unicode bitmap extension for the framebuffer console.

btop and other terminal applications send UTF-8 box drawing, block elements,
and Braille patterns.  The framebuffer console font is a fixed CP437 bitmap,
so this generator renders the small Unicode ranges those programs use into the
same 18x32 grayscale format and embeds them as an extension table.
"""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parent.parent
FONT_PATH = ROOT / "app/gui/libmario-ui/assets/fonts/DejaVuSansMono.ttf"
OUTPUT_PATH = ROOT / "src/video/font_unicode_18x32_gray.inc"
FONT_SIZE = 26
BASE_WIDTH = 16
BASE_HEIGHT = 32
GLYPH_WIDTH = 18
GLYPH_HEIGHT = 32

# Codepoint ranges stored contiguously in the generated table.  The order
# must match the lookup in src/video/fbcon.c.
RANGES = (
    (0x00A0, 0x0100),  # Latin-1 punctuation and symbols
    (0x2500, 0x2580),  # Box drawing
    (0x2580, 0x25A0),  # Block elements
    (0x2800, 0x2900),  # Braille patterns
    (0x2190, 0x2200),  # Arrows
    (0x25A0, 0x2600),  # Geometric shapes
)


def glyph_rows(font: ImageFont.FreeTypeFont, codepoint: int) -> list[list[int]]:
    image = Image.new("L", (BASE_WIDTH, BASE_HEIGHT))
    ImageDraw.Draw(image).text((0, 0), chr(codepoint), font=font, fill=255)
    rows = []
    for y in range(BASE_HEIGHT):
        row = [image.getpixel((x, y)) for x in range(BASE_WIDTH)]
        if 0x2500 <= codepoint < 0x2580:
            row += [
                image.getpixel((BASE_WIDTH - 2, y)),
                image.getpixel((BASE_WIDTH - 1, y)),
            ]
        else:
            row += [0, 0]
        rows.append(row)
    return rows


def main() -> None:
    font = ImageFont.truetype(FONT_PATH, FONT_SIZE)
    lines = [
        "/*",
        " * 18x32 grayscale Unicode bitmap generated from DejaVu Sans Mono 2.37.",
        " * Source: app/gui/libmario-ui/assets/fonts/DejaVuSansMono.ttf.",
        " * Generator: util/gen_unicode_font.py (26 px, 256 levels).",
        " * License: Bitstream Vera / DejaVu Fonts license; see assets/fonts/LICENSE-DejaVu.txt.",
        " *",
        " * Range layout: Latin-1 0x00a0-0x00ff, box drawing 0x2500-0x257f,",
        " * block elements 0x2580-0x259f, braille 0x2800-0x28ff, arrows",
        " * 0x2190-0x21ff, and geometric shapes 0x25a0-0x25ff.",
        " */",
    ]
    for start, end in RANGES:
        for codepoint in range(start, end):
            lines.append("\t{")
            for row in glyph_rows(font, codepoint):
                values = ", ".join(f"0x{value:02x}" for value in row)
                lines.append(f"\t\t{{ {values} }},")
            lines.append("\t},")
    OUTPUT_PATH.write_text("\n".join(lines) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
