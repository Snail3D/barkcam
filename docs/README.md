# Your own browser flasher

This is the whole uploader: no server, no Python, no esptool install on the
buyer's machine. It's four files in two folders, and they're yours to host
wherever you like.

```
index.html                  the page (design matches snail3d.com)
esptool-bundle.js           bundled esptool — talks to the board over WebSerial
firmware/barkcam-v1.1.bin   merged image: bootloader + partitions + app, one
                            file written at 0x0
ui-config.png               screenshot used on the page
```

## Run it locally

WebSerial only works on a secure origin, so `file://` won't work. Serve it:

```bash
cd flasher && python3 -m http.server 8080
```

Open <http://localhost:8080> in **Chrome or Edge** (desktop or Android), plug
the board in over USB, press **Flash**, wait for the success card. Safari and
Firefox have no WebSerial — the page says so and offers the esptool one-liner
instead.

No button pressing: the XIAO's native USB resets itself into the bootloader
when the page opens the port.

## Put it on the internet

Any static host works — GitHub Pages is free and takes a minute:

1. New GitHub repo, drop these into a `docs/` folder — `index.html`, `esptool-bundle.js`, `ui-config.png`, and the `firmware/` folder (keep the subfolder; the page fetches `firmware/barkcam-v1.1.bin` relative to itself).
2. Settings → Pages → Source: `main` branch, `/docs` folder.
3. The URL GitHub gives you is your flasher.

HTTPS is required for WebSerial, which GitHub Pages and Netlify/Vercel give you
for free. The page must be served over HTTPS, not embedded in another origin.

## Point it at your own builds

To flash *your* changes instead of the shipped v1.1:

```bash
pio run                                  # build first
pio run -t merge                         # produces .pio/build/seeed_xiao_esp32s3/firmware.bin
cp .pio/build/seeed_xiao_esp32s3/firmware.bin flasher/firmware/barkcam-v1.1.bin
```

Two rules, learned the hard way:

- **Re-merge with the esptool that ships with PlatformIO (v4.x), never
  esptool v5.** v5 overwrites the bootloader's flash-mode byte and a board
  flashed with that bin never reaches the app — it just watchdog-resets
  forever. If you ever hand-merge, compare byte 2 of the image against a
  known-good bin (it must read `0x02`, QIO).
- Write at `0x0` **without erasing**, so a re-flash keeps the buyer's saved
  WiFi and Telegram settings in NVS.

If you rename the bin, update both places in `index.html` that name it: the
download link in "Other ways" and the `fetch()` in the flash script.
