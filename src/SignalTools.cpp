#include "SignalTools.h"

#include <SPI.h>

#include "MenuSystem.h"
#include "PepeDraw.h"
#include "Pins.h"
#include "SoundUtils.h"

static constexpr uint8_t CC1101_READ_SINGLE = 0x80;
static constexpr uint8_t CC1101_READ_BURST = 0xC0;
static constexpr uint8_t CC1101_IOCFG0 = 0x02;
static constexpr uint8_t CC1101_PARTNUM = 0x30;
static constexpr uint8_t CC1101_VERSION = 0x31;
static constexpr uint8_t CC1101_RSSI = 0x34;
static constexpr uint8_t CC1101_MARCSTATE = 0x35;
static constexpr uint8_t CC1101_SRES = 0x30;
static constexpr uint8_t CC1101_SRX = 0x34;
static constexpr uint8_t CC1101_SIDLE = 0x36;

static constexpr uint8_t CC1101_GDO_CARRIER_SENSE = 0x0E;

static constexpr int IR_LEDC_CHANNEL = 1;
static constexpr int IR_LEDC_FREQ = 38000;
static constexpr int IR_LEDC_RESOLUTION = 8;
static constexpr int IR_LEDC_DUTY = 85;
static constexpr uint16_t IR_RAW_MAX = 768;
static constexpr uint32_t IR_CAPTURE_TIMEOUT_MS = 10000;
static constexpr uint32_t IR_CAPTURE_GAP_US = 25000;
static constexpr uint32_t IR_CAPTURE_MAX_US = 180000;
static constexpr uint32_t IR_MIN_PULSE_US = 250;
static constexpr uint32_t IR_START_MIN_US = 1800;
static constexpr uint32_t IR_START_MAX_US = 20000;

static SPISettings cc1101SpiSettings(4000000, MSBFIRST, SPI_MODE0);
static uint16_t lastIrRaw[IR_RAW_MAX];
static uint16_t lastIrRawCount = 0;
static bool lastIrOverflow = false;

static void irRxMode() {
    ledcWrite(IR_LEDC_CHANNEL, 0);
    ledcDetachPin(IR_TX_PIN);
    pinMode(IR_TX_PIN, INPUT);
    pinMode(IR_RX_PIN, INPUT);
}

static void waitOkReleased() {
    while (digitalRead(BTN_OK) == LOW) delay(5);
    delay(60);
}

static bool cc1101Select() {
    SPI.beginTransaction(cc1101SpiSettings);
    digitalWrite(CC1101_CSN_PIN, LOW);

    unsigned long start = millis();
    while (digitalRead(MISO_PIN) == HIGH && millis() - start < 20) {
        delayMicroseconds(50);
    }

    return digitalRead(MISO_PIN) == LOW;
}

static void cc1101Deselect() {
    digitalWrite(CC1101_CSN_PIN, HIGH);
    SPI.endTransaction();
}

static uint8_t cc1101ReadRegister(uint8_t reg, bool* ready = nullptr) {
    bool isReady = cc1101Select();
    uint8_t readMode = (reg >= 0x30) ? CC1101_READ_BURST : CC1101_READ_SINGLE;
    SPI.transfer(reg | readMode);
    uint8_t value = SPI.transfer(0x00);
    cc1101Deselect();

    if (ready) *ready = isReady;
    return value;
}

static void cc1101WriteRegister(uint8_t reg, uint8_t value) {
    cc1101Select();
    SPI.transfer(reg);
    SPI.transfer(value);
    cc1101Deselect();
}

static void cc1101Strobe(uint8_t command) {
    cc1101Select();
    SPI.transfer(command);
    cc1101Deselect();
}

static bool cc1101PrepareMonitor() {
    bool ready = false;
    cc1101Strobe(CC1101_SRES);
    delay(2);
    cc1101WriteRegister(CC1101_IOCFG0, CC1101_GDO_CARRIER_SENSE);
    cc1101Strobe(CC1101_SIDLE);
    delay(1);
    cc1101Strobe(CC1101_SRX);
    cc1101ReadRegister(CC1101_PARTNUM, &ready);
    return ready;
}

static int cc1101RssiDbm(uint8_t raw) {
    int rssi = raw;
    if (rssi >= 128) rssi -= 256;
    return (rssi / 2) - 74;
}

