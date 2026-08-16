// BARKCAM v1 — dog bark monitor on the Seeed XIAO ESP32S3 Sense
//
// Pipeline:
//   PDM mic (GPIO41/42) -> 16 kHz PCM -> energy burst detector
//   >= BARKS_TO_CONFIRM barks within BARK_WINDOW_MS  ->  bark event
//   bark event (and cooldown expired) -> capture JPEG -> Telegram sendPhoto
//
// Cooldown: at most one photo per cfg.cooldownMs (default 2 min). A dog that
// barks for ten minutes produces at most one photo per two minutes, never more.
//
// Config: for the first 10 minutes after power-on the board broadcasts an open AP
// "barkcam-config" and serves a web UI (http://192.168.4.1) to tune
// sensitivity, cooldown, WiFi and Telegram settings —
// persisted in NVS across reboots.
//
// Serial commands (115200):
//   t  force a test photo + Telegram send (bypasses detector and cooldown)
//   s  print status (mode, noise floor, threshold, last frame level, cooldown)
//   c  clear the cooldown so the next bark sends immediately
//   1  raise detection threshold by 2 dB (fewer triggers)
//   2  lower detection threshold by 2 dB (more triggers)

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <esp_camera.h>
#include <driver/i2s.h>
#include <time.h>
#include <esp_heap_caps.h>

#include "config.h"
#include "credentials.h"
#include "bark_detector.h"
#include "telegram_ca.h"
#include "ui_page.h"

// ============================ runtime config (NVS-persisted)

struct AppConfig {
    String ssid, pass;
    String botToken, chatId;
    float marginDb;       // sensitivity: bark threshold = noise floor + margin
    uint32_t cooldownMs;  // max one photo per this many ms (fixed at COOLDOWN_MS)
    int rotate;           // photo rotation: 0=none 1=90CW 2=90CCW 3=180
    int exposure;         // photo brightness: 0=dim 1=medium 2=bright
    uint32_t apWindowMs;  // config-AP window at boot (default 10 min)
};

static AppConfig cfg = {
    WIFI_SSID, WIFI_PASSWORD,
    TELEGRAM_BOT_TOKEN, TELEGRAM_CHAT_ID,
    THRESHOLD_MARGIN_DB, COOLDOWN_MS, CAM_ROTATE_DEFAULT, CAM_EXPOSURE_DEFAULT, AP_WINDOW_MS
};

static Preferences prefs;   // namespace "barkcam" (opened in loadConfig)

static void loadConfig() {
    prefs.begin("barkcam", false);   // read-write; saves reuse the same handle
    if (prefs.isKey("ssid"))     { String v = prefs.getString("ssid", "");   if (v.length()) cfg.ssid = v; }
    if (prefs.isKey("pass"))     { String v = prefs.getString("pass", "");   if (v.length()) cfg.pass = v; }
    if (prefs.isKey("token"))    { String v = prefs.getString("token", "");  if (v.length()) cfg.botToken = v; }
    if (prefs.isKey("chatId"))   { String v = prefs.getString("chatId", ""); if (v.length()) cfg.chatId = v; }
    if (prefs.isKey("margin"))   cfg.marginDb = prefs.getFloat("margin", THRESHOLD_MARGIN_DB);
    if (prefs.isKey("rotate"))   cfg.rotate = prefs.getInt("rotate", CAM_ROTATE_DEFAULT);
    if (prefs.isKey("exposure")) cfg.exposure = prefs.getInt("exposure", CAM_EXPOSURE_DEFAULT);
    if (prefs.isKey("apWindow")) cfg.apWindowMs = prefs.getInt("apWindow", AP_WINDOW_MS);
}

static void saveConfig() {
    prefs.putString("ssid", cfg.ssid);
    prefs.putString("pass", cfg.pass);
    prefs.putString("token", cfg.botToken);
    prefs.putString("chatId", cfg.chatId);
    prefs.putFloat("margin", cfg.marginDb);
    prefs.putInt("rotate", cfg.rotate);
    prefs.putInt("exposure", cfg.exposure);
    prefs.putInt("apWindow", (int)cfg.apWindowMs);
}

