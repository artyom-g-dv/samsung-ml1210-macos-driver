#!/bin/bash
# Build samsunggdi.pkg — a one-click macOS installer:
#   /usr/libexec/cups/filter/samsunggdi
#   /Library/Printers/PPDs/Contents/Resources/Samsung-ML-1210-gdi.ppd
#   postinstall creates/enables the ML-1210 CUPS queue when the printer
#   is on USB.
# Output: out/SamsungML1210Driver-<ver>.pkg
set -e
cd "$(dirname "$0")"

VER="${1:-1.0.0}"
OUT=out
ROOT="$OUT/pkgroot"
SCRIPTS="$OUT/scripts"
rm -rf "$OUT"
mkdir -p "$ROOT/usr/libexec/cups/filter" \
         "$ROOT/Library/Printers/PPDs/Contents/Resources" \
         "$SCRIPTS"

# universal binary (arm64 + x86_64)
clang -O2 -Wall -Wno-deprecated-declarations \
    -arch arm64 -arch x86_64 \
    -o "$ROOT/usr/libexec/cups/filter/samsunggdi" samsunggdi.c \
    -lcups -framework CoreGraphics
chmod 755 "$ROOT/usr/libexec/cups/filter/samsunggdi"

install -m 644 Samsung-ML-1210-gdi.ppd \
    "$ROOT/Library/Printers/PPDs/Contents/Resources/"

cat > "$SCRIPTS/postinstall" <<'EOS'
#!/bin/bash
# create/refresh the queue; silently skip if printer not on USB yet
PPD="/Library/Printers/PPDs/Contents/Resources/Samsung-ML-1210-gdi.ppd"
URI="$(lpinfo -v 2>/dev/null | awk 'tolower($0) ~ /usb.*samsung/ {print $2; exit}')"
[ -z "$URI" ] && URI="usb://Samsung/ML-1210"
lpadmin -p ML-1210 -E -v "$URI" -P "$PPD" -o printer-is-shared=false 2>/dev/null
cupsenable ML-1210 2>/dev/null
cupsaccept ML-1210 2>/dev/null
exit 0
EOS
chmod 755 "$SCRIPTS/postinstall"

rm -f "$OUT/samsunggdi-component.pkg" "$OUT/SamsungML1210Driver-$VER.pkg"

pkgbuild --root "$ROOT" \
    --scripts "$SCRIPTS" \
    --identifier "com.samsunggdi.ml1210" \
    --version "$VER" \
    --ownership recommended \
    "$OUT/samsunggdi-component.pkg"

productbuild --package "$OUT/samsunggdi-component.pkg" \
    "$OUT/SamsungML1210Driver-$VER.pkg"

echo "Built $OUT/SamsungML1210Driver-$VER.pkg"
