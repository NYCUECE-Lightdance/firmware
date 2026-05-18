// LED costume controller for Pico W.
//
// Boot flow:
//   1. init OLED + read DIP switches
//   2. connect WiFi (skipped if DEBUG_PIN low so unit lights up offline)
//   3. init FastLED + LittleFS
//   4. branch on switches:
//        DEBUG_PIN  (GPIO 18) low  -> show body-part test colors and halt
//        DEBUG_PIN  (GPIO 18) high -> normal boot, continue below
//        SWITCH_PIN (GPIO 17) low  -> load saved animation from flash, enter run loop
//        SWITCH_PIN (GPIO 17) high -> download fresh data from server, save, halt
//                                     (power-cycle with SWITCH_PIN low to play it back)
//   5. run loop: listen on UDP, play back frames synced to host timestamps.
//
// WIFI_PIN (GPIO 20) is read only when DEBUG_PIN is high (i.e. WiFi is needed):
//   high -> profile 0 (SSID "EE219B",     DHCP)
//   low  -> profile 1 (SSID "Lightdance", static IP 192.168.1.{100+PLAYER_NUM})

#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include "hardware/watchdog.h"

// ---- config ----
// PLAYER_NUM identifies this unit; flash a unique value per costume.
// All input pins use INPUT_PULLUP, so "low" = switch closed to GND.
#define PLAYER_NUM     1
#define SDA_PIN        12
#define SCL_PIN        13
#define DEBUG_PIN      18  // low = debug mode (offline test colors)  | high = normal boot
#define SWITCH_PIN     17  // low = load from flash & run             | high = download & halt
#define WIFI_PIN       20  // low = WiFi profile 1 (Lightdance)       | high = profile 0 (EE219B)
#define MAX_FRAMES     1024
#define CHUNK_SIZE     10
#define NUM_CHUNKS     60
#define UDP_RX_PORT    12345
#define UDP_TX_PORT    12346
#define UDP_STOP       1937010544u
#define UDP_HEARTBEAT  1751474546u
#define HEARTBEAT_MS   3000

// Two WiFi profiles, selected by WIFI_PIN at boot. Profile 1 sets a static
// IP (192.168.1.{100+PLAYER_NUM}); profile 0 uses DHCP. RESPOND_TO is the
// host that receives our UDP status replies for that profile.
const char* WIFI_SSID[]  = {"EE219B", "Lightdance"}; //"EE219B"
const char* WIFI_PASS    = "wifiyee219"; //"wifiyee219"
const char* RESPOND_TO[] = {"192.168.0.137", "192.168.1.10"};

// Body-part -> LED mapping. Each row paints `count` LEDs, starting at index
// `start` of `strips[strip]`, with the color packed for body-part `part`.
//   strip : 0..7, index into strips[] (= GPIO 2..9 in addLeds order below)
//   part  : column in frames[][], 1=hat .. 15=board (0 is reserved for time)
// To add a part: append a row here AND a name in KEYS[] at index `part`.
struct Section { uint8_t strip, start, count, part; };
const Section SECTIONS[] = {
    {0,0,3,7}, {0,3,2,2}, {0,5,1,1},   // strip 0 (GPIO 2): tie, face, hat
    {1,0,5,3}, {1,5,2,5}, {1,7,5,9},   // strip 1 (GPIO 3): chestL, armL, gloveL
    {2,0,5,4}, {2,5,2,6}, {2,7,5,10},  // strip 2 (GPIO 4): chestR, armR, gloveR
    {4,0,2,11},{4,2,1,13},             // strip 4 (GPIO 6): legL, shoeL
    {5,0,2,12},{5,2,1,14},             // strip 5 (GPIO 7): legR, shoeR
    {6,0,2,8}, {7,0,1,15},             // strip 6 (GPIO 8): belt | strip 7 (GPIO 9): board
};

// Per-strip LED buffers. Sizes must match the addLeds<> calls in setup().
// Resize these (and the matching addLeds<>) when changing physical LED counts.
CRGB l1[6], l2[12], l3[12], l4[1], l5[3], l6[3], l7[2], l8[1];
CRGB* strips[] = {l1, l2, l3, l4, l5, l6, l7, l8};