// ============================ globals

static const i2s_port_t I2S_PORT = I2S_NUM_0;
static bool micReady = false;
static bool cameraReady = false;
static BarkDetector detector;

static uint32_t lastSendMs = 0;          // cooldown anchor
static volatile uint32_t loopTick = 0;   // watchdog ping
static bool working = false;             // LED solid while capturing/sending

// config access point state
static WebServer server(80);
static bool apMode = false;
static uint32_t apStartMs = 0;
static uint32_t staDiscSince = 0;
static bool everConnected = false;
// Auto-exit (polled in loop): once a phone has connected, when the last one
// leaves, close the AP after 10 s (debounce against transient drops). No
// client ever connected -> apEmptySince stays 0, the 10-min window rules.
static uint32_t apEmptySince = 0;
static bool apClientEver = false;

// web test request (handler sets the flag, loop does the work)
static volatile bool pendingTest = false;
static String lastResult = "none";       // none | ok | fail

#define LED_ON()  digitalWrite(LED_PIN, LOW)   // active-LOW
#define LED_OFF() digitalWrite(LED_PIN, HIGH)

// ============================ helpers

static void *bigMalloc(size_t n) {
#if CONFIG_SPIRAM || defined(BOARD_HAS_PSRAM)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return malloc(n);
}

// ============================ microphone (PDM via I2S)

static bool initMic() {
    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM);
    cfg.sample_rate          = SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_RIGHT;   // single PDM mic
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count        = 4;
    cfg.dma_buf_len          = 256;
    cfg.use_apll             = true;
    if (i2s_driver_install(I2S_PORT, &cfg, 0, NULL) != ESP_OK) return false;

    i2s_pin_config_t pins = {};
    pins.bck_io_num   = I2S_PIN_NO_CHANGE;              // PDM doesn't use BCLK
    pins.ws_io_num    = MIC_PDM_CLK;                    // PDM clock  (GPIO42)
    pins.data_out_num = I2S_PIN_NO_CHANGE;
    pins.data_in_num  = MIC_PDM_DATA;                   // PDM data   (GPIO41)
    return i2s_set_pin(I2S_PORT, &pins) == ESP_OK;
}

// ============================ camera (lazy — only on first bark)

// Apply the configured photo orientation + exposure to the sensor hardware.
// 180° = vflip + hmirror (a true rotation, done in the sensor — no metadata).
// 90° rotations can't be done in hardware; those get an EXIF tag at capture.
static void applyCameraTuning() {
    sensor_t *s = esp_camera_sensor_get();
    if (!s) return;
    int flip = (cfg.rotate == 3) ? 1 : 0;   // 180° = both flips
    s->set_vflip(s, flip);
    s->set_hmirror(s, flip);
    s->set_brightness(s, cfg.exposure);     // 0=dim 1=medium 2=bright
}

