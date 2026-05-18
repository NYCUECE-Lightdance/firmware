// Prop controller for Pico 2W.
//
// Frame data uses `time` + `acc0`..`acc7`; a per-player SECTIONS table maps
// each acc slot onto a range of LEDs.
//
// Pins:
//   GP2/3/4 : PROP1/2/3 WS2812 strips
//   GP9     : 3-LED WS2812 status indicator
//   GP17-20 : DIP1..DIP4 (active low)
//   onboard : CYW43 LED (WiFi connect indicator)
//
// Boot:
//   1. read DIPs
//   2. WiFi connect (skipped if DIP2 low). WiFi must init before FastLED
//      -- both fight for PIO state machines.
//   3. init FastLED + LittleFS
//   4. mode select:
//        DIP2 low           -> debug colors, halt
//        DIP1 high          -> download from server, save, halt
//        DIP1 low (default) -> load from flash, enter run loop
//   5. loop: UDP cmds drive playback synced to host timestamps.
//
// DIP4 picks WiFi profile (high=EE219B/DHCP, low=Lightdance/static).
//
// Indicator (GP9):
//   downloading  : chunk# in base-4 across ind[0..2] (0=R,1=G,2=B,3=W),
//                  leading zeros blank
//   download ok  : all 3 blink green
//   download err : all 3 blink red
//   ready / idle : ind[0] solid green
//   playing      : ind[0] breathing green

#include <ArduinoJson.h>
#include <FastLED.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include "hardware/watchdog.h"

// ---- config ----
#define PLAYER_NUM     3  // selects which SECTIONS table below applies
#define PROP1_PIN      2
#define PROP2_PIN      3
#define PROP3_PIN      4
#define IND_PIN        9   // onboard 3-LED WS2812 chain (status indicator)
#define IND_COUNT      3
#define DIP1_PIN       17  // low = load from flash & run             | high = download & halt
#define DIP2_PIN       18  // low = debug mode (offline test colors)  | high = normal boot
#define DIP3_PIN       19  // reserved
#define DIP4_PIN       20  // low = WiFi profile 1 (Lightdance)       | high = profile 0 (EE219B)

#define NUM_ACC        8        // acc0..acc7
#define NUM_COLS       (1 + NUM_ACC)  // time + acc0..acc7
#define MAX_FRAMES     1024
#define CHUNK_SIZE     10
#define NUM_CHUNKS     60
#define UDP_RX_PORT    12345
#define UDP_TX_PORT    12346
#define UDP_STOP       1937010544u
#define UDP_HEARTBEAT  1751474546u
#define HEARTBEAT_MS   3000

const char* WIFI_SSID[]  = {"EE219B", "Lightdance"}; //"EE219B"
const char* WIFI_PASS    = "wifiyee219"; //"wifiyee219"
const char* RESPOND_TO[] = {"192.168.0.137", "192.168.1.10"};

// Output -> acc-slot mapping. Each row paints `count` LEDs of strip `strip`,
// starting at `start`, with the data packed for `slot` (0..7 -> acc0..acc7).
//   strip : 0=PROP1 (GP2), 1=PROP2 (GP3), 2=PROP3 (GP4)
//   slot  : 0..7 (column in frames[][] = slot + 1; column 0 is reserved for time)
// STRIP_LENS[i] is the LED count for strip i (must cover every section's range).
struct Section { uint8_t strip, start, count, slot; };


// knife 1
#if PLAYER_NUM == 4
const Section SECTIONS[] = {
    {0, 0, 1, 0}, {0, 4, 1, 0}, {0, 1, 1, 1}, {0, 2, 2, 2},
};
const uint8_t STRIP_LENS[3] = {5, 0, 0};

// knife 2
#elif PLAYER_NUM == 6
const Section SECTIONS[] = {
    {0, 2, 2, 2}, {0, 4, 1, 1}, {1, 0, 2, 0},
};
const uint8_t STRIP_LENS[3] = {5, 3, 0};

// big sword
#elif PLAYER_NUM == 3
const Section SECTIONS[] = {
    {0, 0, 3, 0}, {1, 0, 1, 2}, {1, 1, 1, 0}, {1, 2, 1, 2}, {2, 0, 2, 1}
};
const uint8_t STRIP_LENS[3] = {3, 3, 2};

