#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ArduinoJson.h>
#include <EasyButton.h>
#include <FastLED.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Wire.h>
#include <math.h>
#include <WiFiClientSecure.h>
// #include "data.h"

#include "hardware/watchdog.h"

// ============================================================================
// CONFIGURATION
// ============================================================================


#define PLAYER_NUM 0

// Pins
#define SDA_PIN 12      // OLED display data
#define SCL_PIN 13      // OLED display clock
#define DEBUG_PIN 18        // Debug mode: LOW=show test colors
#define SWITCH_PIN 17       // Mode select: HIGH=WiFi download, LOW=load from memory
#define BUTTON_PIN 16       // Manual start button (pull-up)
#define WIFI_PIN 20     // WiFi profile: HIGH=profile 0, LOW=profile 1

// Display
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

// Data
#define MAX_FRAMES 128
#define CHUNK_SIZE 10
#define BODY_PARTS 15
#define NUM_CHUNKS 10

// Network
#define UDP_RX_PORT 12345
#define UDP_TX_PORT 12346
#define UDP_STOP_CMD 1937010544
#define UDP_HEARTBEAT_CMD 1751474546
#define HEARTBEAT_TIMEOUT 3000

// WiFi Settings
const char* WIFI_SSID[] = {"EE219B", "Lightdance"};      // "EE219B", "Lightdance"
const char* WIFI_PASSWORD = "wifiyee219";     // "wifiyee219", "L
const char* RESPONSE_ADDRESS[] = {"192.168.0.137", "192.168.1.10"};      // "192.168.0.137"

// LED Section Mapping - combines all the separate arrays into one structure
struct LEDSection {
    int ledArrayIndex;    // Which LED strip (0-5)
    int startPosition;    // Starting LED in that strip
    int ledCount;         // Number of LEDs in this section
    int bodyPartIndex;    // Which body part color to use (1-9)
};


// New Led Sections
const LEDSection LED_SECTIONS[] = {
    {0, 0, 3, 7},       // tie
    {0, 3, 2, 2},       // face
    {0, 5, 1, 1},       // hat
    {1, 0, 5, 3},       // chestL
    {1, 5, 2, 5},       // armL
    {1, 7, 1, 9},       // gloveL
    {2, 0, 5, 4},       // chestR
    {2, 5, 2, 6},       // armR
    {2, 7, 1, 10},       // gloveR
    {4, 0, 2, 11},       // legL
    {4, 2, 1, 13},       // shoeL
    {5, 0, 2, 12},       // legR
    {5, 2, 1, 14},       // shoeR
    {6, 0, 1, 8},       // belt
    {7, 0, 1, 15},       // board
};

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
WiFiUDP udp;
EasyButton button(BUTTON_PIN, 100, true);

// LED Arrays
CRGB led1[6], led2[8], led3[8], led4[1], led5[3], led6[3], led7[1], led8[1];
CRGB* ledArrays[] = {led1, led2, led3, led4, led5, led6, led7, led8};

// Frame data storage
unsigned int frameData[MAX_FRAMES][BODY_PARTS + 1];  // +1 for time

// System state
enum State {
    READY,      // Waiting for commands
    PLAYING,    // Actively playing back animation
    STOPPED     // Explicitly stopped
};

State currentState = READY;

// Playback state
int currentFrameIndex = 0;
unsigned long playbackStartTime = 0;
unsigned long lastHeartbeatTime = 0;

// Network state
int wifiProfile = 0;
String deviceId;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

void softwareReboot() {
    // 參數都設 0，就是當機後立即重起
    Serial.println("Rebooting...");
    watchdog_reboot(0, 0, 0);
}

void showMessage(String message, int textSize = 2) {
    Serial.println(message);
    display.clearDisplay();
    display.setTextSize(textSize);
    display.setCursor(1, 1);
    display.println(message);
    display.display();
}

int calculateBrightness(unsigned int data) {
    // Extract brightness from bits 2-7 (skipping the 2 flag bits)
    int brightnessValue = (data >> 2) & 0x3F;  // 6 bits: 0-63 range
    return pow(1.74, brightnessValue / 2.5);
}

// ============================================================================
// WIFI & DATA LOADING
// ============================================================================