static bool initCamera() {
    camera_config_t c = {};
    c.ledc_channel = LEDC_CHANNEL_0;
    c.ledc_timer   = LEDC_TIMER_0;
    c.pin_d0  = CAM_PIN_D0;   c.pin_d1  = CAM_PIN_D1;
    c.pin_d2  = CAM_PIN_D2;   c.pin_d3  = CAM_PIN_D3;
    c.pin_d4  = CAM_PIN_D4;   c.pin_d5  = CAM_PIN_D5;
    c.pin_d6  = CAM_PIN_D6;   c.pin_d7  = CAM_PIN_D7;
    c.pin_xclk   = CAM_PIN_XCLK;
    c.pin_pclk   = CAM_PIN_PCLK;
    c.pin_vsync  = CAM_PIN_VSYNC;
    c.pin_href   = CAM_PIN_HREF;
    c.pin_sccb_sda = CAM_PIN_SDA;
    c.pin_sccb_scl = CAM_PIN_SCL;
    c.pin_pwdn  = -1;
    c.pin_reset = -1;
    c.xclk_freq_hz = 20000000;
    c.pixel_format = PIXFORMAT_JPEG;
    c.frame_size   = FRAMESIZE_VGA;      // 640x480 — small file, fast capture
    c.jpeg_quality = CAM_JPEG_QUALITY;   // 12 ≈ 30–60 KB
    c.fb_count     = 2;
    c.grab_mode    = CAMERA_GRAB_LATEST;

    if (esp_camera_init(&c) != ESP_OK) return false;

    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        applyCameraTuning();   // vflip/hmirror per cfg.rotate + brightness per cfg.exposure
        s->set_contrast(s, 2);
        s->set_saturation(s, -1);        // pull down the green cast
        s->set_whitebal(s, 1);
        s->set_awb_gain(s, 1);
        s->set_wb_mode(s, 1);            // "sunny" — good for outdoors
        s->set_lenc(s, 1);               // lens correction
    }
    cameraReady = true;
    Serial.println("camera ready (VGA q12)");
    return true;
}

// EXIF orientation segments for the 90° rotations (the OV2640 can't rotate in
// hardware). One IFD entry with the Orientation tag, inserted right after the
// SOI marker; viewers (Telegram included) apply it when displaying.
static const uint8_t exif_o6[40] = {   // 90° CW
    0xFF, 0xE1, 0x26, 0x00,           // APP1 marker, length 38
    'E', 'x', 'i', 'f', 0, 0,         // "Exif\0\0"
    0x49, 0x49, 0x2A, 0x00,           // little-endian TIFF, magic 42
    0x08, 0x00, 0x00, 0x00,           // IFD offset
    0x01, 0x00,                       // one entry
    0x12, 0x01, 0x00, 0x00,           // tag 0x0112 (Orientation)
    0x03, 0x00, 0x00, 0x00,           // type SHORT
    0x01, 0x00, 0x00, 0x00,           // count = 1
    0x06, 0x00, 0x00, 0x00,           // value = 6
    0x00, 0x00, 0x00, 0x00            // next IFD = none
};
static const uint8_t exif_o8[40] = {   // 90° CCW
    0xFF, 0xE1, 0x26, 0x00,           // APP1 marker, length 38
    'E', 'x', 'i', 'f', 0, 0,         // "Exif\0\0"
    0x49, 0x49, 0x2A, 0x00,           // little-endian TIFF, magic 42
    0x08, 0x00, 0x00, 0x00,           // IFD offset
    0x01, 0x00,                       // one entry
    0x12, 0x01, 0x00, 0x00,           // tag 0x0112 (Orientation)
    0x03, 0x00, 0x00, 0x00,           // type SHORT
    0x01, 0x00, 0x00, 0x00,           // count = 1
    0x08, 0x00, 0x00, 0x00,           // value = 8
    0x00, 0x00, 0x00, 0x00            // next IFD = none
};

static bool injectExif(const uint8_t *in, size_t inLen, const uint8_t *tpl, uint8_t **out, size_t *outLen) {
    if (inLen < 2 || in[0] != 0xFF || in[1] != 0xD8) return false;   // not a JPEG
    size_t total = inLen + 40;
    uint8_t *buf = (uint8_t *)bigMalloc(total);
    if (!buf) return false;
    memcpy(buf, in, 2);                        // SOI
    memcpy(buf + 2, tpl, 40);                  // EXIF APP1 segment
    memcpy(buf + 42, in + 2, inLen - 2);       // rest of the JPEG
    *out = buf;                                // fresh allocation — caller frees
    *outLen = total;
    return true;
}

