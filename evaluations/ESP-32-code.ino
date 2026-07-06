// =============================================================================
//  ECG_BLE_Monitor_v4.ino
//  AD8232 + ESP32-WROOM-32
//  Arduino IDE 2.x  |  Board: ESP32 Dev Module
//
//  FIXES vs v3 (driven by BPM=41 stuck + raw mV negative + SQ flicker):
//
//  [V9]  DC offset auto-calibration: measure real midpoint during warmup
//         instead of hardcoded 1650 mV — fixes raw mV always negative
//  [V10] Pan-Tompkins threshold LOWERED to 0.30 + refractory reduced to
//         200ms — fixes every-other-beat miss that caused BPM=41 (half rate)
//  [V11] SQ RMS window extended to 150 samples (300ms) — fixes GOOD/NOISY
//         flicker caused by R-wave peaks inflating a 10-sample window
//  [V12] BPM outlier filter: if new BPM differs >40% from running average,
//         discard it instead of resetting ring buffer (more robust)
//  [V13] Added QRS inter-beat interval (IBI) print in Serial for manual
//         verification: count seconds between <<QRS!>> markers yourself
//  [V14] Refractory period now adaptive: 60% of last valid RRI (auto-adjusts
//         to the person's actual heart rate instead of fixed 250ms)
// =============================================================================

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <esp_adc_cal.h>

// =============================================================================
// PINS
// NOTE: GPIO 34/35/36 input-only — external 10kΩ pull-down on LO+/LO- to GND
// =============================================================================
#define ECG_PIN    36
#define LO_PLUS    35
#define LO_MINUS   34

// =============================================================================
// BLE
// =============================================================================
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLEServer*         pServer         = nullptr;
BLECharacteristic* pCharacteristic = nullptr;
bool               deviceConnected = false;

// =============================================================================
// TIMING
// =============================================================================
const uint32_t SAMPLING_RATE        = 500;
const uint32_t SAMPLING_INTERVAL_US = 2000UL;   // 1 000 000 / 500
const uint32_t SAMPLES_PER_PKT      = 10;

unsigned long prevSampleUs = 0;
unsigned long lastNotifyMs = 0;
unsigned long lastSerialMs = 0;

// =============================================================================
// DOUBLE BUFFER
// =============================================================================
volatile int16_t ecgBuf[2][SAMPLES_PER_PKT];
volatile uint8_t fillBuf  = 0;
volatile uint8_t sendBuf  = 1;
volatile bool    bufReady = false;
volatile uint8_t bufIdx   = 0;

// =============================================================================
// BLE PACKET (26 bytes)
//  [0-1]  uint16 LE  seq
//  [2-21] 10×int16   samples (mV×10)
//  [22]   uint8      electrodes (1=ON)
//  [23]   uint8      BPM
//  [24]   uint8      qrs_flag
//  [25]   uint8      XOR checksum
// =============================================================================
#define PKT_SIZE 26
uint16_t pktSeq  = 0;
uint32_t pktSent = 0;

// =============================================================================
// ADC CALIBRATION
// =============================================================================
esp_adc_cal_characteristics_t adcChars;

// =============================================================================
// [V9] DC OFFSET AUTO-CALIBRATION
//  During warmup, accumulate the mean of raw ADC voltage.
//  AD8232 output rests at VCC/2 = ~1650 mV but varies ±50-100 mV per board.
//  We measure it live and use the real midpoint.
// =============================================================================
float    dcOffset        = 1650.0f;   // initial guess — overwritten after warmup
double   dcAccum         = 0.0;       // accumulator (double for precision)
uint32_t dcAccumCount    = 0;
bool     dcCalibrated    = false;

// =============================================================================
// SIGNAL STATE
// =============================================================================
float ecg_mV        = 0.0f;
float ecg_raw_mV    = 0.0f;
bool  electrodesConnected = true;
int   heartRateBPM  = 0;
bool  qrsDetectedFlag = false;
bool  qrsJustFired    = false;
uint32_t lastIBI_ms   = 0;   // [V13] last inter-beat interval in ms

// =============================================================================
// BASELINE WANDER REMOVAL — HP IIR Fc=0.5 Hz @ 500 Hz  [F8]
// =============================================================================
float hp_x1 = 0.0f, hp_y1 = 0.0f;
inline float removeBaseline(float x) {
    float y = x - hp_x1 + 0.9937f * hp_y1;
    hp_x1 = x; hp_y1 = y;
    return y;
}

// =============================================================================
// 50 Hz NOTCH FILTER — IIR biquad @ 500 Hz  [F7]
// =============================================================================
float nf_x1=0, nf_x2=0, nf_y1=0, nf_y2=0;
inline float notch50Hz(float x) {
    float y = 0.9695f*x - 1.7329f*nf_x1 + 0.9695f*nf_x2
              + 1.7329f*nf_y1 - 0.9391f*nf_y2;
    nf_x2=nf_x1; nf_x1=x;
    nf_y2=nf_y1; nf_y1=y;
    return y;
}

