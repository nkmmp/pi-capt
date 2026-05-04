# pi-capt

_Canon LBP-* driver for Raspberry PI for CUPS_

This driver initially based on code from https://aur.archlinux.org/packages/capt-src/ and from http://www.boichat.ch/nicolas/capt/

## Supported printers

| Model | Color | Notes |
|-------|-------|-------|
| Canon LBP810 / LBP1120 | Mono | Original pi-capt target |
| Canon LBP7010C | Color | Full CAPT 0x0300 implementation |

## Installation

### Dependencies

```bash
sudo apt install build-essential cups libcups2-dev libcupsimage2-dev ghostscript
```

### Build

```bash
git clone https://github.com/nkmmp/pi-capt.git
cd pi-capt/src/capt
make
sudo make install
```

`make` builds two binaries: `capt` (LBP810/LBP1120 mono) and `lbp7010c` (LBP7010C color).

### Register the printer (LBP7010C)

```bash
sudo lpadmin -p LBP7010C \
    -E \
    -v usb://Canon/LBP7010C \
    -P /usr/share/ppd/CNCUPSLBP7010CCAPTJ.ppd \
    -D "Canon LBP7010C"
```

For LBP810 / LBP1120, use `ppd/CNCUPSLBP810CAPTJ.ppd` and the `usb://Canon/LBP810` URI instead.

### Selecting a USB device

By default `capt` opens `/dev/usb/lp0`. To use a different port, set `CAPT_DEVICE` before printing or pass `-d` when calling `capt` directly:

```bash
# via environment variable (capt-print picks this up)
export CAPT_DEVICE=/dev/usb/lp1

# or pass directly
capt -d /dev/usb/lp1
```

### ccpd stub (required for job control commands)

```bash
gcc -O2 -o /usr/sbin/stub_ccpd stub_ccpd.c
sudo systemctl enable --now ccpd
```

A minimal `stub_ccpd` that accepts and discards JC-protocol commands is sufficient for normal printing.

## Notes

- No support, Experimental