static void irTxBegin() {
    pinMode(IR_TX_PIN, OUTPUT);
    digitalWrite(IR_TX_PIN, LOW);
    ledcSetup(IR_LEDC_CHANNEL, IR_LEDC_FREQ, IR_LEDC_RESOLUTION);
    ledcAttachPin(IR_TX_PIN, IR_LEDC_CHANNEL);
    ledcWrite(IR_LEDC_CHANNEL, 0);
}

static void irTxEnd() {
    ledcWrite(IR_LEDC_CHANNEL, 0);
    ledcDetachPin(IR_TX_PIN);
    irRxMode();
}

static void irMark(uint32_t usec) {
    ledcWrite(IR_LEDC_CHANNEL, IR_LEDC_DUTY);
    delayMicroseconds(usec);
}

static void irSpace(uint32_t usec) {
    ledcWrite(IR_LEDC_CHANNEL, 0);
    delayMicroseconds(usec);
}

static void sendIrRaw(const uint16_t* raw, uint16_t count) {
    if (!raw || count == 0) return;

    irTxBegin();
    for (uint16_t i = 0; i < count; i++) {
        if ((i % 2) == 0) irMark(raw[i]);
        else irSpace(raw[i]);
    }
    irTxEnd();
}

static void drawToolFrame(const char* title) {
    tft.fillScreen(TFT_BLACK);
    tft.drawRect(0, 0, 320, 240, TFT_WHITE);
    tft.drawFastHLine(0, 34, 320, TFT_WHITE);
    tft.drawFastHLine(0, 212, 320, TFT_WHITE);
    drawStringBig(10, 8, title, TFT_WHITE, 1);
    drawStringCustom(10, 220, "OK: RETURN", TFT_WHITE, 1);
}

static void runHardwareDiag() {
    irRxMode();
    pinMode(CC1101_CSN_PIN, OUTPUT);
    digitalWrite(CC1101_CSN_PIN, HIGH);
    pinMode(CC1101_GDO0_PIN, INPUT);
    bool monitorReady = cc1101PrepareMonitor();

    drawToolFrame("SIGNAL DIAG");

    bool ready = false;
    uint8_t partnum = cc1101ReadRegister(CC1101_PARTNUM, &ready);
    uint8_t version = cc1101ReadRegister(CC1101_VERSION);
    uint8_t marc = cc1101ReadRegister(CC1101_MARCSTATE);
    uint8_t rssiRaw = cc1101ReadRegister(CC1101_RSSI);

    drawStringCustom(12, 50, "IR UNIT", TFT_CYAN, 2);
    drawStringCustom(22, 74, "OUT/TX GPIO: " + String(IR_TX_PIN), TFT_WHITE, 1);
    drawStringCustom(22, 90, "IN/RX GPIO : " + String(IR_RX_PIN), TFT_WHITE, 1);
    drawStringCustom(22, 106, "IN LEVEL   : " + String(digitalRead(IR_RX_PIN)), TFT_YELLOW, 1);

    drawStringCustom(12, 132, "CC1101", TFT_CYAN, 2);
    drawStringCustom(22, 156, "CSN GPIO: " + String(CC1101_CSN_PIN), TFT_WHITE, 1);
    drawStringCustom(22, 172, "GDO0 GPIO: " + String(CC1101_GDO0_PIN), TFT_WHITE, 1);
    drawStringCustom(22, 188, "SPI READY: " + String((ready && monitorReady) ? "YES" : "NO"),
                     (ready && monitorReady) ? TFT_GREEN : TFT_RED, 1);

    drawStringCustom(170, 74, "PART: 0x" + String(partnum, HEX), TFT_WHITE, 1);
    drawStringCustom(170, 90, "VER : 0x" + String(version, HEX), TFT_WHITE, 1);
    drawStringCustom(170, 106, "MARC: 0x" + String(marc, HEX), TFT_WHITE, 1);
    drawStringCustom(170, 122, "RSSI: " + String(cc1101RssiDbm(rssiRaw)) + " dBm", TFT_WHITE, 1);

    while (digitalRead(BTN_OK) == HIGH) delay(20);
    waitOkReleased();
}