// =============================================================================
// [F9+V1+V2+V10+V12+V14] PAN-TOMPKINS QRS DETECTOR v4
//
//  v4 changes:
//  [V10] threshold multiplier = 0.30 (was 0.45 — too high, missed every 2nd beat)
//  [V10] initial refractory   = 200 ms
//  [V12] BPM outlier filter: discard if >40% away from running average
//  [V14] adaptive refractory: 60% of last valid RRI (tracks actual heart rate)
// =============================================================================
#define PT_WIN        15      // 30 ms integration window @ 500 Hz
#define PT_WARMUP     1000    // 2 s @ 500 Hz before reporting
#define PT_RRI_BUF    6       // average over last 6 RRI values

float    ptIntBuf[PT_WIN] = {0};
int      ptIntIdx    = 0;
float    ptSPK       = 0.0f;
float    ptNPK       = 0.0f;
float    ptPrev1     = 0.0f;
float    ptPrev2     = 0.0f;
float    ptMaxSeen   = 0.0f;
uint32_t ptSampleCnt = 0;
bool     ptReady     = false;

unsigned long lastQRSms    = 0;
uint32_t      refractoryMs = 200;   // [V14] adaptive, starts at 200 ms

// [V8/V12] RRI ring buffer
uint32_t rriBuf[PT_RRI_BUF] = {0};
uint8_t  rriHead  = 0;
uint8_t  rriCount = 0;

// Compute average RRI from ring buffer
uint32_t avgRRI() {
    if (rriCount == 0) return 800;  // default ~75 BPM
    uint32_t s = 0;
    for (uint8_t i = 0; i < rriCount; i++) s += rriBuf[i];
    return s / rriCount;
}

bool panTompkins(float sample, unsigned long nowMs) {
    ptSampleCnt++;

    // Derivative
    float deriv = (sample - ptPrev2) / 2.0f;
    ptPrev2 = ptPrev1; ptPrev1 = sample;

    // Square
    float sq = deriv * deriv;

    // Moving window integration
    ptIntBuf[ptIntIdx % PT_WIN] = sq;
    ptIntIdx++;
    float mwi = 0.0f;
    for (uint8_t i = 0; i < PT_WIN; i++) mwi += ptIntBuf[i];
    mwi /= PT_WIN;

    // Warmup: observe max, seed peaks, calibrate DC offset
    if (!ptReady) {
        if (mwi > ptMaxSeen) ptMaxSeen = mwi;
        if (ptSampleCnt >= PT_WARMUP) {
            ptSPK   = ptMaxSeen * 0.55f;
            ptNPK   = ptMaxSeen * 0.10f;
            ptReady = true;
            Serial.printf("[QRS] Warm-up done. ptSPK=%.2f ptNPK=%.2f dcOffset=%.1f mV\n",
                          ptSPK, ptNPK, dcOffset);
        }
        return false;
    }

    // [V10] threshold multiplier 0.30 — lower = more sensitive = catches every beat
    float threshold = ptNPK + 0.30f * (ptSPK - ptNPK);

    bool qrsFound = false;
    if (mwi > threshold) {
        ptSPK = 0.125f * mwi + 0.875f * ptSPK;

        if (nowMs - lastQRSms > refractoryMs) {
            if (lastQRSms > 0) {
                uint32_t rri = nowMs - lastQRSms;

                // [V12] Outlier filter: accept only if within 40% of running avg
                uint32_t avg = avgRRI();
                bool outlier = (rriCount >= 2) &&
                               ((rri < avg * 60 / 100) || (rri > avg * 140 / 100));

                if (!outlier) {
                    rriBuf[rriHead % PT_RRI_BUF] = rri;
                    rriHead++;
                    if (rriCount < PT_RRI_BUF) rriCount++;

                    uint32_t newAvg = avgRRI();
                    int bpm = (int)(60000UL / newAvg);

                    if (bpm >= 30 && bpm <= 200) {
                        heartRateBPM = bpm;
                        lastIBI_ms   = rri;  // [V13]

                        // [V14] adaptive refractory = 60% of last RRI
                        // (ensures we don't block during tachycardia or bradycardia)
                        refractoryMs = (uint32_t)(newAvg * 60 / 100);
                        refractoryMs = constrain(refractoryMs, 150, 500);
                    }
                }
                // Even outlier beats update lastQRSms to avoid cascading misses
            }
            lastQRSms = nowMs;
            qrsFound  = true;
        }
    } else {
        ptNPK = 0.125f * mwi + 0.875f * ptNPK;
    }
    return qrsFound;
}