static bool capturePhoto(uint8_t **out, size_t *outLen) {
    if (!cameraReady && !initCamera()) {
        Serial.println("!! camera init failed — is the Sense expansion attached? !!");
        return false;
    }
    if (!cameraReady) return false;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { Serial.println("camera: no frame"); return false; }

    bool ok = false;
    if (cfg.rotate == 1 || cfg.rotate == 2) {
        // 90° rotation: hardware can't do it — tag the JPEG with EXIF orientation
        uint8_t *buf = nullptr;
        size_t len = 0;
        if (injectExif(fb->buf, fb->len, (cfg.rotate == 1) ? exif_o6 : exif_o8, &buf, &len)) {
            *out = buf; *outLen = len; ok = true;
        } else {
            Serial.println("exif inject failed — sending raw frame");
        }
    }
    if (!ok) {
        // 0° (raw) or 180° (already flipped in sensor hardware): copy the frame
        uint8_t *buf = (uint8_t *)bigMalloc(fb->len);
        if (buf) { memcpy(buf, fb->buf, fb->len); *out = buf; *outLen = fb->len; ok = true; }
    }
    esp_camera_fb_return(fb);

    if (ok) Serial.printf("photo ready (%u KB, rot=%d)\n", (unsigned)(*outLen / 1024), cfg.rotate);
    else Serial.println("!! photo capture failed !!");
    return ok;
}

// ============================ time (NTP, for captions)

static void initTime() {
    configTime(TZ_OFFSET_HOURS * 3600, 0, "0.us.pool.ntp.org", "1.us.pool.ntp.org");
}

static String clockString() {
    time_t t = time(nullptr);
    if (t < 1600000000) return String();   // NTP not synced yet
    struct tm lt;
    localtime_r(&t, &lt);
    char b[16];
    strftime(b, sizeof(b), "%H:%M:%S", &lt);
    return String(b);
}

// ============================ telegram

static bool sendTelegramPhoto(const uint8_t *jpeg, size_t jpegLen) {
    if (!jpeg || !jpegLen) return false;

    String url = String("https://api.telegram.org/bot") + cfg.botToken + "/sendPhoto";

    WiFiClientSecure client;
    client.setCACert(TELEGRAM_ROOT_CA);   // GoDaddy Root G2 (api.telegram.org)
    client.setTimeout(15000);   // bound the TLS handshake — fail clean, don't stall loop()
    HTTPClient http;
    if (!http.begin(client, url)) {
        Serial.println("telegram: begin failed");
        return false;
    }

    String ts = clockString();
    String caption = "🐕 Bark detected" + (ts.length() ? (" " + ts) : String());

    // Hand-rolled multipart/form-data body (binary JPEG can't go through a
    // String — NUL bytes). Header parts are text, the JPEG is memcpy'd in.
    String boundary = "BarkCam" + String(millis(), HEX);
    String head = "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + String(cfg.chatId) + "\r\n"
                  "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" + caption + "\r\n"
                  "--" + boundary + "\r\n"
                  "Content-Disposition: form-data; name=\"photo\"; filename=\"barkcam.jpg\"\r\n"
                  "Content-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";

    size_t total = head.length() + jpegLen + tail.length();
    uint8_t *body = (uint8_t *)bigMalloc(total);
    if (!body) { Serial.println("telegram: out of memory for body buffer"); http.end(); return false; }
    size_t off = 0;
    memcpy(body, head.c_str(), head.length());   off += head.length();
    memcpy(body + off, jpeg, jpegLen);           off += jpegLen;
    memcpy(body + off, tail.c_str(), tail.length());

    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    int code = http.POST(body, total);
    free(body);

    String resp = http.getString();
    http.end();
    client.stop();

    if (code != 200) { Serial.printf("telegram HTTP %d: %s\n", code, resp.c_str()); return false; }
    if (!resp.startsWith("{\"ok\":true")) { Serial.printf("telegram not ok: %s\n", resp.c_str()); return false; }
    Serial.printf("telegram accepted photo (%u KB)\n", (unsigned)(jpegLen / 1024));
    return true;
}

// ============================ bark event handler

