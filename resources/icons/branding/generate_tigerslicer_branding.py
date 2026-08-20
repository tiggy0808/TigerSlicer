#!/usr/bin/env python3
"""Deterministic TigerSlicer branding asset generator (Phase U8).

Renders the canonical vector marks

    TigerSlicer.svg              (main application)
    TigerSlicer-gcodeviewer.svg  (G-code viewer)

into every raster/icon derivative shipped in this directory. The canonical
brand color is pink #F6A5C1 (246,165,193); both semicircular elements sit
on a transparent background so the mark reads cleanly on any theme.

Requirements (developer tooling, not needed at build time):
    pip install svglib reportlab pillow

The renderer has no native rasterizer with alpha, so each mark is rendered
twice (over black and over white) and per-pixel alpha is recovered from the
difference. This keeps antialiased edges clean without halos and is fully
deterministic for identical inputs.

Run from this directory:  python generate_tigerslicer_branding.py
"""

import os

from PIL import Image, ImageOps
from reportlab.graphics import renderPM
from svglib.svglib import svg2rlg

HERE = os.path.dirname(os.path.abspath(__file__))
SUPERSAMPLE_PX = 1024  # master render size; every target is a downscale

ICO_SIZES = [16, 24, 32, 48, 64, 128, 256]
ICNS_SIZES = [16, 32, 64, 128, 256, 512, 1024]


def render_master(svg_name: str) -> Image.Image:
    """Render an SVG to an RGBA master image with recovered alpha."""

    def render_on(background: int) -> Image.Image:
        drawing = svg2rlg(os.path.join(HERE, svg_name))
        scale = SUPERSAMPLE_PX / max(drawing.width, drawing.height)
        drawing.scale(scale, scale)
        drawing.width = drawing.height = SUPERSAMPLE_PX
        return renderPM.drawToPIL(drawing, dpi=72, bg=background).convert("RGB")

    on_black = render_on(0x000000)
    on_white = render_on(0xFFFFFF)

    master = Image.new("RGBA", on_black.size)
    black_px = on_black.load()
    white_px = on_white.load()
    out_px = master.load()
    for y in range(master.height):
        for x in range(master.width):
            rb, gb, bb = black_px[x, y]
            rw, gw, bw = white_px[x, y]
            # alpha = 255 - (white_render - black_render), channel-averaged
            alpha = 255 - ((rw - rb) + (gw - gb) + (bw - bb)) // 3
            alpha = max(0, min(255, alpha))
            if alpha == 0:
                out_px[x, y] = (0, 0, 0, 0)
            else:
                # un-premultiply from the black render
                out_px[x, y] = (min(255, rb * 255 // alpha),
                                min(255, gb * 255 // alpha),
                                min(255, bb * 255 // alpha),
                                alpha)
    return master


def sized(master: Image.Image, px: int) -> Image.Image:
    return master.resize((px, px), Image.LANCZOS)


def save_png(image: Image.Image, name: str) -> None:
    image.save(os.path.join(HERE, name), format="PNG", optimize=True)
    print(f"  {name}  {image.size[0]}x{image.size[1]}")


def generate(app_svg: str, base: str) -> None:
    print(f"{app_svg} ->")
    master = render_master(app_svg)

    save_png(sized(master, 154) if base == "TigerSlicer" else sized(master, 192),
             f"{base}.png" if base == "TigerSlicer" else f"{base}_192px.png")
    if base == "TigerSlicer":
        save_png(sized(master, 192), f"{base}_192px.png")
    save_png(sized(master, 128), f"{base}_128px.png")
    save_png(sized(master, 32), f"{base}_32px.png")
    save_png(sized(master, 128), f"{base}-mac_128px.png")

    # Pillow derives every entry from the base image, which must be the
    # largest requested size; smaller frames come from append_images.
    ico = [sized(master, px) for px in sorted(ICO_SIZES, reverse=True)]
    ico[0].save(os.path.join(HERE, f"{base}.ico"), format="ICO",
                sizes=[(px, px) for px in ICO_SIZES],
                append_images=ico[1:])
    print(f"  {base}.ico  sizes {ICO_SIZES}")

    icns = [sized(master, px) for px in ICNS_SIZES]
    icns[-1].save(os.path.join(HERE, f"{base}.icns"), format="ICNS",
                  append_images=icns[:-1])
    print(f"  {base}.icns  sizes {ICNS_SIZES}")

    if base == "TigerSlicer":
        grayscale = ImageOps.grayscale(sized(master, 192)).convert("RGBA")
        grayscale.putalpha(sized(master, 192).getchannel("A"))
        save_png(grayscale, f"{base}_192px_grayscale.png")
        # Kept for callers that reference the historical "_transparent" name;
        # since the disc was dropped, every variant is transparent and this is
        # identical to the regular 192px render.
        save_png(sized(master, 192), f"{base}_192px_transparent.png")


def main() -> None:
    generate("TigerSlicer.svg", "TigerSlicer")
    generate("TigerSlicer-gcodeviewer.svg", "TigerSlicer-gcodeviewer")


if __name__ == "__main__":
    main()
