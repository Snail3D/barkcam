#pragma once

// ============================================================
// BARKCAM — XIAO ESP32S3 Sense pin map + tuning knobs
// Pin map for the Seeed XIAO ESP32S3 Sense.
// ============================================================

// --- microphone (PDM, via I2S) — do NOT reuse these pins ---
#define MIC_PDM_CLK   42
#define MIC_PDM_DATA  41

// --- camera (DVP parallel) — do NOT reuse these pins ---
#define CAM_PIN_XCLK   10
#define CAM_PIN_PCLK   13
#define CAM_PIN_VSYNC  38
#define CAM_PIN_HREF   47
#define CAM_PIN_SDA    40   // SCCB (SIOD)
#define CAM_PIN_SCL    39   // SCCB (SIOC)
#define CAM_PIN_D0     15
#define CAM_PIN_D1     17
#define CAM_PIN_D2     18
#define CAM_PIN_D3     16
#define CAM_PIN_D4     14
#define CAM_PIN_D5     12
#define CAM_PIN_D6     11
#define CAM_PIN_D7     48

// --- misc ---
#define LED_PIN        21   // USER LED, active-LOW

// If photos come out upside-down / mirrored, flip these and reflash.
#define CAM_VFLIP      1
#define CAM_HMIRROR    0
#define CAM_JPEG_QUALITY 12 // lower = sharper but bigger file (5–30)

// --- audio / bark detection tuning ---
#define SAMPLE_RATE          16000
#define FRAME_SAMPLES        256     // 16 ms per analysis frame
#define HP_CUTOFF_HZ         250.0f  // one-pole high-pass: kills rumble / AC hum
#define NOISE_FLOOR_DB       -60.0f  // never track noise below this
#define NOISE_RISE_COEF      0.10f   // per quiet frame, how fast the floor rises
#define NOISE_FALL_COEF      0.01f   // per quiet frame, how fast it falls
#define THRESHOLD_MARGIN_DB  15.0f   // bark threshold = noise floor + margin
#define BURST_MIN_FRAMES     3       // ~50 ms minimum to count as a bark
#define BURST_MAX_FRAMES     24      // ~380 ms maximum (longer = not a bark)
#define BURST_DECAY_DB       6.0f    // must drop this far below threshold to end a burst
#define BURST_DECAY_FRAMES   2       // consecutive decayed frames that end a burst
#define BARKS_TO_CONFIRM     2       // barks within the window => bark event
#define BARK_WINDOW_MS       4000    // ...within this many ms

// --- behavior ---
#define COOLDOWN_MS          120000  // max one photo per 2 min (fixed — not user-tunable)
#define WIFI_TIMEOUT_MS      20000
// Must outlast the worst un-ticked path in loop(): camera capture (~3 s) +
// a Telegram send where every HTTP stage hits its 15 s timeout (~45 s).
// A truly stuck loop still reboots within 90 s.
#define WATCHDOG_TIMEOUT_MS  90000
#define PERIODIC_REBOOT_MS   21600000ULL // reboot every 6 h — keeps heap fresh for unattended duty

// Your UTC offset in hours (e.g. -7 for Pacific). Used for photo captions.
#define TZ_OFFSET_HOURS      0

// Bump for each release — shown in the serial banner ("fw %d").
#define FIRMWARE_VERSION   7

// --- config access point (open AP, first N minutes after power-on) ---
#define AP_SSID          "barkcam-config"
#define AP_WINDOW_MS     600000  // 10 min window, then drop the AP (board stays on home WiFi)

// --- camera orientation setting (0=none 3=180; 90° not supported by this camera) ---
#define CAM_ROTATE_DEFAULT 3     // this install is mounted upside-down — needs 180°
// --- camera exposure setting (0=dim 1=medium 2=bright) ---
#define CAM_EXPOSURE_DEFAULT 1     // medium = the look you approved; dim for window light, bright outdoors