static void handleBarkEvent(const char *why, bool force) {
    uint32_t now = millis();
    if (!force && (now - lastSendMs) < cfg.cooldownMs) {
        // Throttle the "skipping" log so a barking fit doesn't spam serial.
        static uint32_t lastSkipPrint = 0;
        if (now - lastSkipPrint > 10000) {
            Serial.printf("bark event (%s) in cooldown — skipping\n", why);
            lastSkipPrint = now;
        }
        return;
    }

    lastSendMs = now;
    working = true;
    LED_ON();
    // Wait for home WiFi (bounded, non-blocking) — right after boot it may
    // still be associating. Tick loopTick so the watchdog stays happy.
    uint32_t waitStart = millis();
    while (!WiFi.isConnected() && (millis() - waitStart < 20000)) {
        loopTick++;   // keep the watchdog fed while we wait
        delay(100);
    }

    Serial.printf("BARK EVENT (%s) — capturing + sending\n", why);

    uint8_t *jpg = nullptr;
    size_t jpgLen = 0;
    bool ok = false;
    if (WiFi.isConnected()) {
        ok = capturePhoto(&jpg, &jpgLen) && sendTelegramPhoto(jpg, jpgLen);
    } else {
        Serial.println("wifi not connected — skipping send");
    }
    if (jpg) free(jpg);

    working = false;
    LED_OFF();
    lastResult = ok ? "ok" : "fail";

    if (!ok) {
        // Failed: allow a retry sooner than the full cooldown.
        uint32_t retry = cfg.cooldownMs < 30000 ? cfg.cooldownMs : 30000;
        lastSendMs = now - (cfg.cooldownMs > retry ? cfg.cooldownMs - retry : 0);
        Serial.println("send failed — will retry sooner if barking continues");
    } else {
        Serial.printf("photo sent. next send allowed in %lu s\n", (unsigned long)(cfg.cooldownMs / 1000));
    }
}

// ============================ config access point (AP + web UI)

static void connectSTA();   // forward decl — startAP calls it below its definition
static void startAP() {
    WiFi.mode(WIFI_AP_STA);   // keep home WiFi while serving config — test photos work
    WiFi.softAP(AP_SSID);   // open AP — local config window
    apMode = true;          // lost in the mDNS commit — without it the web UI never runs
    apStartMs = millis();   // ...and the window is measured from this open, not boot
    static bool mdnsUp = false;
    if (!mdnsUp) { MDNS.begin("barkcam"); mdnsUp = true; }   // http://barkcam.local
    apEmptySince = 0;
    apClientEver = false;   // fresh window: stay open until a client comes and goes
    server.begin();
    connectSTA();   // home WiFi in parallel (board stays online while configuring)
    Serial.printf("config AP '%s' up for %lu min — phone: connect, open http://barkcam.local (or 192.168.4.1)\n",
                  AP_SSID, (unsigned long)(cfg.apWindowMs / 60000));
}

static void exitAP() {
    if (!apMode) return;
    apMode = false;
    server.close();   // free the port-80 listener (begin() re-creates it on reopen)
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);   // let the radio modem-sleep in normal operation (power/heat)
}

static void connectSTA() {
    if (cfg.ssid.length()) WiFi.begin(cfg.ssid, cfg.pass);
}

static void handleRoot() { server.send_P(200, "text/html", UI_PAGE); }

