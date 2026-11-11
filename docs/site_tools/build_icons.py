#!/usr/bin/env python3
"""Render the images made from assets/favicon.svg: run it again whenever that file changes.

    python site_tools/build_icons.py [--browser PATH]

Writes favicon.ico (16, 32 and 48 pixels) at the site root, assets/apple-touch-icon.png (180 pixels, on the site's dark
background) and assets/og-card.png (the 1200x630 link preview, from og_card.html beside this file). A headless Chrome or
Edge draws the SVG and the card (--browser, else the usual install locations and PATH); Pillow resizes and packs them.
The card uses the site's web fonts when the machine is online and the local fallbacks in its stylesheet otherwise.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
SITE = HERE.parent / "site"
BACKGROUND = (11, 13, 19, 255)
PROGRAM_FOLDERS = [os.environ[name] for name in ("ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA") if name in os.environ]
BROWSERS = (
    [folder + "/Google/Chrome/Application/chrome.exe" for folder in PROGRAM_FOLDERS]
    + [folder + "/Microsoft/Edge/Application/msedge.exe" for folder in PROGRAM_FOLDERS]
    + ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"]
)
ON_PATH = ["google-chrome", "chromium", "chromium-browser", "chrome", "msedge"]


def find_browser(named):
    if named:
        return named
    for candidate in BROWSERS:
        if pathlib.Path(candidate).exists():
            return candidate
    for name in ON_PATH:
        found = shutil.which(name)
        if found:
            return found
    raise SystemExit("build_icons: no Chrome or Edge found; pass --browser PATH")


def screenshot(browser, page, width, height, target, scratch):
    """The page's top-left width x height pixels; the window is taller, since a headless window loses rows to its frame."""
    subprocess.run([browser, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    "--default-background-color=00000000", "--virtual-time-budget=8000",
                    "--user-data-dir=" + str(scratch / "profile"),
                    "--window-size=%d,%d" % (width, height + 200), "--screenshot=" + str(target), page.as_uri()],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
    image = Image.open(target).convert("RGBA")
    if image.width < width or image.height < height:
        raise SystemExit("build_icons: the browser drew %dx%d, short of %dx%d" % (image.width, image.height, width, height))
    return image.crop((0, 0, width, height))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--browser")
    args = ap.parse_args()
    browser = find_browser(args.browser)
    favicon = SITE / "assets" / "favicon.svg"
    if not favicon.exists():
        raise SystemExit("build_icons: %s is missing" % favicon)
    with tempfile.TemporaryDirectory() as folder:
        scratch = pathlib.Path(folder)
        page = scratch / "icon.html"
        page.write_text('<!DOCTYPE html><html><body style="margin:0"><img src="%s" width="512" height="512" '
                        'style="display:block"></body></html>' % favicon.as_uri(), encoding="utf-8")
        icon = screenshot(browser, page, 512, 512, scratch / "icon.png", scratch)
        icon.save(SITE / "favicon.ico", sizes=[(16, 16), (32, 32), (48, 48)])
        touch = Image.new("RGBA", (180, 180), BACKGROUND)
        small = icon.resize((180, 180), Image.LANCZOS)
        touch.alpha_composite(small)
        touch.convert("RGB").save(SITE / "assets" / "apple-touch-icon.png", optimize=True)
        card = screenshot(browser, HERE / "og_card.html", 1200, 630, scratch / "card.png", scratch)
        flat = Image.new("RGBA", card.size, BACKGROUND)
        flat.alpha_composite(card)
        flat.convert("RGB").save(SITE / "assets" / "og-card.png", optimize=True)
    print("icons: favicon.ico, assets/apple-touch-icon.png, assets/og-card.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
