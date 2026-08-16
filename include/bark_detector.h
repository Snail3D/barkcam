#pragma once
#include <Arduino.h>
#include "config.h"

// Energy-based bark detector — no ML model.
//
// This is the same trick the cheap $20 "bark sensors" use (a loud sudden
// sound above a threshold), done with better signal processing:
//   1. one-pole high-pass (~250 Hz) removes rumble / AC hum
//   2. per-frame RMS -> dB envelope (16 ms frames)
//   3. adaptive noise floor tracks the quiet ambient level
//   4. burst shape check: a bark is a fast attack lasting ~50–380 ms
//   5. confirmation: >= BARKS_TO_CONFIRM bursts within BARK_WINDOW_MS
//      (dogs bark in sequences; a door slam is one event)
//
// For a single dog in your own yard this is plenty. If false positives become
// annoying, swap this class for a different detector — keep the same public
// interface.

class BarkDetector {
public:
    static const int HIST_LEN = 40;   // ~640 ms of envelope for the web meter

    void begin() { reset(); }

    void reset() {
        _prevX = 0;
        _prevY = 0;
        _noiseDb = NOISE_FLOOR_DB;
        _state = IDLE;
        _burstFrames = 0;
        _quietFrames = 0;
        memset(_recentBarks, 0, sizeof(_recentBarks));
        _barkIdx = 0;
        _eventLatched = false;
        for (int i = 0; i < HIST_LEN; i++) _hist[i] = -120.0f;
        _histIdx = 0;
    }

    void setMargin(float db) { _marginDb = db; }
    float marginDb() const { return _marginDb; }

    // Feed exactly FRAME_SAMPLES of 16-bit PCM.
    void processFrame(const int16_t *pcm, uint32_t nowMs) {
        // 1. high-pass + RMS over the frame
        float sumSq = 0;
        for (size_t i = 0; i < FRAME_SAMPLES; i++) {
            float x = (float)pcm[i];
            float y = x - _prevX + _hpA * _prevY;   // one-pole high-pass
            _prevX = x;
            _prevY = y;
            sumSq += y * y;
        }
        float rms = sqrtf(sumSq / (float)FRAME_SAMPLES);
        float db = 20.0f * log10f(rms / 32768.0f);
        if (db < -120.0f) db = -120.0f;
        _lastFrameDb = db;

        // history ring for the web meter (oldest at _hist[_histIdx])
        _hist[_histIdx] = db;
        _histIdx = (_histIdx + 1) % HIST_LEN;

        float threshold = _noiseDb + _marginDb;
        bool above = db > threshold;

        // 2. adaptive noise floor — only track during quiet frames
        if (!above) {
            float coef = (db > _noiseDb) ? NOISE_RISE_COEF : NOISE_FALL_COEF;
            _noiseDb += (db - _noiseDb) * coef;
            if (_noiseDb < NOISE_FLOOR_DB) _noiseDb = NOISE_FLOOR_DB;
        }

        // 3. burst state machine
        bool wellBelow = db < (threshold - BURST_DECAY_DB);
        if (_state == IDLE) {
            if (above) {
                _state = IN_BURST;
                _burstFrames = 1;
                _quietFrames = 0;
            }
        } else { // IN_BURST
            if (wellBelow) _quietFrames++;
            else           _quietFrames = 0;
            _burstFrames++;
            bool ended = (_quietFrames >= BURST_DECAY_FRAMES) ||
                         (_burstFrames > (uint32_t)(BURST_MAX_FRAMES * 3));
            if (ended) {
                // A real bark: fast attack, ~50–380 ms of energy.
                if (_burstFrames >= BURST_MIN_FRAMES && _burstFrames <= (uint32_t)BURST_MAX_FRAMES) {
                    registerBurst(nowMs);
                }
                _state = IDLE;
                _burstFrames = 0;
                _quietFrames = 0;
            }
        }
    }

    // True once per confirmed bark sequence (auto-clears).
    bool eventDetected() {
        bool e = _eventLatched;
        _eventLatched = false;
        return e;
    }

    float noiseDb() const { return _noiseDb; }
    float thresholdDb() const { return _noiseDb + _marginDb; }
    float lastFrameDb() const { return _lastFrameDb; }
    const float *hist() const { return _hist; }
    int histIdx() const { return _histIdx; }

private:
    void registerBurst(uint32_t nowMs) {
        _recentBarks[_barkIdx] = nowMs;
        _barkIdx = (_barkIdx + 1) % RECENT_BARKS;
        uint32_t count = 0;
        for (int i = 0; i < RECENT_BARKS; i++) {
            uint32_t t = _recentBarks[i];
            if (t != 0 && nowMs >= t && (nowMs - t) <= BARK_WINDOW_MS) count++;
        }
        if (count >= BARKS_TO_CONFIRM) _eventLatched = true;
    }

    enum State { IDLE, IN_BURST };
    static const int RECENT_BARKS = 8;

    float _hpA = 1.0f / (1.0f + 2.0f * 3.14159265f * HP_CUTOFF_HZ / (float)SAMPLE_RATE);
    float _prevX = 0, _prevY = 0;
    float _noiseDb = NOISE_FLOOR_DB;
    float _marginDb = THRESHOLD_MARGIN_DB;
    State _state = IDLE;
    uint32_t _burstFrames = 0, _quietFrames = 0;
    uint32_t _recentBarks[RECENT_BARKS] = {0};
    int _barkIdx = 0;
    bool _eventLatched = false;
    float _lastFrameDb = -120.0f;
    float _hist[HIST_LEN];
    int _histIdx = 0;
};
