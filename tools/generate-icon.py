#!/usr/bin/env python3
"""Regenerate Bolt's icon set from assets/icon.png.

The master artwork is user-supplied. Pillow is required.
"""
from pathlib import Path
from PIL import Image
import base64

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "assets"
MASTER = OUTPUT / "icon.png"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)

def main():
    image = Image.open(MASTER).convert("RGBA")
    image.save(OUTPUT / "bolt.png", optimize=True)
    resized = {s: image.resize((s, s), Image.Resampling.LANCZOS) for s in SIZES}
    resized[256].save(OUTPUT / "bolt-256.png", optimize=True)
    resized[256].save(OUTPUT / "bolt.ico", sizes=[(s, s) for s in SIZES])
    image.save(OUTPUT / "bolt.icns")
    data = base64.b64encode((OUTPUT / "bolt-256.png").read_bytes()).decode("ascii")
    (OUTPUT / "bolt.svg").write_text(
        '<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256" role="img" aria-label="Bolt">\n'
        f'  <image width="256" height="256" href="data:image/png;base64,{data}"/>\n</svg>\n',
        encoding="utf-8",
    )
    print(f"Generated Bolt icons from {MASTER}")

if __name__ == "__main__":
    main()