static float normDb(float db) {
    float v = (db + 80.0f) / 60.0f;   // -80..-20 dBFS -> 0..1
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

static void handleLevel() {
    String j = "{\"mode\":\"";
    j += apMode ? (WiFi.isConnected() ? "config + online" : "config, no wifi") : (WiFi.isConnected() ? "online" : "offline");
    j += "\",\"db\":" + String(normDb(detector.lastFrameDb()), 2)
       + ",\"noise\":" + String(normDb(detector.noiseDb()), 2)
       + ",\"thr\":" + String(normDb(detector.thresholdDb()), 2) + ",\"hist\":[";
    const float *h = detector.hist();
    int idx = detector.histIdx();   // oldest first
    for (int i = 0; i < BarkDetector::HIST_LEN; i++) {
        if (i) j += ",";
        j += String(normDb(h[(idx + i) % BarkDetector::HIST_LEN]), 2);
    }
    j += "]}";
    server.send(200, "application/json", j);
}

static void handleGetConfig() {
    String j = "{\"ssid\":\"" + cfg.ssid + "\",\"pass\":\"" + cfg.pass
             + "\",\"token\":\"" + cfg.botToken + "\",\"chatId\":\"" + cfg.chatId
             + "\",\"margin\":" + String(cfg.marginDb, 1)
             + ",\"rotate\":" + String(cfg.rotate)
             + ",\"exposure\":" + String(cfg.exposure)
             + ",\"apWindowMs\":" + String((uint32_t)cfg.apWindowMs) + "}";
    server.send(200, "application/json", j);
}

static void handlePostConfig() {
    bool changed = false;
    if (server.hasArg("ssid"))       { cfg.ssid = server.arg("ssid"); changed = true; }
    if (server.hasArg("pass"))       { cfg.pass = server.arg("pass"); changed = true; }
    if (server.hasArg("token"))      { cfg.botToken = server.arg("token"); changed = true; }
    if (server.hasArg("chatId"))     { cfg.chatId = server.arg("chatId"); changed = true; }
    if (server.hasArg("margin"))     { cfg.marginDb = server.arg("margin").toFloat(); detector.setMargin(cfg.marginDb); changed = true; }
    if (server.hasArg("rotate"))     { int v = server.arg("rotate").toInt(); if (v >= 0 && v <= 3) { cfg.rotate = v; changed = true; } }
    if (server.hasArg("exposure"))   { int v = server.arg("exposure").toInt(); if (v >= 0 && v <= 2) { cfg.exposure = v; changed = true; } }
    if (server.hasArg("apWindowMs")) { uint32_t v = server.arg("apWindowMs").toInt(); if (v >= 5000) { cfg.apWindowMs = v; changed = true; } }

    if (changed) { saveConfig(); if (cameraReady) applyCameraTuning(); }
    Serial.println("config updated via web UI");

    // WiFi changed: reconnect with the new credentials (keep the config AP up).
    if (apMode && server.hasArg("ssid") && cfg.ssid.length()) {
        Serial.println("wifi changed — reconnecting");
        WiFi.disconnect();
        connectSTA();
    }

    server.send(200, "application/json", changed ? "{\"ok\":true}" : "{\"ok\":false}");
}

static void handlePostTest() {
    pendingTest = true;
    lastResult = "pending";
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"test started\"}");
}

static void handleStatus() {
    uint32_t now = millis();
    String j = "{\"mode\":\"";
    j += apMode ? "ap" : (WiFi.isConnected() ? "sta" : "offline");
    j += "\",\"rssi\":" + String(WiFi.RSSI())
       + ",\"cooldownLeftMs\":" + String((now - lastSendMs < cfg.cooldownMs) ? (cfg.cooldownMs - (now - lastSendMs)) : 0)
       + ",\"pendingTest\":" + (pendingTest ? "true" : "false")
       + ",\"lastResult\":\"" + lastResult + "\"}";
    server.send(200, "application/json", j);
}

static void handleClose() {
    if (apMode) { Serial.println("config closed via web UI"); exitAP(); }
    server.send(200, "application/json", "{\"ok\":true}");
}

static void setupWeb() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/level", HTTP_GET, handleLevel);
    server.on("/config", HTTP_GET, handleGetConfig);
    server.on("/config", HTTP_POST, handlePostConfig);
    server.on("/test", HTTP_POST, handlePostTest);
    server.on("/close", HTTP_POST, handleClose);
    server.on("/status", HTTP_GET, handleStatus);
}

// ============================ status + watchdog