// umbrella
#elif PLAYER_NUM == 1
const Section SECTIONS[] = {
    {0, 0, 4, 0}, {1, 0, 4, 0}, {2, 0, 2, 1},
};
const uint8_t STRIP_LENS[3] = {4, 4, 2};

#else
#error "Define a SECTIONS table for this PLAYER_NUM in main_props.cpp."
#endif

// Per-strip LED buffers. Sized to the largest count any player needs, so the
// same firmware can be reflashed across players without resizing buffers.
// FastLED uses STRIP_LENS at addLeds() time to set the actual chain length.
#define MAX_LEDS_PER_STRIP 20
CRGB l0[MAX_LEDS_PER_STRIP], l1[MAX_LEDS_PER_STRIP], l2[MAX_LEDS_PER_STRIP];
CRGB* strips[] = {l0, l1, l2};
CRGB ind[IND_COUNT];

// Animation data. frames[i][0] = start tick (50ms units),
// frames[i][1..8] = packed acc0..acc7 with this bit layout:
//   bits 31..8 : 24-bit RGB color (0xRRGGBB)
//   bits  7..4 : brightness 0..15 (gamma-2.2 mapped to 0..255)
//   bit      0 : transition flag (1 = blend linearly into next frame)
uint32_t frames[MAX_FRAMES][NUM_COLS];
int      numFrames   = 0;
int      wifiProfile = 0;
String   deviceId;

WiFiUDP udp;
WiFiClientSecure httpsClient;
HTTPClient       http;

enum State { READY, PLAYING };
State state = READY;
int           frameIdx   = 0;
unsigned long startMs    = 0;
unsigned long lastBeatMs = 0;

// ---- helpers ----
void msg(const String& s) {
    Serial.println(s);
}

[[noreturn]] void halt(const String& s) {
    msg(s);
    while (1) delay(1000);
}

[[noreturn]] void reboot() {
    watchdog_reboot(0, 0, 0);
    while (1);
}

void connectWiFi(int p) {
    msg("WiFi: " + String(WIFI_SSID[p]));
    if (p == 1) WiFi.config(IPAddress(192, 168, 1, 152 + PLAYER_NUM));
    WiFi.begin(WIFI_SSID[p], WIFI_PASS);

    // LED_BUILTIN routes through CYW43, so it is only controllable after
    // WiFi.begin() brings the wireless chip up.
    pinMode(LED_BUILTIN, OUTPUT);

    for (int i = 0; i < 10 && WiFi.status() != WL_CONNECTED; i++) {
        digitalWrite(LED_BUILTIN, i & 1);
        delay(500);
    }

    if (WiFi.status() != WL_CONNECTED) {
        digitalWrite(LED_BUILTIN, LOW);
        msg("WiFi failed");
        delay(2000);
        reboot();
    }

    digitalWrite(LED_BUILTIN, HIGH);
    msg("Connected " + WiFi.localIP().toString());
}