static void drawIrRawPreview(uint16_t count, bool overflow) {
    tft.fillRect(1, 36, 318, 175, TFT_BLACK);

    unsigned long total = 0;
    uint16_t minPulse = 65535;
    uint16_t maxPulse = 0;
    for (uint16_t i = 0; i < count; i++) {
        uint16_t value = lastIrRaw[i];
        total += value;
        if (value < minPulse) minPulse = value;
        if (value > maxPulse) maxPulse = value;
    }

    drawStringCustom(12, 46, "Captured timings", TFT_CYAN, 2);
    drawStringCustom(18, 74, "COUNT : " + String(count), TFT_WHITE, 1);
    drawStringCustom(18, 90, "TOTAL : " + String(total / 1000.0f, 1) + " ms", TFT_WHITE, 1);
    drawStringCustom(18, 106, "MIN/MAX: " + String(minPulse) + "/" + String(maxPulse) + " us", TFT_WHITE, 1);
    drawStringCustom(18, 122, overflow ? "BUFFER: FULL" : "BUFFER: OK",
                     overflow ? TFT_RED : TFT_GREEN, 1);

    drawStringCustom(12, 146, "First timings us:", TFT_YELLOW, 1);
    for (uint16_t i = 0; i < count && i < 12; i++) {
        int x = 14 + (i % 4) * 76;
        int y = 164 + (i / 4) * 14;
        drawStringCustom(x, y, String(lastIrRaw[i]), TFT_WHITE, 1);
    }
}

static bool captureIrRaw() {
    lastIrRawCount = 0;
    lastIrOverflow = false;

    unsigned long waitStart = millis();
    bool started = false;
    while (!started) {
        if (digitalRead(BTN_OK) == LOW) {
            waitOkReleased();
            return false;
        }
        if (millis() - waitStart > IR_CAPTURE_TIMEOUT_MS) {
            return false;
        }

        if (digitalRead(IR_RX_PIN) == LOW) {
            uint32_t lowStart = micros();
            while (digitalRead(IR_RX_PIN) == LOW) {
                if (digitalRead(BTN_OK) == LOW) {
                    waitOkReleased();
                    return false;
                }
                if (millis() - waitStart > IR_CAPTURE_TIMEOUT_MS) {
                    return false;
                }
            }

            uint32_t lowDuration = micros() - lowStart;
            if (lowDuration >= IR_START_MIN_US && lowDuration <= IR_START_MAX_US) {
                if (lowDuration > 65535) lowDuration = 65535;
                lastIrRaw[lastIrRawCount++] = static_cast<uint16_t>(lowDuration);
                started = true;
                break;
            }
        }

        delay(1);
    }

    int currentLevel = HIGH;
    uint32_t lastChange = micros();
    uint32_t lastEdge = lastChange;
    uint32_t captureStart = lastChange;

    while ((uint32_t)(micros() - lastEdge) < IR_CAPTURE_GAP_US &&
           (uint32_t)(micros() - captureStart) < IR_CAPTURE_MAX_US) {
        int level = digitalRead(IR_RX_PIN);
        if (level != currentLevel) {
            uint32_t now = micros();
            uint32_t duration = now - lastChange;
            if (duration >= IR_MIN_PULSE_US) {
                if (lastIrRawCount < IR_RAW_MAX) {
                    if (duration > 65535) duration = 65535;
                    lastIrRaw[lastIrRawCount++] = static_cast<uint16_t>(duration);
                } else {
                    lastIrOverflow = true;
                    break;
                }
                currentLevel = level;
                lastChange = now;
                lastEdge = now;
            }
        }
    }

    return lastIrRawCount >= 4 && !lastIrOverflow;
}

static void runIrRawCapture() {
    irRxMode();

    drawToolFrame("IR RAW CAPTURE");
    drawStringCustom(12, 52, "Point remote at IR module.", TFT_CYAN, 1);
    drawStringCustom(12, 70, "Press a remote button now.", TFT_CYAN, 1);
    drawStringCustom(12, 92, "Waiting up to 10 seconds...", TFT_WHITE, 1);

    bool ok = captureIrRaw();
    if (!ok) {
        tft.fillRect(1, 36, 318, 175, TFT_BLACK);
        drawStringCustom(30, 92, "NO IR FRAME", TFT_RED, 2);
        drawStringCustom(22, 122, "Try again closer to receiver.", TFT_WHITE, 1);
    } else {
        drawIrRawPreview(lastIrRawCount, lastIrOverflow);
        Serial.print("[IR] Raw count=");
        Serial.print(lastIrRawCount);
        Serial.print(" data=");
        for (uint16_t i = 0; i < lastIrRawCount; i++) {
            if (i) Serial.print(',');
            Serial.print(lastIrRaw[i]);
        }
        Serial.println();
    }

    while (digitalRead(BTN_OK) == HIGH) delay(20);
    waitOkReleased();
}