static void printStatus() {
    uint32_t now = millis();
    uint32_t cdLeft = (now - lastSendMs < cfg.cooldownMs) ? (cfg.cooldownMs - (now - lastSendMs)) / 1000 : 0;
    Serial.printf(
        "--- barkcam status ---\n"
        "mode        : %s\n"
        "noise floor : %6.1f dBFS\n"
        "threshold   : %6.1f dBFS (margin %.1f)\n"
        "last frame  : %6.1f dBFS\n"
        "wifi rssi   : %d dBm  sleep:%s  heap: %u KB  psram free: %u KB\n"
        "cooldown    : %lu s remaining (cap %lu s)\n",
        apMode ? "config AP" : (WiFi.isConnected() ? "sta" : "offline"),
        detector.noiseDb(),
        detector.thresholdDb(),
        detector.marginDb(),
        detector.lastFrameDb(),
        WiFi.RSSI(),
        WiFi.getSleep() ? "on" : "off",
        (unsigned)(ESP.getFreeHeap() / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned long)cdLeft,
        (unsigned long)(cfg.cooldownMs / 1000));
}

// ---- wifi scan (serial command 'w') — top networks by RSSI
struct NetInfo { String ssid; int rssi; uint8_t ch; };
static void wifiScan() {
    Serial.println("scanning...");
    int n = WiFi.scanNetworks();   // blocking, a few seconds
    if (n <= 0) { Serial.println("no networks found"); return; }
    int m = min(n, 24);
    static NetInfo nets[24];
    for (int i = 0; i < m; i++) { nets[i].ssid = WiFi.SSID(i); nets[i].rssi = WiFi.RSSI(i); nets[i].ch = WiFi.channel(i); }
    for (int i = 1; i < m; i++) {   // insertion sort, best first
        NetInfo key = nets[i];
        int j = i - 1;
        while (j >= 0 && nets[j].rssi < key.rssi) { nets[j + 1] = nets[j]; j--; }
        nets[j + 1] = key;
    }
    for (int i = 0; i < m && i < 12; i++)
        Serial.printf("%2d. %-24s %3d dBm  ch%u\n", i + 1, nets[i].ssid.c_str(), nets[i].rssi, nets[i].ch);
}

static void watchdogTask(void *) {
    uint32_t last = loopTick;
    uint32_t staleMs = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (loopTick != last) { last = loopTick; staleMs = 0; }
        else                  { staleMs += 1000; }
        if (staleMs > WATCHDOG_TIMEOUT_MS) {
            Serial.println("WATCHDOG: loop stalled — rebooting");
            ESP.restart();
        }
    }
}

// ============================ setup / loop

void setup() {
    Serial.begin(115200);

    pinMode(LED_PIN, OUTPUT);
    LED_OFF();

    loadConfig();
    detector.begin();
    detector.setMargin(cfg.marginDb);

    // Mic — always on; continuous listening is the whole point of this device.
    micReady = initMic();

    // Phase 1: config window (open AP + home WiFi, first N minutes after power-on)
    setupWeb();
    startAP();

    // USB CDC settle — keep serving the web UI while we wait, so a phone that
    // connects in the first seconds still gets its config page.
    uint32_t t0 = millis();
    while (millis() - t0 < 3000) { server.handleClient(); delay(50); }

    Serial.println();
    Serial.printf("=== BARKCAM v1 (fw %d) ===\n", FIRMWARE_VERSION);
    if (micReady) Serial.println("mic ready (PDM 16 kHz)");
    else          Serial.println("!! MIC INIT FAILED — is the Sense expansion attached? !!");

    xTaskCreatePinnedToCore(watchdogTask, "wd", 2048, nullptr, 1, nullptr, 1);

    Serial.println("commands: t=test photo  s=status  c=clear cooldown  i=wifi info  w=scan  a=reopen config AP  1/2=tune");
}