bool downloadChunk(int n) {
    String url = "https://eesa.dece.nycu.edu.tw/lightdance/api/items/eesa3/LATEST/player="
                 + String(PLAYER_NUM) + "/chunk=" + String(n);

    http.begin(httpsClient, url);

    if (http.GET() != 200) {
        http.end();
        return false;
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();
    if (err) return false;

    static const char* KEYS[NUM_COLS] = {
        "time", "acc0", "acc1", "acc2", "acc3", "acc4", "acc5", "acc6", "acc7"
    };

    JsonArray arr = doc["player_data"];
    for (int i = 0; i < (int)arr.size() && i < CHUNK_SIZE; i++) {
        int idx = n * CHUNK_SIZE + i;
        for (int k = 0; k < NUM_COLS; k++) {
            frames[idx][k] = arr[i][KEYS[k]].as<uint32_t>();
        }
        numFrames = idx + 1;
    }

    return true;
}

// On-flash format: [int numFrames][uint32_t frames[MAX_FRAMES][NUM_COLS]].
// Bumping MAX_FRAMES or NUM_COLS invalidates any previously saved /data.bin.
void saveData() {
    File f = LittleFS.open("/data.bin", "w");
    if (!f) return;
    f.write((uint8_t*)&numFrames, sizeof(numFrames));
    f.write((uint8_t*)frames, sizeof(frames));
    f.close();
}

void loadData() {
    File f = LittleFS.open("/data.bin", "r");
    if (!f) return;
    f.read((uint8_t*)&numFrames, sizeof(numFrames));
    f.read((uint8_t*)frames, sizeof(frames));
    f.close();
}

uint8_t brightnessFrom(uint32_t d) {
    return uint8_t(powf(((d >> 4) & 0x0F) / 15.0f, 2.2f) * 255);
}

uint32_t blendColor(uint32_t a, uint32_t b, float p) {
    p = constrain(p, 0.0f, 1.0f);
    CRGB ca((a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
    CRGB cb((b >> 16) & 0xFF, (b >> 8) & 0xFF, b & 0xFF);
    CRGB o = blend(ca, cb, uint8_t(p * 255));
    return (uint32_t(o.r) << 16) | (uint32_t(o.g) << 8) | o.b;
}

void renderFrame() {
    unsigned long t = millis() - startMs;
    for (auto& s : SECTIONS) {
        int      col   = 1 + s.slot;
        uint32_t cur   = frames[frameIdx][col];
        uint32_t color = cur >> 8;
        uint8_t  bri   = brightnessFrom(cur);

        if ((cur & 1) && frameIdx + 1 < numFrames) {
            uint32_t      nxt = frames[frameIdx + 1][col] >> 8;
            unsigned long t0  = frames[frameIdx][0]     * 50;
            unsigned long t1  = frames[frameIdx + 1][0] * 50;
            if (t1 > t0) color = blendColor(color, nxt, float(t - t0) / float(t1 - t0));
        }

        for (int i = 0; i < s.count; i++) {
            CRGB& led = strips[s.strip][s.start + i];
            led = color;
            led.nscale8(bri);
        }
    }
    FastLED.show();
}

// Fill every LED on every configured strip with DBG_COLOR. Uses STRIP_LENS so
// it covers the full chain for this player, not just the LEDs referenced by
// SECTIONS -- handy for catching dead pixels past the last mapped section.
[[noreturn]] void runDebugMode() {
    static const uint32_t DBG_COLOR = 0xFFFFFF;

    for (int s = 0; s < 3; s++) {
        for (int i = 0; i < STRIP_LENS[s]; i++) {
            strips[s][i] = DBG_COLOR;
        }
    }

    FastLED.show();
    while (1) delay(1000);
}

[[noreturn]] void haltBlinking(const String& s, uint32_t color) {
    msg(s);
    bool on = false;
    while (1) {
        on = !on;
        for (int i = 0; i < IND_COUNT; i++) ind[i] = on ? CRGB(color) : CRGB::Black;
        FastLED.show();
        delay(500);
    }
}

// Display n on the 3 indicator LEDs as 3 base-4 digits.
// Per-digit color: 0=red, 1=green, 2=blue, 3=white. Leading zeros are blanked
// (LED off) so e.g. n=5 shows [off, green, green] instead of [red, green, green].
// LED order: ind[0]=MSD, ind[2]=LSD.
void showChunkNumber(int n) {
    static const uint32_t COLORS[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
    uint8_t d[3] = {
        uint8_t((n >> 4) & 3),
        uint8_t((n >> 2) & 3),
        uint8_t(n & 3),
    };

    int firstSig = 2;  // n==0 still shows the LSD
    for (int i = 0; i < 3; i++) {
        if (d[i]) { firstSig = i; break; }
    }

    for (int i = 0; i < IND_COUNT; i++) {
        ind[i] = (i < firstSig) ? CRGB::Black : CRGB(COLORS[d[i]]);
    }
    FastLED.show();
}

int readUDP() {
    if (!udp.parsePacket()) return 0;
    lastBeatMs = millis();

    uint8_t b[4];
    udp.read(b, 4);
    uint32_t cmd = (uint32_t(b[0]) << 24)
                 | (uint32_t(b[1]) << 16)
                 | (uint32_t(b[2]) << 8)
                 |  uint32_t(b[3]);

    if (cmd == UDP_STOP)      return -1;
    if (cmd == UDP_HEARTBEAT) return -2;
    return cmd;
}

void respond(const char* m) {
    String s = deviceId + ": " + m;
    udp.beginPacket(RESPOND_TO[wifiProfile], UDP_TX_PORT);
    udp.write(s.c_str());
    udp.endPacket();
}

// ---- setup / loop ----
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    msg("Starting...");

    pinMode(DIP1_PIN, INPUT_PULLUP);
    pinMode(DIP2_PIN, INPUT_PULLUP);
    pinMode(DIP3_PIN, INPUT_PULLUP);
    pinMode(DIP4_PIN, INPUT_PULLUP);

    bool debugMode = !digitalRead(DIP2_PIN);

    // CRITICAL: WiFi MUST init before FastLED. The CYW43 driver and FastLED's
    // NeoPixel driver both grab PIO state machines (RP2350 has only 8). If
    // FastLED runs first it can starve CYW43, and WiFi.begin() then fails
    // silently with no recovery short of a reboot. Do not reorder.
    if (!debugMode) {
        wifiProfile = digitalRead(DIP4_PIN) ? 0 : 1;
        deviceId    = "prop_p" + String(PLAYER_NUM);
        connectWiFi(wifiProfile);
    }

    // Template arg is the data GPIO (must be a literal). Length comes from the
    // per-player STRIP_LENS table; strips with length 0 are skipped.
    if (STRIP_LENS[0]) FastLED.addLeds<NEOPIXEL, PROP1_PIN>(strips[0], STRIP_LENS[0]);
    if (STRIP_LENS[1]) FastLED.addLeds<NEOPIXEL, PROP2_PIN>(strips[1], STRIP_LENS[1]);
    if (STRIP_LENS[2]) FastLED.addLeds<NEOPIXEL, PROP3_PIN>(strips[2], STRIP_LENS[2]);
    FastLED.addLeds<NEOPIXEL, IND_PIN>(ind, IND_COUNT);
    FastLED.setBrightness(255);
    FastLED.clear(true);

    if (!LittleFS.begin()) {
        LittleFS.format();
        LittleFS.begin();
    }

    if (debugMode) {
        msg("Debug Mode");
        runDebugMode();
    }

    if (!digitalRead(DIP1_PIN)) {
        msg("Loading...");
        loadData();
    } else {
        msg("Downloading...");
        httpsClient.setInsecure();  // server cert not validated
        http.setReuse(true);        // keep TCP/TLS alive across chunk requests
        for (int c = 0; c < NUM_CHUNKS; c++) {
            showChunkNumber(c);
            bool ok = false;
            for (int a = 0; a < 3 && !(ok = downloadChunk(c)); a++) {
                msg("Retry " + String(a + 1) + "/3 chunk " + String(c));
            }
            if (!ok) haltBlinking("Download failed", 0xFF0000);
            msg("Chunk " + String(c));
        }
        saveData();
        haltBlinking("Download success", 0x00FF00);
    }

    udp.begin(UDP_RX_PORT);
    udp.flush();
    lastBeatMs = millis();
    msg("Ready! ip=" + WiFi.localIP().toString());
}

void loop() {
    int cmd = readUDP();

    if (cmd == -1) {
        respond("stopped");
        state = READY;
        frameIdx = 0;
        FastLED.clear(true);
    } else if (cmd == -2) {
        respond("heartbeat received");
    } else if (cmd > 0) {
        // Sync trick: pretend playback "started" (millis() - cmd) ago so the
        // frame index calculation lines up with the host's elapsed ms.
        respond("running");
        startMs = millis() - cmd;
        state = PLAYING;
    }

    if (state == READY) {
        if (millis() - lastBeatMs > 5000) {
            msg("No heartbeat, reboot");
            delay(500);
            reboot();
        }
        ind[0] = CRGB::Green;
        FastLED.show();
    } else {
        // current playback time in 50ms ticks
        int cur = (millis() - startMs) / 50;
        while (frameIdx + 1 < numFrames && frames[frameIdx + 1][0] < (uint32_t)cur) {
            frameIdx++;
        }
        // breathing green: ~2s period via 8-bit sine LUT
        ind[0] = CRGB(0, sin8((millis() >> 3) & 0xFF), 0);
        renderFrame();
    }

    delay(5);
}