void connectWiFi(int wifiProfile) {
    showMessage("Connecting\n" + String(WIFI_SSID[wifiProfile]), 1);
    
    // Set static IP for profile 1
    if (wifiProfile == 1) {
        IPAddress localIP(192, 168, 1, 100 + PLAYER_NUM);
        WiFi.config(localIP);
    }
    
    Serial.println("Starting WiFi test...");
    WiFi.begin(WIFI_SSID[wifiProfile], WIFI_PASSWORD);
    
    // Wait for connection (max 5 seconds)
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 10) {
        Serial.print("Status: ");
        Serial.println(WiFi.status());  // Print actual status code
        delay(500);
        attempts++;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        showMessage("WiFi Failed!");
        delay(2000);
        softwareReboot();
    }
    
    showMessage("Connected\n" + WiFi.localIP().toString(), 1);
    delay(1000);
}

bool downloadChunk(int chunkNumber) {
    String apiUrl = "https://eesa.dece.nycu.edu.tw/lightdance/api/items/back_test/LATEST/player=" + String(PLAYER_NUM) + "/chunk=" + String(chunkNumber);
    WiFiClientSecure client;
    client.setInsecure();  // Skip SSL certificate verification
    Serial.print("Downloading from: ");
    Serial.println(apiUrl);

    HTTPClient http;
    http.begin(client, apiUrl);
    int responseCode = http.GET();
    
    // Check for connection/HTTP errors
    if (responseCode <= 0) {
        showMessage("HTTP Error: " + String(responseCode), 2);
        http.end();
        return false;
    }
    
    // Check for non-success HTTP status
    if (responseCode != 200) {
        showMessage("HTTP " + String(responseCode), 2);
        http.end();
        return false;
    }
    
    // Parse JSON
    StaticJsonDocument<4096> doc;
    DeserializationError jsonError = deserializeJson(doc, http.getString());
    
    if (jsonError) {
        showMessage("JSON Error", 2);
        http.end();
        return false;
    }
    
    showMessage("Chunk " + String(chunkNumber), 2);
    
    JsonArray players = doc["player_data"];
    int numFrames = players.size();

    for (int i = 0; i < numFrames && i < CHUNK_SIZE; i++) {
        int frameIndex = chunkNumber * CHUNK_SIZE + i;
        frameData[frameIndex][0]  = players[i]["time"].as<unsigned int>();
        frameData[frameIndex][1]  = players[i]["hat"].as<unsigned int>();
        frameData[frameIndex][2]  = players[i]["face"].as<unsigned int>();
        frameData[frameIndex][3]  = players[i]["chestL"].as<unsigned int>();
        frameData[frameIndex][4]  = players[i]["chestR"].as<unsigned int>();
        frameData[frameIndex][5]  = players[i]["armL"].as<unsigned int>();
        frameData[frameIndex][6]  = players[i]["armR"].as<unsigned int>();
        frameData[frameIndex][7]  = players[i]["tie"].as<unsigned int>();
        frameData[frameIndex][8]  = players[i]["belt"].as<unsigned int>();
        frameData[frameIndex][9]  = players[i]["gloveL"].as<unsigned int>();
        frameData[frameIndex][10] = players[i]["gloveR"].as<unsigned int>();
        frameData[frameIndex][11] = players[i]["legL"].as<unsigned int>();
        frameData[frameIndex][12] = players[i]["legR"].as<unsigned int>();
        frameData[frameIndex][13] = players[i]["shoeL"].as<unsigned int>();
        frameData[frameIndex][14] = players[i]["shoeR"].as<unsigned int>();
        frameData[frameIndex][15] = players[i]["board"].as<unsigned int>();
    }
    
    http.end();
    delay(20);
    return true;
}

void saveDataToMemory() {
    File file = LittleFS.open("/data.bin", "w");
    if (file) {
        file.write((uint8_t*)frameData, sizeof(frameData));
        file.close();
        showMessage("Saved!");
    }
}

void loadDataFromMemory() {
    File file = LittleFS.open("/data.bin", "r");
    if (file) {
        file.read((uint8_t*)frameData, sizeof(frameData));
        file.close();
        showMessage("Loaded!");
    }
}

// ============================================================================
// LED CONTROL
// ============================================================================

