# Buyer notes — Bark Cam

Email your GitHub username to snailmail3d@gmail.com and you'll be added to this
repository, usually within a day.

## Start here

1. Get the board: **Seeed XIAO ESP32S3 Sense** — the *Sense* version, with the
   camera + mic expansion and an OV2640. A plain XIAO ESP32S3 has no camera or
   mic and can't do this. About $14–20 shipped.
2. Flash it: `pio run -t upload` over USB (no button pressing), or serve the
   [`flasher/`](flasher/) folder and flash from Chrome — see
   [`flasher/README.md`](flasher/README.md).
3. Make a Telegram bot with @BotFather, grab your numeric user ID from
   @userinfobot, press **Start** in the bot chat.
4. Power the board near the dog, join its `barkcam-config` WiFi for 10 minutes,
   open `barkcam.local` on your phone, fill in WiFi + bot token + user ID, hit
   Save. Bark at it once to check.

## What's in the box

```
src/main.cpp                  the firmware: mic pipeline, camera, Telegram,
                              config AP + web UI, watchdog, schedule
include/bark_detector.h       the bark detector — high-pass, noise floor,
                              burst shape, confirmation. Swappable by design
include/config.h              pin map + every tuning knob in one place
include/ui_page.h             the phone config UI, one self-contained page
flasher/                      your own browser flasher (page + esptool + bin)
tools/mock_ui.py              run the config UI on your Mac, no hardware
tools/secret_scan.sh          pre-publish sweep: greps text AND the .bin for
                              leaked credentials (scan output is redacted)
platformio.ini                the build, pinned to the one core version that
                              doesn't break the PDM mic driver
```

## Your Telegram bill, honestly

Telegram's Bot API is free and Bark Cam sends a few kilobytes per bark. You
need no account with anyone, including me — the bot is yours, the photo path
is board → Telegram → your phone. Nothing routes through a server of mine, and
there's nothing to subscribe to.

## Updates

Every update lands here — `git pull`. If you bought before an update and it's
material, you'll get an email too.

## Questions

snailmail3d@gmail.com — real human, answers within a day or two.
