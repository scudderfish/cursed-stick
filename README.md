# Cursed Gotek WiFi stick



This came out of a cursed idea I had a couple of days ago https://weird.autos/@scudderfish/117366678543083688  I can't do USB OTG with my desktop, but I can with an ESP32.

The aim of this is to present an ADF collection to a Gotek floppy emulator over USB, and push files to
it over WiFi. Deliberately read-only (for now) to avoid any write conflicts and keep it simple.

```
PC (adfpush / browser)  ──WiFi──▶  ESP32-S3  ──USB device (MSC, read-only)──▶  Gotek ──▶ Amiga
                                    LittleFS ─▶ FAT16 image (PSRAM)
```

## How it works

- ADF disk images live in a **LittleFS** partition on the ESP32's flash.
- On boot and after every upload, a deterministic **FAT16 image** (6 MiB) is built
  in **PSRAM** containing those files.
- The native USB port presents that image to the Gotek as a **read-only**
  mass-storage device (TinyUSB MSC). FlashFloppy reads it as an ordinary stick.
- A tiny HTTP server (WiFi + mDNS) accepts uploads and forces a USB
  re-enumeration so FlashFloppy re-scans the directory.

The 6 MiB image holds about six ADFs. FlashFloppy needs them uncompressed, so
compressing the ADFs would not fit any more of them. A stick with no `.adf` on
it shows **E34** on the Gotek (no usable disk image).

## Hardware

- ESP32-S3 with **native USB (OTG)** and **PSRAM** — e.g. ESP32-S3-DevKitC-1 with
  an **N16R8** module (16 MB flash + 8 MB PSRAM).
  - Only S2 / S3 / C6 / P4 have the OTG peripheral. A classic ESP32 or an
    ESP32-C3 will **not** work.
- USB cable from the ESP32's **native USB** port (not the UART bridge port) to
  the Gotek's USB-A socket.
- Power the ESP32 from its own supply/pins. A dev board can usually run off the
  Gotek's 5 V rail, but WiFi TX current spikes can brown it out — add a ~100 µF
  cap or use a separate supply if it resets under load.

## Configure

WiFi credentials live in `include/secrets.h`, which is **gitignored** so they
cannot be committed:

```c
#define WIFI_SSID   "YOUR_WIFI_SSID"
#define WIFI_PASS   "YOUR_WIFI_PASSWORD"
```

The mDNS name is `HOSTNAME` in `src/main.cpp` (`cursed` → `cursed.local`).

## Build & flash

```bash
pio run                 # compile
pio run -t upload       # flash the firmware
pio device monitor      # UART log (115200)
```

If your module isn't N16R8, adjust `board_build.arduino.memory_type` and
`partitions.csv` to match its flash/PSRAM size.

## Use

```bash
tools/adfpush ~/amiga/adfs/Lemmings.adf
tools/adfpush ~/amiga/adfs/            # bulk-upload *.adf
```

Each upload rebuilds the image and re-enumerates the USB device, so the new
files appear on the Gotek immediately; in a browser the page returns to the
file list once the upload finishes.

The stick stores the filename you send, and the name the Gotek sees is generated
from what is stored: an 8.3 short name plus a VFAT long-name entry, so
`Turrican II.adf` stays `Turrican II.adf` on the Gotek and in the file list even
though the directory entry is `TURRIC~1.ADF`. The two names are independent —
nothing on the stick is renamed to please the FAT layer.

The one exception is a filesystem that cannot hold the name at all: an image
written by `pio run -t uploadfs` is capped at 32 bytes by mklittlefs, and a
longer name is stored under a shortened but stable form instead
(`Workbench v3.1 rev 40.42 (1994)(Commodore)(Disk 1 of 6)(Install).adf` becomes
`Workbench v3.1 rev 40~0882ee.adf` — same extension, and the same input always
maps to the same name so re-uploading replaces rather than duplicates). Uploads
that cannot be stored at all are reported as an HTTP `500` and the partial file
is removed rather than being silently dropped; the UART log names the file and
the reason (`errno`).

That 32-byte cap lives in the filesystem, not the firmware, and only because it
was *seeded*: the firmware formats the partition with a 255-byte limit. To remove
it, erase just the filesystem partition and reboot — the next mount finds nothing
and formats it itself, after which full-length names are stored verbatim:

```bash
pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 --port <uart-port> \
    erase_region 0x610000 0x9F0000
```

`<uart-port>` is the WCH UART bridge you flash over — the same one as `pio run -t
upload`. Do not use the native USB (OTG) port: it is the one wired to the Gotek
and it cannot reset the chip. The `ttyACM`/`ttyUSB` number is not stable across
plug-ins, so check with `pio device list` (the bridge shows as `1a86:55d3`).
`erase_region` wants a 4096-byte-aligned address and size — `0x610000`/`0x9F0000`
is exactly the filesystem partition from `partitions.csv`. (`pio pkg exec` finds
PlatformIO's esptool; a bare `python -m esptool` does **not** work on this
machine — esptool and pyserial live only in PlatformIO's environment. The
equivalent explicit form is `~/.platformio/penv/bin/python
~/.platformio/packages/tool-esptoolpy/esptool.py …`.)

This deletes the ADFs on the stick (re-push them), and do **not** follow it with
`pio run -t uploadfs`, which would impose the 32-byte limit all over again. Back
up anything you cannot simply re-push first — a raw
`read_flash 0x610000 0x9F0000` dump can be read back with `littlefs-python` (the
exact call is in AGENTS.md), which is the only way to recover a file that exists
nowhere else.

Browsing to the stick's address gives the file list, an upload form, and a
**delete** button per file (deleting also re-enumerates USB, so the Gotek
re-scans):

```
http://172.16.4.15/
```

`adfpush` defaults to the mDNS name `cursed.local`. mDNS is link-local and does
not cross a router or VLAN, so if your PC is on a different network, pass the IP
the firmware prints on the UART at boot:

```bash
CURSED_HOST=172.16.4.15 tools/adfpush ~/amiga/adfs/Lemmings.adf
```

The stick carries a default `FF.CFG` and an empty `IMAGE_A.CFG` (both written
on first boot), so a factory-fresh Gotek is configured without a hand-prepared
USB stick. Upload your own `FF.CFG` to override it:

```bash
tools/adfpush my-FF.CFG
```

FlashFloppy stores FF.CFG values into the Gotek's own flash as its new
defaults, so only change what you actually need.

I apologise now for this unholy tottering pile of tech babel.
