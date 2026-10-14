#!/bin/bash
# Install the ML-1210 CUPS driver (samsunggdi filter + PPD) and create
# the print queue. Run: ./install.sh
set -e
cd "$(dirname "$0")"

if [ ! -x samsunggdi ] || [ samsunggdi.c -nt samsunggdi ]; then
    clang -O2 -Wall -Wno-deprecated-declarations -o samsunggdi samsunggdi.c \
        -lcups -framework CoreGraphics
fi

sudo install -o root -g wheel -m 755 samsunggdi /usr/libexec/cups/filter/samsunggdi

URI="$(lpinfo -v 2>/dev/null | awk 'tolower($0) ~ /usb.*samsung/ {print $2; exit}')"
if [ -z "$URI" ]; then
    URI="usb://Samsung/ML-1210"
    echo "! printer not detected via lpinfo; using generic URI: $URI"
    echo "  if printing fails, run lpinfo -v with the printer connected and:"
    echo "  sudo lpadmin -p ML-1210 -v <real-usb-uri>"
fi

sudo lpadmin -p ML-1210 -E -v "$URI" -P "$PWD/Samsung-ML-1210-gdi.ppd" \
    -o printer-is-shared=false
sudo cupsenable ML-1210
sudo cupsaccept ML-1210

echo "Installed queue 'ML-1210' -> $URI"
echo "Test: lp -d ML-1210 /etc/cups/cups-files.conf   (or any text/PDF file)"