// =============================================================================
// [V11] SIGNAL QUALITY — RMS over 150 samples (300 ms)
//  Excludes samples during/after QRS (100ms window) to not penalise R-peaks
// =============================================================================
#define SQ_WIN 150
float    sqBuf[SQ_WIN]  = {0};
uint16_t sqIdx          = 0;
uint8_t  sqQRShold      = 0;    // hold counter after QRS (ignore R-peak samples)

void sqUpdate(float sample, bool qrs) {
    if (qrs) sqQRShold = 50;    // blank 50 samples (100ms) after each QRS
    if (sqQRShold > 0) { sqQRShold--; sqBuf[sqIdx % SQ_WIN] = 0; }
    else                sqBuf[sqIdx % SQ_WIN] = sample;
    sqIdx++;
}

const char* signalQuality() {
    if (!electrodesConnected) return "LEADS-OFF";
    float sumSq = 0;
    for (uint16_t i = 0; i < SQ_WIN; i++) sumSq += sqBuf[i] * sqBuf[i];
    float rms = sqrtf(sumSq / SQ_WIN);
    // <15 mV RMS noise (post-QRS-blank) = GOOD; 15-30 = FAIR; >30 = NOISY
    if (rms < 15.0f) return "GOOD  ";
    if (rms < 30.0f) return "FAIR  ";
    return "NOISY ";
}

// =============================================================================
// BLE CALLBACKS
// =============================================================================
class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer* pSrv) override {
        deviceConnected = true;
        Serial.println("[BLE] Client connected.");
    }
    void onDisconnect(BLEServer* pSrv) override {
        deviceConnected = false;
        Serial.println("[BLE] Disconnected — restarting advertising.");
        delay(500);
        pSrv->startAdvertising();
    }
};

// =============================================================================
// SEND BINARY BLE PACKET
// =============================================================================
void sendBLEPacket(uint8_t bank) {
    uint8_t pkt[PKT_SIZE] = {0};
    pkt[0] = pktSeq & 0xFF;
    pkt[1] = (pktSeq >> 8) & 0xFF;
    pktSeq++;

    for (uint8_t i = 0; i < SAMPLES_PER_PKT; i++) {
        int16_t v = ecgBuf[bank][i];
        pkt[2 + i*2]     = (uint8_t)(v & 0xFF);
        pkt[2 + i*2 + 1] = (uint8_t)((v >> 8) & 0xFF);
    }
    pkt[22] = electrodesConnected ? 0x01 : 0x00;
    pkt[23] = (uint8_t)constrain(heartRateBPM, 0, 250);
    pkt[24] = qrsDetectedFlag ? 0x01 : 0x00;

    uint8_t ck = 0;
    for (uint8_t i = 0; i < PKT_SIZE-1; i++) ck ^= pkt[i];
    pkt[25] = ck;

    pCharacteristic->setValue(pkt, PKT_SIZE);
    pCharacteristic->notify();
    qrsDetectedFlag = false;
    pktSent++;
}

// =============================================================================
// SETUP
// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(800);
    Serial.println(F("\n=== ECG BLE Monitor v4 | AD8232 + ESP32 ==="));
    Serial.println(F("  Fixes: DC auto-cal | PT threshold 0.30 | adaptive refractory"));
    Serial.println(F("  SQ window 300ms+QRS-blank | BPM outlier filter | IBI display"));
    Serial.println(F("  First 2 seconds: [WARMUP] — do not move electrodes\n"));

    esp_adc_cal_value_t ct = esp_adc_cal_characterize(
        ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adcChars);
    Serial.printf("[ADC] Calibration: %s\n",
        ct == ESP_ADC_CAL_VAL_EFUSE_TP   ? "Two-Point eFuse (best)"  :
        ct == ESP_ADC_CAL_VAL_EFUSE_VREF ? "Vref eFuse (good)"       :
                                            "Default (acceptable)");

    pinMode(LO_PLUS,  INPUT);
    pinMode(LO_MINUS, INPUT);
    analogReadResolution(12);
    analogSetAttenuation(ADC_11db);

    BLEDevice::init("ESP32_ECG_v4");
    BLEDevice::setMTU(185);
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService* pSvc = pServer->createService(SERVICE_UUID);
    pCharacteristic  = pSvc->createCharacteristic(
        CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_NOTIFY);
    pCharacteristic->addDescriptor(new BLE2902());
    pSvc->start();
    pServer->getAdvertising()->start();
    Serial.println(F("[BLE] Advertising as 'ESP32_ECG_v4'\n"));
    Serial.println(F("Col: [ filt mV ] [ raw mV ] [ BPM ] [ IBI ms ] [ SQ ] [ pkts ] [ tag ]"));
    Serial.println(F("-------------------------------------------------------------------------\n"));

    prevSampleUs = micros();
}

