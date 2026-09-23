"""Render monochrome Kindle controls at the same optical weight."""

from math import cos, pi, sin
from pathlib import Path

from PIL import Image, ImageDraw

SIZE = 64
SCALE = 4
INK = (51, 53, 53, 255)
STROKE = 3.5
OUTPUT = Path(__file__).resolve().parents[1] / "client/share/icons"


def line(draw, points, width=STROKE):
    scaled = [(round(x * SCALE), round(y * SCALE)) for x, y in points]
    thickness = round(width * SCALE)
    draw.line(scaled, fill=INK, width=thickness, joint="curve")
    radius = thickness / 2
    for x, y in (scaled[0], scaled[-1]):
        draw.ellipse((round(x - radius), round(y - radius),
                      round(x + radius), round(y + radius)), fill=INK)


def circle(draw, box, width=STROKE):
    draw.ellipse(tuple(round(value * SCALE) for value in box),
                 outline=INK, width=round(width * SCALE))


def draw_icon(name, draw):
    if name == "home":
        line(draw, [(10, 30), (32, 11), (54, 30)])
        line(draw, [(17, 25), (17, 52), (47, 52), (47, 25)])
        line(draw, [(27, 52), (27, 36), (37, 36), (37, 52)])
    elif name == "search":
        circle(draw, (12, 12, 41, 41))
        line(draw, [(39, 39), (52, 52)])
    elif name == "settings":
        # Eight evenly spaced teeth, with a hollow center like the other icons.
        points = []
        for tooth in range(8):
            center = -pi / 2 + tooth * pi / 4
            for degrees, radius in ((-20, 17), (-12, 17), (-10, 21),
                                    (10, 21), (12, 17), (20, 17)):
                angle = center + degrees * pi / 180
                points.append((32 + radius * cos(angle), 32 + radius * sin(angle)))
        line(draw, points + [points[0]])
        circle(draw, (26, 26, 38, 38))
    elif name == "help":
        circle(draw, (11, 11, 53, 53))
        line(draw, [(25, 27), (25, 24), (27, 21), (30, 19), (34, 19),
                    (38, 21), (40, 24), (40, 27), (38, 30), (34, 33),
                    (32, 36), (32, 39)])
        draw.ellipse(tuple(round(value * SCALE) for value in (30, 44, 34, 48)), fill=INK)
    elif name == "close":
        line(draw, [(15, 15), (49, 49)])
        line(draw, [(49, 15), (15, 49)])
    elif name in ("previous", "next"):
        points = [(39, 15), (22, 32), (39, 49)]
        if name == "next":
            points = [(64 - x, y) for x, y in points]
        line(draw, points)
    elif name in ("first", "last"):
        for offset in (0, 16):
            points = [(40 - offset, 15), (23 - offset, 32), (40 - offset, 49)]
            if name == "last":
                points = [(64 - x, y) for x, y in points]
            line(draw, points)


def main():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    for name in ("home", "search", "settings", "help", "close", "first", "previous", "next", "last"):
        image = Image.new("RGBA", (SIZE * SCALE, SIZE * SCALE), (0, 0, 0, 0))
        draw_icon(name, ImageDraw.Draw(image))
        image.resize((SIZE, SIZE), Image.Resampling.LANCZOS).save(OUTPUT / f"{name}.png")


if __name__ == "__main__":
    main()