// Animation data. frames[i][0] is the start time of frame i in 50ms ticks
// (so frame i fires at frames[i][0] * 50 ms after playback start).
// frames[i][1..15] is packed body-part data, with this bit layout:
//   bits 31..8 : 24-bit RGB color (0xRRGGBB)
//   bits  7..4 : brightness 0..15 (gamma-2.2 mapped to 0..255 at render)
//   bit      0 : transition flag (1 = blend linearly into next frame's color)
uint32_t frames[MAX_FRAMES][16];
int      numFrames   = 0;
int      wifiProfile = 0;
String   deviceId;

Adafruit_SSD1306 oled(128, 64, &Wire, -1);
WiFiUDP udp;
WiFiClientSecure httpsClient;
HTTPClient       http;

enum State { READY, PLAYING };
State state = READY;
int           frameIdx   = 0;
unsigned long startMs    = 0;
unsigned long lastBeatMs = 0;

// ---- helpers ----
// Skips redrawing the OLED if the message hasn't changed -- the I2C refresh
// is slow (~30ms) and dominates the run loop if called every iteration.
String lastMsg;
void msg(const String& s, int sz = 2) {
    if (s == lastMsg) return;
    lastMsg = s;
    Serial.println(s);
    oled.clearDisplay();
    oled.setTextSize(sz);
    oled.setCursor(1, 1);
    oled.println(s);
    oled.display();
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
    msg("WiFi: " + String(WIFI_SSID[p]), 1);
    if (p == 1) WiFi.config(IPAddress(192, 168, 1, 100 + PLAYER_NUM));
    WiFi.begin(WIFI_SSID[p], WIFI_PASS);

    for (int i = 0; i < 10 && WiFi.status() != WL_CONNECTED; i++) {
        delay(500);
    }

    if (WiFi.status() != WL_CONNECTED) {
        msg("WiFi failed");
        delay(2000);
        reboot();
    }

    msg("Connected\n" + WiFi.localIP().toString(), 1);
    delay(1000);
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

    // Index in KEYS[] = column in frames[][]. Order matters: the SECTIONS
    // table's `part` field references these positions directly.
    static const char* KEYS[] = {
        "time","hat","face","chestL","chestR","armL","armR","tie",
        "belt","gloveL","gloveR","legL","legR","shoeL","shoeR","board"
    };

    JsonArray arr = doc["player_data"];
    for (int i = 0; i < (int)arr.size() && i < CHUNK_SIZE; i++) {
        int idx = n * CHUNK_SIZE + i;
        for (int k = 0; k < 16; k++) {
            frames[idx][k] = arr[i][KEYS[k]].as<uint32_t>();
        }
        numFrames = idx + 1;
    }

    return true;
}