uint32_t ColorGradient(float startTime, float endTime, uint32_t currentColor, uint32_t nextColor, float currentTime, bool direction) {
    float progress = (currentTime - startTime) / (endTime - startTime);
    progress = min(max(progress, 0.0f), 1.0f);
    
    // Convert uint32_t to CRGB
    CRGB currentCRGB((currentColor >> 16) & 0xFF, (currentColor >> 8) & 0xFF, currentColor & 0xFF);
    CRGB nextCRGB((nextColor >> 16) & 0xFF, (nextColor >> 8) & 0xFF, nextColor & 0xFF);
    
    CHSV hsvStart = rgb2hsv_approximate(currentCRGB);
    CHSV hsvEnd = rgb2hsv_approximate(nextCRGB);

    int16_t dh = hsvEnd.h - hsvStart.h;
    if (direction) {
        dh %= 256;
    } else {
        dh %= 256;
        dh -= 256;
    }

    uint8_t h = hsvStart.h + (int16_t)(progress * dh);
    uint8_t s = hsvStart.s;
    uint8_t v = hsvStart.v;

    CHSV hsvInterp(h, s, v);
    
    CRGB result;
    hsv2rgb_rainbow(hsvInterp, result);
    
    return ((uint32_t)result.r << 16) | ((uint32_t)result.g << 8) | result.b;
}

void updateLEDs() {
  Serial.println("Updating led");
  Serial.print("Frame: ");
  Serial.println(currentFrameIndex);
  // Loop through each body part section and update its LEDs
  for (const LEDSection& section : LED_SECTIONS) {
    

    unsigned int bodyPartData = frameData[currentFrameIndex][section.bodyPartIndex];
    
    // Extract flags from last 2 bits
    bool shouldTransition = (bodyPartData >> 1) & 0x01;        // Bit 1: transition flag
    int transitionDirection = bodyPartData & 0x01;             // Bit 0: direction (0 or 1)
    
    uint32_t currentColor = bodyPartData >> 8;          // Extract color (upper 24 bits)
    int brightness = calculateBrightness(bodyPartData); // May need adjustment if brightness uses lower bits
    
    uint32_t finalColor = currentColor;
    
    // Check if this body part should transition
    if (shouldTransition && currentFrameIndex + 1 < MAX_FRAMES) {
      // Get next frame's color
      unsigned int nextBodyPartData = frameData[currentFrameIndex + 1][section.bodyPartIndex];
      uint32_t nextColor = nextBodyPartData >> 8;
      
      // Get frame times in milliseconds
      unsigned long startTime = frameData[currentFrameIndex][0] * 50;
      unsigned long endTime = frameData[currentFrameIndex + 1][0] * 50;
      unsigned long currentTime = millis() - playbackStartTime;
      
      // Calculate transition color using per-body-part direction
      finalColor = ColorGradient(startTime, endTime, currentColor, nextColor, currentTime, transitionDirection);
    }
    Serial.print("color: ");
    Serial.println(finalColor);
    Serial.print("brghtness: ");
    Serial.println(brightness);
    // Set all LEDs in this section
    for (int ledIndex = 0; ledIndex < section.ledCount; ledIndex++) {
      int position = section.startPosition + ledIndex;
      ledArrays[section.ledArrayIndex][position] = finalColor;
      ledArrays[section.ledArrayIndex][position].nscale8(brightness);
    }
  }
  
  FastLED.show();
}

void clearAllLEDs() {
    FastLED.clear();
    FastLED.show();
}

void runDebugMode() {
    // Show all body parts in their assigned colors for testing
    uint32_t bodyPartColors[] = {
        0x000000,   // 0  - unused
        0xFF3B30,   // 1  - hat (RED)
        0xFFD60A,   // 2  - face (YELLOW)
        0x007AFF,   // 3  - chestL (BLUE)
        0x5AC8FA,   // 4  - chestR (SKY BLUE)
        0x34C759,   // 5  - armL (GREEN)
        0x00E676,   // 6  - armR (NEON GREEN)
        0xFF9500,   // 7  - tie (ORANGE)
        0xFFD700,   // 8  - belt (GOLD)
        0xAF52DE,   // 9  - gloveL (PURPLE)
        0xFF2D55,   // 10 - gloveR (PINK)
        0x40E0D0,   // 11 - legL (AQUA)
        0x00CED1,   // 12 - legR (DARK TURQUOISE)
        0x8B4513,   // 13 - shoeL (BROWN)
        0xD2691E,   // 14 - shoeR (CHOCOLATE)
        0xFFFFFF    // 15 - board (WHITE)
    };
    
    for (const LEDSection& section : LED_SECTIONS) {
        for (int ledIndex = 0; ledIndex < section.ledCount; ledIndex++) {
            int position = section.startPosition + ledIndex;
            ledArrays[section.ledArrayIndex][position] = bodyPartColors[section.bodyPartIndex];
        }
    }
    
    FastLED.setBrightness(255);
    while (1) {
        Serial.println("Debugging");
        FastLED.show();
        delay(1000);
    }
}