static void runIrReplayLast() {
    drawToolFrame("IR REPLAY");

    if (lastIrRawCount == 0) {
        drawStringCustom(30, 92, "NO CAPTURE SAVED", TFT_RED, 2);
        drawStringCustom(22, 122, "Run IR Raw Capture first.", TFT_WHITE, 1);
        while (digitalRead(BTN_OK) == HIGH) delay(20);
        waitOkReleased();
        return;
    }

    drawStringCustom(20, 58, "Ready to replay last frame.", TFT_CYAN, 1);
    drawStringCustom(20, 78, "Timings: " + String(lastIrRawCount), TFT_WHITE, 1);
    drawStringCustom(20, 102, "Tap OK to send once.", TFT_YELLOW, 1);
    drawStringCustom(20, 122, "Hold OK after send to return.", TFT_WHITE, 1);

    while (true) {
        if (digitalRead(BTN_OK) == LOW) {
            sendIrRaw(lastIrRaw, lastIrRawCount);

            tft.fillRect(20, 150, 280, 22, TFT_BLACK);
            drawStringCustom(20, 154, "SENT " + String(lastIrRawCount) + " timings", TFT_GREEN, 1);
            bool held = waitOkReleaseWasLong();
            if (held) return;
        }
        delay(10);
    }
}

static void runIrTxTest() {
    drawToolFrame("IR TX TEST");
    drawStringCustom(20, 58, "Use phone camera to view LEDs.", TFT_CYAN, 1);
    drawStringCustom(20, 82, "Sending 3 carrier bursts...", TFT_WHITE, 1);

    irTxBegin();
    for (int i = 0; i < 3; i++) {
        irMark(120000);
        irSpace(160000);
    }
    irTxEnd();

    drawStringCustom(20, 118, "DONE", TFT_GREEN, 2);
    while (digitalRead(BTN_OK) == HIGH) delay(20);
    waitOkReleased();
}

static void runInputMonitor() {
    irRxMode();

    drawToolFrame("INPUT MONITOR");
    drawStringCustom(12, 48, "IR receiver only. TX is disabled.", TFT_CYAN, 1);
    drawStringCustom(12, 64, "Use remote to check real frames.", TFT_CYAN, 1);

    unsigned long irFrames = 0;
    unsigned long irNoise = 0;
    unsigned long lastDraw = 0;
    bool irLowTracking = false;
    uint32_t irLowStart = 0;

    while (true) {
        int ir = digitalRead(IR_RX_PIN);

        if (ir == LOW && !irLowTracking) {
            irLowTracking = true;
            irLowStart = micros();
        }
        if (ir == HIGH && irLowTracking) {
            uint32_t duration = micros() - irLowStart;
            if (duration >= IR_START_MIN_US && duration <= IR_START_MAX_US) {
                irFrames++;
            } else if (duration >= IR_MIN_PULSE_US) {
                irNoise++;
            }
            irLowTracking = false;
        }

        if (millis() - lastDraw > 120) {
            tft.fillRect(12, 94, 296, 96, TFT_BLACK);
            drawStringCustom(20, 100, "IR IN LEVEL : " + String(ir), TFT_WHITE, 2);
            drawStringCustom(20, 126, "IR FRAMES   : " + String(irFrames), TFT_YELLOW, 1);
            drawStringCustom(20, 142, "IR NOISE    : " + String(irNoise), TFT_DARKGREY, 1);
            lastDraw = millis();
        }

        if (digitalRead(BTN_OK) == LOW) {
            waitOkReleased();
            return;
        }

        delayMicroseconds(100);
    }
}

void runSignalTools() {
    static const char* items[] = {
        "Hardware Diag",
        "Input Monitor",
        "IR Raw Capture",
        "IR Replay Last",
        "IR TX Test"
    };

    bool exitSub = false;
    while (!exitSub) {
        int choice = runSubMenu("SIGNAL TOOLS", items, sizeof(items) / sizeof(char*));
        switch (choice) {
            case -1: exitSub = true;      break;
            case  0: runHardwareDiag();   break;
            case  1: runInputMonitor();   break;
            case  2: runIrRawCapture();   break;
            case  3: runIrReplayLast();   break;
            case  4: runIrTxTest();       break;
        }
    }
}
