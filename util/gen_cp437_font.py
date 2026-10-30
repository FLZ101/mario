#!/usr/bin/env python3
"""Generate Mario's 8x16 CP437 framebuffer-console font."""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parent.parent
FONT_PATH = ROOT / "app/usr/libmario-ui/assets/fonts/DejaVuSansMono.ttf"
OUTPUT_PATH = ROOT / "src/video/font_cp437_8x16.inc"
FONT_SIZE = 13
CELL_WIDTH = 8
CELL_HEIGHT = 16
THRESHOLD = 96
CP437_CONTROL = (
    " ", "☺", "☻", "♥", "♦", "♣", "♠", "•", "◘", "○", "◙", "♂", "♀", "♪", "♫", "☼",
    "►", "◄", "↕", "‼", "¶", "§", "▬", "↨", "↑", "↓", "→", "←", "∟", "↔", "▲", "▼",
)


def cp437_character(code: int) -> str:
    if code < len(CP437_CONTROL):
        return CP437_CONTROL[code]
    if code == 0x7F:
        return "⌂"
    return bytes((code,)).decode("cp437")


def glyph_rows(font: ImageFont.FreeTypeFont, code: int) -> list[int]:
    image = Image.new("L", (CELL_WIDTH, CELL_HEIGHT))
    ImageDraw.Draw(image).text((0, 0), cp437_character(code), font=font,
                               fill=255)
    rows = []
    for y in range(CELL_HEIGHT):
        row = 0
        for x in range(CELL_WIDTH):
            if image.getpixel((x, y)) >= THRESHOLD:
                row |= 0x80 >> x
        rows.append(row)
    return rows


def main() -> None:
    font = ImageFont.truetype(FONT_PATH, FONT_SIZE)
    lines = [
        "/*",
        " * 8x16 CP437 bitmap generated from DejaVu Sans Mono 2.37.",
        " * Source: app/usr/libmario-ui/assets/fonts/DejaVuSansMono.ttf.",
        " * Generator: util/gen_cp437_font.py (13 px, threshold 96).",
        " * License: Bitstream Vera / DejaVu Fonts license; see assets/fonts/LICENSE-DejaVu.txt.",
        " */",
    ]
    for code in range(256):
        values = ", ".join(f"0x{value:02x}" for value in glyph_rows(font, code))
        lines.append(f"\t{{ {values}, }},")
    OUTPUT_PATH.write_text("\n".join(lines) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
