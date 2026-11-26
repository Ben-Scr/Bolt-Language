#!/usr/bin/env python3
"""Regenerate Bolt source-file icons from assets/fileIcon.png.

The master artwork is user-supplied. Pillow is required.
"""
from pathlib import Path
from PIL import Image
import base64

ROOT = Path(__file__).resolve().parents[1]
OUTPUT = ROOT / "assets"
MASTER = OUTPUT / "fileIcon.png"
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)

def main():
    image = Image.open(MASTER).convert("RGBA")
    resized = {s: image.resize((s, s), Image.Resampling.LANCZOS) for s in SIZES}
    resized[16].save(OUTPUT / "bolt-file-16.png", optimize=True)
    resized[32].save(OUTPUT / "bolt-file-32.png", optimize=True)
    resized[256].save(OUTPUT / "bolt-file.png", optimize=True)
    resized[256].save(OUTPUT / "bolt-file.ico", sizes=[(s, s) for s in SIZES])
    image.save(OUTPUT / "bolt-file.icns")
    data = base64.b64encode((OUTPUT / "bolt-file.png").read_bytes()).decode("ascii")
    (OUTPUT / "bolt-file.svg").write_text(
        '<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="0 0 32 32" role="img" aria-label="Bolt source file">\n'
        f'  <image width="32" height="32" href="data:image/png;base64,{data}"/>\n</svg>\n',
        encoding="utf-8",
    )
    ide = ROOT / "editors/intellij/src/main/resources/icons"
    ide.mkdir(parents=True, exist_ok=True)
    resized[16].save(ide / "bolt.png", optimize=True)
    print(f"Generated Bolt file icons from {MASTER}")

if __name__ == "__main__":
    main()
