#!/usr/bin/env python3
"""Prints the pairing link for the web UI, built from API_TOKEN in include/secrets.h.

    python scripts/pair_link.py                 -> http://visualizer.local/#token=...
    python scripts/pair_link.py 192.168.1.82    -> http://192.168.1.82/#token=...
    python scripts/pair_link.py --qr            -> also draws a QR code (needs: pip install qrcode)

Open the link once in a browser and the page saves the token there, so it never has to be typed.
The part after the "#" is not sent to the board and is removed from the address bar on arrival.
Treat the link like a password: anyone who has it can change the lights.
"""
import re
import sys
from pathlib import Path

secrets = Path(__file__).resolve().parent.parent / "include" / "secrets.h"
try:
    text = secrets.read_text(encoding="utf-8")
except OSError:
    sys.exit("Cannot read include/secrets.h")

m = re.search(r'^\s*#define\s+API_TOKEN\s+"([^"]+)"', text, re.M)
if not m:
    sys.exit("API_TOKEN is not defined in include/secrets.h")

args = [a for a in sys.argv[1:] if not a.startswith("--")]
host = args[0] if args else "visualizer.local"
link = f"http://{host}/#token={m.group(1)}"
print(link)

if "--qr" in sys.argv:
    try:
        import qrcode
    except ImportError:
        sys.exit("For a QR code run:  pip install qrcode")
    qr = qrcode.QRCode(border=1)
    qr.add_data(link)
    qr.print_ascii(invert=True)