// ============================================================================
// UDP COMMAND HANDLING
// ============================================================================

int checkForUDPCommand() {
    // Serial.print("Listening... ");
    int packetSize = udp.parsePacket();
    if (!packetSize) return 0;  // No packet received
    lastHeartbeatTime = millis();
    // Debug: Show we received something
    Serial.print("UDP received from: ");
    Serial.print(udp.remoteIP());
    Serial.print(":");
    Serial.println(udp.remotePort());
    
    // Read 4-byte command
    byte buffer[4];
    udp.read(buffer, 4);
    uint32_t command = (buffer[0] << 24) | (buffer[1] << 16) | (buffer[2] << 8) | buffer[3];
    
    // Return command type
    if (command == UDP_STOP_CMD) return -1;        // Stop command
    if (command == UDP_HEARTBEAT_CMD) return -2;   // Heartbeat
    return command;  // Timestamp in milliseconds
}

void sendUDPResponse(const char* message) {
    String response = deviceId + ": " + message;

    Serial.print("Sending response to: ");
    Serial.print(RESPONSE_ADDRESS[wifiProfile]);
    Serial.print(":");
    Serial.println(UDP_TX_PORT);
    Serial.println(response);

    udp.beginPacket(RESPONSE_ADDRESS[wifiProfile], UDP_TX_PORT);
    udp.write(response.c_str());
    udp.endPacket();
}

void syncPlaybackToTimestamp(int timestamp) {
    // Just set when playback "started" (in the past)
    playbackStartTime = millis() - timestamp;
    
    Serial.println("Synced to timestamp " + String(timestamp) + " ms");
    Serial.println("Playback start time " + String(playbackStartTime) + " ms");
}

// ============================================================================
// SETUP - Runs once at startup
// ============================================================================

void setup() {
    pinMode(LED_BUILTIN, OUTPUT);
    Serial.begin(115200);
    while (!Serial && millis() < 3000);
    Serial.println("\n=== LED Controller Starting ===");
    
    // Initialize I2C for display
    Wire.setSDA(SDA_PIN);
    Wire.setSCL(SCL_PIN);
    Wire.begin();
    
    // Initialize display
    if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
        Serial.println("Display initialization failed!");
    }
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.display();

    showMessage("Starting...");
    
    // Setup input pins
    pinMode(DEBUG_PIN, INPUT_PULLUP);
    pinMode(SWITCH_PIN, INPUT_PULLUP);
    pinMode(WIFI_PIN, INPUT_PULLUP);
    
    // Check for debug mode
    if (!digitalRead(DEBUG_PIN)) {
        showMessage("Debug Mode");
        Serial.println("Debug Mode");
        runDebugMode();  // Never returns
    }
    Serial.println(DEBUG_PIN);
    
    // Setup button
    button.begin();
    button.onPressed([]() {
        if (currentState == READY || currentState == STOPPED) {
            currentState = PLAYING;
            showMessage("Started!");
        }
    });
    
    // Connect to WiFi (profile selected by WIFI_PIN)
    wifiProfile = digitalRead(WIFI_PIN) ? 0 : 1;
    deviceId = "player" + String(PLAYER_NUM);
    connectWiFi(wifiProfile);
    
    // Initialize LED strips        CRGB led1[6], led2[8], led3[8], led4[1], led5[3], led6[3], led7[1], led8[1];
    FastLED.addLeds<NEOPIXEL, 2>(ledArrays[0], 6);
    FastLED.addLeds<NEOPIXEL, 3>(ledArrays[1], 8);
    FastLED.addLeds<NEOPIXEL, 4>(ledArrays[2], 8);
    FastLED.addLeds<NEOPIXEL, 5>(ledArrays[3], 1);
    FastLED.addLeds<NEOPIXEL, 6>(ledArrays[4], 3);
    FastLED.addLeds<NEOPIXEL, 7>(ledArrays[5], 3);
    FastLED.addLeds<NEOPIXEL, 8>(ledArrays[6], 1);
    FastLED.addLeds<NEOPIXEL, 9>(ledArrays[7], 1);
    FastLED.setBrightness(255);
    clearAllLEDs();
    
    // Initialize filesystem
    if (!LittleFS.begin()) {
        Serial.println("Formatting filesystem...");
        LittleFS.format();
        LittleFS.begin();
    }

    // if (!initLocalData()) {
    //     showMessage("FATAL: Data load failed", 5);
    //     while(1);  // Halt
    // }
    
    // Load animation data - WiFi download ONLY happens here in setup
    bool useMemoryMode = !digitalRead(SWITCH_PIN);
    if (useMemoryMode) {
        showMessage("Loading from\nmemory...");
        loadDataFromMemory();
    } else {
        showMessage("Downloading\nfrom WiFi...");
        
        bool allSuccess = true;
        
        for (int chunk = 0; chunk < NUM_CHUNKS; chunk++) {
            // Retry each chunk up to 3 times
            bool chunkSuccess = false;
            for (int attempt = 0; attempt < 3; attempt++) {
                if (downloadChunk(chunk)) {
                    chunkSuccess = true;
                    break;
                }
                showMessage("Retry " + String(attempt + 1) + "/3\nChunk " + String(chunk));
                delay(500);
            }
            
            if (!chunkSuccess) {
                allSuccess = false;
                showMessage("Failed!\nChunk " + String(chunk));
                break;  // Stop downloading
            }
        }
        
        if (allSuccess) {
            showMessage("Download OK!\nSaving...");
            saveDataToMemory();
        } else {
            showMessage("Download failed\nUsing memory...");
            loadDataFromMemory();  // Fallback to stored data
        }
    }
    Serial.println("ip: " + WiFi.localIP().toString());
    // Start UDP listener
    udp.begin(UDP_RX_PORT);
    udp.flush();
    Serial.println("UDP listening on port " + String(UDP_RX_PORT));
    
    currentState = READY;
    showMessage("Ready!\n" + WiFi.localIP().toString());
    Serial.println("=== Setup Complete ===");
    Serial.println("State: READY\n");
    lastHeartbeatTime = millis();
}