// On-flash format: [int numFrames][uint32_t frames[MAX_FRAMES][16]].
// Bumping MAX_FRAMES, the column count, or moving numFrames invalidates
// any previously saved /data.bin -- bump a version byte if you care.
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
        uint32_t cur   = frames[frameIdx][s.part];
        uint32_t color = cur >> 8;
        uint8_t  bri   = brightnessFrom(cur);
        if ((cur & 1) && frameIdx + 1 < numFrames) {
            uint32_t      nxt = frames[frameIdx + 1][s.part] >> 8;
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

// DBG[i] is the test color for body-part i (1=hat .. 15=board); index 0
// is unused. Tweak these to verify wiring -- each body part lights a
// distinct color so you can spot mismatched strip indexes or counts.
[[noreturn]] void runDebugMode() {
    static const uint32_t DBG[] = {
        0, 0xFF3B30, 0xFFD60A, 0x007AFF, 0x5AC8FA, 0x34C759, 0x00E676, 0xFF9500,
        0xFFD700, 0xAF52DE, 0xFF2D55, 0x40E0D0, 0x00CED1, 0x8B4513, 0xD2691E, 0xFFFFFF
    };

    for (auto& s : SECTIONS) {
        for (int i = 0; i < s.count; i++) {
            strips[s.strip][s.start + i] = DBG[s.part];
        }
    }

    FastLED.show();
    while (1) delay(1000);
}

// Inbound UDP protocol (port UDP_RX_PORT): a single 4-byte big-endian uint32.
//   == UDP_STOP      -> stop playback, clear LEDs       (returns -1)
//   == UDP_HEARTBEAT -> just refresh lastBeatMs         (returns -2)
//   else             -> treat as ms timestamp; (re)sync (returns cmd > 0)
// Any received packet also resets the heartbeat clock.
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

    Wire.setSDA(SDA_PIN);
    Wire.setSCL(SCL_PIN);
    Wire.begin();

    oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
    oled.setTextColor(SSD1306_WHITE);
    msg("Starting...");

    pinMode(DEBUG_PIN,  INPUT_PULLUP);
    pinMode(SWITCH_PIN, INPUT_PULLUP);
    pinMode(WIFI_PIN,   INPUT_PULLUP);

    bool debugMode = !digitalRead(DEBUG_PIN);

    // CRITICAL: WiFi MUST init before FastLED. The CYW43 driver and FastLED's
    // NeoPixel driver both grab PIO state machines (RP2350 has only 8). If
    // FastLED runs first it can starve CYW43, and WiFi.begin() then fails
    // silently with no recovery short of a reboot. Do not reorder.
    if (!debugMode) {
        wifiProfile = digitalRead(WIFI_PIN) ? 0 : 1;
        deviceId = "player" + String(PLAYER_NUM);
        connectWiFi(wifiProfile);
    }

    // Template arg is the data GPIO (must be a literal). Length must match
    // the buffer size declared at file scope. Order here = strip index in SECTIONS.
    FastLED.addLeds<NEOPIXEL, 2>(strips[0], 6);
    FastLED.addLeds<NEOPIXEL, 3>(strips[1], 12);
    FastLED.addLeds<NEOPIXEL, 4>(strips[2], 12);
    FastLED.addLeds<NEOPIXEL, 5>(strips[3], 1);
    FastLED.addLeds<NEOPIXEL, 6>(strips[4], 3);
    FastLED.addLeds<NEOPIXEL, 7>(strips[5], 3);
    FastLED.addLeds<NEOPIXEL, 8>(strips[6], 2);
    FastLED.addLeds<NEOPIXEL, 9>(strips[7], 1);
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

    if (!digitalRead(SWITCH_PIN)) {
        msg("Loading...");
        loadData();
    } else {
        msg("Downloading...");
        httpsClient.setInsecure();  // server cert not validated
        http.setReuse(true);        // keep TCP/TLS alive across chunk requests
        for (int c = 0; c < NUM_CHUNKS; c++) {
            bool ok = false;
            for (int a = 0; a < 3 && !(ok = downloadChunk(c)); a++) {
                msg("Retry " + String(a + 1) + "/3\nChunk " + String(c));
            }
            if (!ok) halt("Download failed");
            msg("Chunk " + String(c));
        }
        saveData();
        halt("Download success");
    }

    udp.begin(UDP_RX_PORT);
    udp.flush();
    lastBeatMs = millis();
    msg("Ready!\n" + WiFi.localIP().toString(), 1);
}

// State machine:
//   READY   : idle. Reboots if no UDP traffic (any packet) for 5s.
//   PLAYING : advances frameIdx by elapsed time, renders each loop.
// Transitions:
//   any state + UDP_STOP timestamp  -> READY
//   any state + UDP_HEARTBEAT       -> stays put, just refreshes liveness
//   any state + timestamp (cmd > 0) -> PLAYING (startMs aligned to host clock)
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
            msg("No heartbeat\nReboot");
            delay(500);
            reboot();
        }
        msg("Ready!");
    } else {
        msg(millis() - lastBeatMs > HEARTBEAT_MS ? "No Signal!" : "Playing!");

        // current playback time in 50ms ticks
        int cur = (millis() - startMs) / 50;

        // Advance to the latest frame whose start tick is <= cur. Linear is
        // fine for sequential playback; swap to binary search if you ever
        // seek to arbitrary timestamps.
        while (frameIdx + 1 < numFrames && frames[frameIdx + 1][0] < (uint32_t)cur) {
            frameIdx++;
        }

        renderFrame();
    }

    delay(5);
}
