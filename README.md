# samsunggdi — Samsung ML-1210 driver for macOS

A working CUPS driver for the Samsung ML-1210 (and ML-1000/1200-series
SmartGDI printers) on modern macOS, including Apple Silicon.

The ML-1210 is a host-based GDI printer that speaks Samsung's "SMART"
language: a PJL wrapper around MODITIFF-compressed 128-line raster bands.
`samsunggdi` is a small self-contained CUPS filter that renders PDF pages
with CoreGraphics at 600dpi and emits that stream. The wire-format logic
is a port of Samsung's `gdevgdi.c` Ghostscript device (GPL).

## Install

**Easy way:** download `SamsungML1210Driver-*.pkg` from
[Releases](../../releases), open it, follow the installer. The package is
unsigned, so macOS may warn — right-click → Open, or:

```sh
sudo installer -pkg SamsungML1210Driver-1.0.0.pkg -target /
```

The installer drops the filter + PPD and creates the `ML-1210` queue
whenever the printer is plugged into USB.

**From source:**

```sh
cd driver
./install.sh          # builds the filter, installs it + the PPD, adds queue
```

Then print normally:

```sh
lp -d ML-1210 file.pdf
```

or use "ML-1210" in any print dialog.

## How it works

- `driver/samsunggdi.c` — CUPS filter. Accepts `application/pdf`
  (rendered in-process via CoreGraphics — the macOS filter sandbox forbids
  spawning external renderers), `application/vnd.cups-raster`, and raw
  `image/x-portable-bitmap`. Output: `ESC%-12345X` + PJL header,
  per-band headers + MODITIFF data, `$PJL EOJ` trailer. Letter/A4/Legal,
  600/300dpi.
- `driver/Samsung-ML-1210-gdi.ppd` — queue definition; the single
  `application/pdf 100 samsunggdi` filter entry takes PDFs straight to
  the printer.
- `driver/install.sh` — installs to `/usr/libexec/cups/filter/`, creates
  the `ML-1210` queue on the USB URI reported by `lpinfo -v`.

## Requirements

- macOS with CUPS (tested on CUPS 2.3.4, arm64)
- Command Line Tools (clang + cups/raster.h SDK headers) for building
- Printer on USB

## License

GPL-2.0 — the compression/stream code is derived from Samsung's
GPL-licensed `gdevgdi.c`.