// =============================================================================
// MAIN LOOP
// =============================================================================
void loop() {
    unsigned long nowUs = micros();
    unsigned long nowMs = millis();

    // =========================================================================
    // 1) SAMPLE AT 500 Hz
    // =========================================================================
    if (nowUs - prevSampleUs >= SAMPLING_INTERVAL_US) {
        prevSampleUs += SAMPLING_INTERVAL_US;

        uint32_t raw    = analogRead(ECG_PIN);
        uint32_t voltMv = esp_adc_cal_raw_to_voltage(raw, &adcChars);

        // [V9] Accumulate DC offset during warmup, use calibrated value after
        if (!dcCalibrated) {
            dcAccum += (double)voltMv;
            dcAccumCount++;
            if (dcAccumCount >= PT_WARMUP) {           // 2 s of samples
                dcOffset    = (float)(dcAccum / dcAccumCount);
                dcCalibrated = true;
                Serial.printf("[DC]  Measured midpoint: %.1f mV (was 1650.0 mV)\n", dcOffset);
            }
        }

        float rawSample = (float)voltMv - dcOffset;   // [V9] calibrated centre
        ecg_raw_mV = rawSample;

        // Filter chain
        float sample = removeBaseline(rawSample);
        sample       = notch50Hz(sample);
        ecg_mV       = sample;

        // QRS
        bool qrs = panTompkins(sample, nowMs);
        if (qrs) {
            qrsDetectedFlag = true;
            qrsJustFired    = true;
        }

        // [V11] Signal quality update (QRS-blanked)
        sqUpdate(sample, qrs);

        // Leads-off
        electrodesConnected = !(digitalRead(LO_PLUS) || digitalRead(LO_MINUS));

        // Double-buffer fill
        ecgBuf[fillBuf][bufIdx] = (int16_t)(sample * 10.0f);
        bufIdx++;
        if (bufIdx >= SAMPLES_PER_PKT) {
            bufIdx   = 0;
            sendBuf  = fillBuf;
            fillBuf ^= 1;
            bufReady = true;
        }
    }

    // =========================================================================
    // 2) BLE NOTIFY at 50 Hz
    // =========================================================================
    if (deviceConnected && bufReady && (nowMs - lastNotifyMs >= 20)) {
        lastNotifyMs = nowMs;
        bufReady     = false;
        sendBLEPacket(sendBuf);
    }

    // =========================================================================
    // 3) SERIAL at 10 Hz
    // =========================================================================
    if (nowMs - lastSerialMs >= 100) {
        lastSerialMs = nowMs;

        const char* tag = "";
        if (!ptReady)        tag = " [WARMUP]";
        else if (qrsJustFired) { tag = " <<QRS!>>"; qrsJustFired = false; }

        Serial.printf("[ECG] %7.1f mV | raw %7.1f mV | BPM: %3d | IBI: %4lu ms | SQ: %s | pkts: %5lu%s\n",
                      ecg_mV,
                      ecg_raw_mV,
                      heartRateBPM,
                      (unsigned long)lastIBI_ms,
                      signalQuality(),
                      pktSent,
                      tag);
    }
}

// =============================================================================
// VALIDATION GUIDE
// ─────────────────────────────────────────────────────────────────────────────
// STEP 1 — Warmup (first 2 seconds)
//   Expect: "[WARMUP]" tag. BPM=0. Do NOT move — DC calibration happening.
//
// STEP 2 — After warmup message appears:
//   "[QRS] Warm-up done. ptSPK=X ptNPK=Y dcOffset=Z mV"
//   "[DC]  Measured midpoint: ZZZZ mV"
//   Healthy dcOffset should be 1550–1750 mV. Outside this → check wiring.
//
// STEP 3 — Normal operation. Target values:
//   BPM        : 55–85 (resting adult)
//   IBI ms     : 700–1100 ms (= 60000/BPM)
//   SQ         : GOOD (most of the time)
//   <<QRS!>>   : appears every IBI ms — count with a stopwatch to verify
//   raw mV     : oscillates ±10–60 mV around 0
//   filt mV    : quieter than raw, same rhythm, peaks 80–200 mV
//
// STEP 4 — IBI manual validation (no clinical equipment needed):
//   1. Note timestamp when you see <<QRS!>>
//   2. Note timestamp of NEXT <<QRS!>>
//   3. IBI column should match the difference
//   4. BPM = 60000 / IBI — verify against your phone pulse-ox app
//
// PYTHON BLE DECODER:
//   import struct
//   seq, *s, elec, bpm, qrs, ck = struct.unpack('<H10hBBBB', pkt)
//   ecg_mv = [v/10.0 for v in s]
// =============================================================================