// ============================================================================
// MAIN LOOP - Single unified control flow with state machine
// ============================================================================

void loop() {
    // Always check button
    button.read();
    // Serial.println("loop");
    digitalWrite(LED_BUILTIN, HIGH);
    // Check for UDP commands
    int command = checkForUDPCommand();
    
    // Handle stop command
    if (command == -1) {
        sendUDPResponse("stopped");
        currentState = STOPPED;
        currentFrameIndex = 0;
        clearAllLEDs();
    }
    // Handle heartbeat command
    else if (command == -2) {
        sendUDPResponse("heartbeat received");
    }
    // Handle timestamp command (starts/syncs playback)
    else if (command > 0) {
        sendUDPResponse("running");
        syncPlaybackToTimestamp(command);
        currentState = PLAYING;
    }
    
    // Execute behavior based on current state
    switch (currentState) {
        case READY:
            currentFrameIndex = 0;
            if (millis() - lastHeartbeatTime > 5000) {
                showMessage("No heartbeat\nRebooting...");
                delay(500);
                softwareReboot();
            }
            showMessage("Ready!");
            break;
            
        case STOPPED:
            currentFrameIndex = 0;
            if (millis() - lastHeartbeatTime > 5000) {
                showMessage("No heartbeat\nRebooting...");
                delay(500);
                softwareReboot();
            }
            showMessage("Stopped!");
            break;
            
        case PLAYING:
            // Check for heartbeat timeout
            if (millis() - lastHeartbeatTime > HEARTBEAT_TIMEOUT) {
                showMessage("No Signal!");
            }
            else{
                showMessage("Playing!");
            }
            
            // Calculate current playback time in frames
            unsigned long elapsedTime = millis() - playbackStartTime;
            int currentFrame = elapsedTime / 50;  // 50ms per frame
            Serial.print("currentFrame: ");
            Serial.println(currentFrame);
            

            // Advance to current time position (skip frames if needed)
            while (currentFrameIndex+1 < MAX_FRAMES && 
                   frameData[currentFrameIndex+1][0] < currentFrame) {
                currentFrameIndex++;
            }
            Serial.print("currentFrameIndex: ");
            Serial.println(currentFrameIndex);
            updateLEDs();
            break;
    }
    
    delay(5);
}