void loop() {
    loopTick++;

    // --- periodic reboot (6 h) — keeps heap fresh for unattended duty ---
    if (millis() > PERIODIC_REBOOT_MS) { Serial.println("uptime reached — periodic reboot"); ESP.restart(); }

    // --- audio -> detector (always — the web meter needs it in AP mode too) ---
    if (micReady) {
        static int16_t frameBuf[FRAME_SAMPLES];
        static size_t fill = 0;
        int16_t chunk[256];
        size_t gotBytes = 0;
        i2s_read(I2S_PORT, chunk, sizeof(chunk), &gotBytes, pdMS_TO_TICKS(50));
        size_t n = gotBytes / 2;   // 16-bit samples
        for (size_t i = 0; i < n; i++) {
            frameBuf[fill++] = chunk[i];
            if (fill == FRAME_SAMPLES) {
                detector.processFrame(frameBuf, millis());
                fill = 0;
            }
        }
    } else {
        delay(50);
    }

    // --- config web UI (AP mode only) ---
    if (apMode) server.handleClient();

    // --- config window expired -> drop the AP (board stays on home WiFi) ---
    if (apMode && (millis() - apStartMs >= cfg.apWindowMs)) {
        Serial.println("config window closed — dropping config AP");
        exitAP();
    }

    // --- last config client left -> drop the AP (10 s debounce) ---
    if (apMode) {
        if (WiFi.softAPgetStationNum() > 0) { apClientEver = true; apEmptySince = 0; }
        else if (apClientEver) {
            if (!apEmptySince) apEmptySince = millis();
            else if (millis() - apEmptySince > 10000) {
                Serial.println("config client gone — dropping config AP");
                apEmptySince = 0;
                exitAP();
            }
        }
    }

    // --- serial commands ---
    while (Serial.available()) {
        char c = Serial.read();
        switch (c) {
            case 't': handleBarkEvent("manual test", true); break;
            case 's': printStatus(); break;
            case 'c': lastSendMs = 0; Serial.println("cooldown cleared"); break;
            case 'w': wifiScan(); break;
            case 'i': Serial.printf("wifi: ssid=%s ip=%s rssi=%d\n", WiFi.SSID().c_str(),
                      WiFi.localIP().toString().c_str(), WiFi.RSSI()); break;
            case 'a': startAP(); Serial.println("config AP reopened"); break;
            case '1': detector.setMargin(detector.marginDb() + 2.0f);
                      cfg.marginDb = detector.marginDb(); saveConfig();
                      Serial.printf("threshold margin now %.1f dB\n", detector.marginDb()); break;
            case '2': detector.setMargin(detector.marginDb() - 2.0f);
                      cfg.marginDb = detector.marginDb(); saveConfig();
                      Serial.printf("threshold margin now %.1f dB\n", detector.marginDb()); break;
        }
    }

    // --- web test request (handler set the flag) ---
    if (pendingTest) {
        pendingTest = false;
        handleBarkEvent("web test", true);   // sets lastResult ok/fail
    }

    // --- bark event? ---
    if (detector.eventDetected()) handleBarkEvent("barks", false);

    // --- STA recovery: disconnected too long -> back to config AP ---
    if (!apMode && WiFi.isConnected()) {
        everConnected = true;
        staDiscSince = 0;
    } else if (!apMode && !WiFi.isConnected()) {
        uint32_t grace = everConnected ? 60000 : 15000;
        static uint32_t lastConnLog = 0;
        if (millis() - lastConnLog > 5000) {
            lastConnLog = millis();
            Serial.printf("wifi: not connected (status %d)\n", WiFi.status());
        }
        if (!staDiscSince) staDiscSince = millis();
        else if (millis() - staDiscSince > grace) {
            Serial.println("wifi lost — reopening config AP");
            staDiscSince = 0;
            startAP();
        }
    }

    // --- LED: AP = 2 Hz blink, idle listening = 1 Hz, working = solid ---
    if (!working) {
        static uint32_t lastBlink = 0;
        static bool on = false;
        uint32_t period = apMode ? 250 : 500;
        if (millis() - lastBlink > period) {
            on = !on;
            digitalWrite(LED_PIN, on ? LOW : HIGH);
            lastBlink = millis();
        }
    }
}
