"""Generate Mario's supersampled CP437 framebuffer-console font."""

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parent.parent
FONT_PATH = ROOT / "app/gui/libmario-ui/assets/fonts/DejaVuSansMono.ttf"
OUTPUT_PATH = ROOT / "src/video/font_cp437_18x32_gray.inc"
FONT_SIZE = 26
BASE_WIDTH = 16
BASE_HEIGHT = 32
GLYPH_WIDTH = 18
GLYPH_HEIGHT = 32
CP437_CONTROL = (
    " ",
    "☺",
    "☻",
    "♥",
    "♦",
    "♣",
    "♠",
    "•",
    "◘",
    "○",
    "◙",
    "♂",
    "♀",
    "♪",
    "♫",
    "☼",
    "►",
    "◄",
    "↕",
    "‼",
    "¶",
    "§",
    "▬",
    "↨",
    "↑",
    "↓",
    "→",
    "←",
    "∟",
    "↔",
    "▲",
    "▼",
)


def cp437_character(code: int) -> str:
    if code < len(CP437_CONTROL):
        return CP437_CONTROL[code]
    if code == 0x7F:
        return "⌂"
    return bytes((code,)).decode("cp437")


def glyph_rows(font: ImageFont.FreeTypeFont, code: int) -> list[list[int]]:
    image = Image.new("L", (BASE_WIDTH, BASE_HEIGHT))
    ImageDraw.Draw(image).text((0, 0), cp437_character(code), font=font, fill=255)
    rows = []
    for y in range(BASE_HEIGHT):
        row = [image.getpixel((x, y)) for x in range(BASE_WIDTH)]
        if 0xC0 <= code <= 0xDF:
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
        " * 18x32 grayscale CP437 bitmap generated from DejaVu Sans Mono 2.37.",
        " * Source: app/gui/libmario-ui/assets/fonts/DejaVuSansMono.ttf.",
        " * Generator: util/gen_cp437_font.py (26 px, 256 levels).",
        " * License: Bitstream Vera / DejaVu Fonts license; see assets/fonts/LICENSE-DejaVu.txt.",
        " */",
    ]
    for code in range(256):
        lines.append("\t{")
        for row in glyph_rows(font, code):
            values = ", ".join(f"0x{value:02x}" for value in row)
            lines.append(f"\t\t{{ {values} }},")
        lines.append("\t},")
    OUTPUT_PATH.write_text("\n".join(lines) + "\n", encoding="ascii")


if __name__ == "__main__":
    main()
