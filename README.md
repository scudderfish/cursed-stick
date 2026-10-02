# Cursed Gotek WiFi stick



This came out of a cursed idea I had a couple of days ago https://weird.autos/@scudderfish/117366678543083688  I can't do USB OTG with my desktop, but I can with an ESP32.

The aim of this is to present an ADF collection to a Gotek floppy emulator over USB, and push files to
it over WiFi. Deliberately read-only (for now) to avoid any write conflicts and keep it simple.

```
PC (adfpush / browser)  ──WiFi──▶  ESP32-S3  ──USB device (MSC, read-only)──▶  Gotek ──▶ Amiga
                                    LittleFS ─▶ FAT16 image (PSRAM)
```

## How it works

- ADF/`.adz` files live in a **LittleFS** partition on the ESP32's flash.
- On boot and after every upload, a deterministic **FAT16 image** (6 MiB) is built
  in **PSRAM** containing those files.
- The native USB port presents that image to the Gotek as a **read-only**
  mass-storage device (TinyUSB MSC). FlashFloppy reads it as an ordinary stick.
- A tiny HTTP server (WiFi + mDNS) accepts uploads and forces a USB
  re-enumeration so FlashFloppy re-scans the directory.

6 MiB of image holds ~6 raw ADFs, or considerably more if you send **`.adz`**
(gzipped ADF), which FlashFloppy decompresses on the fly.

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

Edit `src/main.cpp`:

```c
#define WIFI_SSID   "YOUR_WIFI_SSID"
#define WIFI_PASS   "YOUR_WIFI_PASSWORD"
#define HOSTNAME    "cursed"          // -> cursed.local
```

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
tools/adfpush ~/amiga/adfs/            # bulk-upload *.adf / *.adz
```

```bash
gzip -9k Lemmings.adf && tools/adfpush Lemmings.adz
```

Each upload rebuilds the image and re-enumerates the USB device, so the new
files appear on the Gotek immediately.

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