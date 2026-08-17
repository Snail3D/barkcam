# Product

<!-- impeccable:product-schema 1 -->

## Platform

web (mobile-first — most visitors flash from a phone in Chrome; desktop Chrome/Edge also supported)

## Users

Dog owners who bought a Seeed XIAO ESP32S3 Sense (~$20) and want to know when their dog barks. Two equally-weighted visitor types (user-confirmed):

- **New owners** doing first-time setup: flash firmware, join the board's WiFi, enter Telegram credentials.
- **Existing owners** re-flashing after a firmware update: just want the flash button to work.

## Product Purpose

Get bark alerts working on a $20 half-dollar-size camera board: when the dog barks, the board snaps a photo and sends it to the owner on Telegram. Success = visitor flashes firmware in their browser (no install, no soldering) and completes the ~2-minute setup.

## Positioning

No cloud, no AI model — DSP on the board: high-pass filter, adaptive noise floor, burst-shape check. One photo per 2 minutes no matter how long the dog barks. (User-approved copy, live on snail3d.com.)

## Operating Context

- Flashing: WebSerial in Chrome/Edge (desktop or Android). Safari/Firefox unsupported — those visitors get the download link.
- The board auto-resets into bootloader on connect; no button pressing (XIAO ESP32S3 native USB).
- After flashing: board broadcasts open AP "barkcam-config" for 10 minutes; config UI at barkcam.local (or 192.168.4.1).
- The flasher writes the merged bin to 0x0 without erasing — NVS config survives re-flashes.
- Hosted on GitHub Pages (snail3d.github.io/barkcam). Local assets: esptool-bundle.js, firmware/barkcam-v1.bin (~1.06 MB), ui-config.png.

## Capabilities and Constraints

- WebSerial + esptool-bundle.js (local ES module) is the flash mechanism; it must keep working exactly as-is.
- Firmware bin: single merged file (bootloader@0x0 + partitions@0x8000 + app@0x10000), qio/80m/8MB.
- Product version v1 (FIRMWARE_VERSION 6); board is XIAO ESP32S3 Sense only.
- Fonts load from the same Google Fonts CDN as snail3d.com (Space Grotesk, JetBrains Mono, Instrument Serif).

## Brand Commitments

- Name: "Bark Cam" with 🐕.
- Visual world is pinned by the user to snail3d.com's design language: dark #05050a ground, neon accents (cyan/magenta/gold/purple), Space Grotesk + JetBrains Mono + Instrument Serif, particle canvas background, film grain. Bark Cam's accent on the portfolio is green (#3dffa0).
- Portfolio copy (user-approved, reuse verbatim where it fits): "A half-dollar-size camera board that listens for your dog. When it hears a bark, it snaps a photo and texts it to you on Telegram — no cloud, no AI model, just signal processing on the board." plus its three feature bullets.

## Evidence on Hand

- docs/ui-config.png — real screenshot of the board's config UI (live meter + settings).
- The device works: bark → photo → Telegram verified on hardware.
- No testimonials, no customer counts, no benchmarks — do not invent any.

## Product Principles

1. The mechanism is the pitch: show listening/triggering, don't just claim it.
2. Zero friction: one click to flash; setup is 3 steps, ~2 minutes.
3. No cloud, no AI — the privacy story is product truth; never imply server-side processing.
4. Works on a phone — mobile is the primary scene, not an afterthought.
5. Maker honesty: show the board, the code, and the limits (Safari/Firefox note).

## Accessibility & Inclusion

Standard web expectations: readable contrast on the dark ground, prefers-reduced-motion support (the portfolio honors it), keyboard-operable flash button.
