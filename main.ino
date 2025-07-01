#include <Arduino.h>
#include <HardwareSerial.h>
#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_Fingerprint.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

#define RFID_RX_PIN 16 // RX2 on ESP32 for RDM6300
#define RELAY_PIN 13   // Relay connected to D13
#define BUTTON_PIN 14  // Push button connected to D14 (D14 to GND when pressed)
#define FINGER_RX_PIN 25 // RX for R307 (UART1)
#define FINGER_TX_PIN 33 // TX for R307 (UART1)
#define TFT_CS 5       // Chip Select for ST7735
#define TFT_DC 2       // Data/Command for ST7735
#define TFT_RST 4      // Reset for ST7735

// EEPROM configuration
#define EEPROM_SIZE 112 // SSID (32) + Password (64) + Device ID (16)
#define SSID_ADDR 0
#define PASS_ADDR 32
#define DEVICE_ID_ADDR 96

// Soft AP configuration
const char *softAP_ssid = "ESP32-Setup";
const char *softAP_password = "12345678";

// Timeout for WiFi connection attempt (in milliseconds)
const unsigned long wifiTimeout = 10000; // 10 seconds

// Web server on port 80
WebServer server(80);

// Variables to store WiFi credentials and device ID
String ssid = "";
String password = "";
String deviceId = "";

HardwareSerial RFIDSerial(2); // UART2 for RDM6300
HardwareSerial FingerSerial(1); // UART1 for R307
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&FingerSerial);
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

const char* apiEndpoint = "http://92.113.149.175:8000/api/attendance/";
const long ADMIN_RFID_ID = 3353115; // Admin RFID for enrollment

const int BUFFER_SIZE = 14;
const int DATA_TAG_SIZE = 8;
uint8_t buffer[BUFFER_SIZE];
int buffer_index = 0;
unsigned long last_read_time = 0; // Last read time for debouncing
const unsigned long DEBOUNCE_DELAY = 6000; // 6 seconds for debounce
unsigned long last_button_time = 0; // Last button press time
bool is_processing = false; // Block processing during API/relay/enrollment
bool is_soft_ap_mode = false; // Track Soft AP mode

// Helper function to center text
void drawCenteredText(const char* text, uint16_t color, int yOffset = 0) {
  int16_t x1, y1;
  uint16_t w, h;
  tft.setTextColor(color);
  tft.setTextSize(1); // ~10-pixel height
  tft.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  int x = (128 - w) / 2; // Center horizontally
  int y = (160 - h) / 2 + yOffset; // Center vertically, adjust for offset
  tft.setCursor(x, y);
  tft.println(text);
}

// Helper function to display default prompt
void drawDefaultPrompt() {
  tft.fillScreen(ST7735_BLACK);
  drawCenteredText("Tap finger", ST7735_WHITE, -10);
  drawCenteredText("or card", ST7735_WHITE, 10);
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  // Initialize TFT display
  tft.initR(INITR_BLACKTAB); // Initialize ST7735 with black tab
  tft.setRotation(0); // Portrait mode
  tft.fillScreen(ST7735_BLACK); // Black background
  drawCenteredText("Initializing...", ST7735_WHITE);

  // Initialize EEPROM
  EEPROM.begin(EEPROM_SIZE);

  // Initialize relay and button pins
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // Ensure relay is off
  pinMode(BUTTON_PIN, INPUT_PULLUP); // Button with internal pull-up (pressed = LOW)
  
  // Initialize RFID
  RFIDSerial.begin(9600, SERIAL_8N1, RFID_RX_PIN, -1); // TX not needed
  
  // Initialize fingerprint sensor
  FingerSerial.begin(57600, SERIAL_8N1, FINGER_RX_PIN, FINGER_TX_PIN);
  finger.begin(57600);
  
  if (finger.verifyPassword()) {
    Serial.println("Found fingerprint sensor!");
    drawCenteredText("Fingerprint OK", ST7735_WHITE);
  } else {
    Serial.println("Did not find fingerprint sensor :(");
    drawCenteredText("No Fingerprint", ST7735_WHITE);
    while (1) delay(1);
  }
  delay(1000);
  
  // Read stored credentials and device ID
  readCredentials();
  
  // Connect to WiFi
  if (!connectToWiFi()) {
    startSoftAP();
    startWebServer();
    is_soft_ap_mode = true;
  } else {
    Serial.println("RFID and Fingerprint Reader Ready");
    drawDefaultPrompt();
  }
}

void loop() {
  if (is_soft_ap_mode) {
    server.handleClient(); // Handle web server requests in Soft AP mode
  } else if (!is_processing) {
    checkRFID();
    checkFingerprint();
    checkPushButton();
  }
}

// --------------------- CONNECT TO WIFI --------------------------------------
bool connectToWiFi() {
  if (ssid == "" || password == "") {
    Serial.println("No credentials stored. Starting Soft AP...");
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("No Credentials", ST7735_WHITE);
    delay(1000);
    return false;
  }

  Serial.print("Connecting to ");
  Serial.println(ssid);
  tft.fillScreen(ST7735_BLACK);
  drawCenteredText("Connecting WiFi...", ST7735_WHITE);
  
  WiFi.begin(ssid.c_str(), password.c_str());

  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < wifiTimeout) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("Connected", ST7735_WHITE, -20);
    String ip = "IP: " + WiFi.localIP().toString();
    drawCenteredText(ip.c_str(), ST7735_WHITE, 20);
    delay(2000);
    return true;
  } else {
    Serial.println("\nFailed to connect to WiFi.");
    WiFi.disconnect();
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("WiFi Failed", ST7735_WHITE);
    delay(2000);
    return false;
  }
}

// --------------------- START SOFT AP --------------------------------------
void startSoftAP() {
  Serial.println("Starting Soft AP...");
  tft.fillScreen(ST7735_BLACK);
  drawCenteredText("WiFi Setup Mode", ST7735_WHITE, -20);
  drawCenteredText("Soft AP: ESP32-Setup", ST7735_WHITE, 20);
  WiFi.softAP(softAP_ssid, softAP_password);
  Serial.print("Soft AP IP Address: ");
  Serial.println(WiFi.softAPIP());
}

// --------------------- READ CREDENTIALS ------------------------------------
void readCredentials() {
  char storedSSID[32] = {0};
  char storedPassword[64] = {0};
  char storedDeviceId[16] = {0};

  // Read SSID
  for (int i = 0; i < 32; i++) {
    storedSSID[i] = EEPROM.read(SSID_ADDR + i);
    if (storedSSID[i] == '\0') break;
  }

  // Read Password
  for (int i = 0; i < 64; i++) {
    storedPassword[i] = EEPROM.read(PASS_ADDR + i);
    if (storedPassword[i] == '\0') break;
  }

  // Read Device ID
  for (int i = 0; i < 16; i++) {
    storedDeviceId[i] = EEPROM.read(DEVICE_ID_ADDR + i);
    if (storedDeviceId[i] == '\0') break;
  }

  ssid = String(storedSSID);
  password = String(storedPassword);
  deviceId = String(storedDeviceId);

  Serial.println("Stored Credentials:");
  Serial.println("SSID: " + ssid);
  Serial.println("Password: " + password);
  Serial.println("Device ID: " + deviceId);
}

// --------------------- SAVE CREDENTIALS ------------------------------------
void saveCredentials(String newSSID, String newPassword, String newDeviceId) {
  Serial.println("Saving new credentials...");
  tft.fillScreen(ST7735_BLACK);
  drawCenteredText("Saving Credentials...", ST7735_WHITE);

  // Clear EEPROM section
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }

  // Save SSID
  for (int i = 0; i < newSSID.length() && i < 32; i++) {
    EEPROM.write(SSID_ADDR + i, newSSID[i]);
  }

  // Save Password
  for (int i = 0; i < newPassword.length() && i < 64; i++) {
    EEPROM.write(PASS_ADDR + i, newPassword[i]);
  }

  // Save Device ID
  for (int i = 0; i < newDeviceId.length() && i < 16; i++) {
    EEPROM.write(DEVICE_ID_ADDR + i, newDeviceId[i]);
  }

  EEPROM.commit();
  Serial.println("Credentials saved to EEPROM.");

  // Update variables
  ssid = newSSID;
  password = newPassword;
  deviceId = newDeviceId;
}

// --------------------- START WEB SERVER ------------------------------------
void startWebServer() {
  // Serve the configuration page
  server.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head><title>ESP32 WiFi Setup</title></head><body>";
    html += "<h1>ESP32 WiFi Configuration</h1>";

    // Scan for available networks
    int n = WiFi.scanNetworks();
    html += "<form method='POST' action='/save'>";
    html += "<label>SSID: </label><select name='ssid'>";
    for (int i = 0; i < n; i++) {
      html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + "</option>";
    }
    html += "</select><br><br>";
    html += "<label>Password: </label><input type='password' name='password'><br><br>";
    html += "<label>Device ID: </label><input type='text' name='device_id'><br><br>";
    html += "<input type='submit' value='Save'>";
    html += "</form></body></html>";

    server.send(200, "text/html", html);
  });

  // Handle saving credentials
  server.on("/save", HTTP_POST, []() {
    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");
    String newDeviceId = server.arg("device_id");

    if (newSSID != "" && newPassword != "" && newDeviceId != "") {
      saveCredentials(newSSID, newPassword, newDeviceId);
      server.send(200, "text/html", "<h1>Credentials saved! Rebooting...</h1>");
      delay(1000);
      ESP.restart();
    } else {
      server.send(400, "text/html", "<h1>Invalid credentials!</h1>");
    }
  });

  server.begin();
  Serial.println("Web server started.");
}

// --------------------- CHECK PUSH BUTTON ------------------------------------
void checkPushButton() {
  if (digitalRead(BUTTON_PIN) == LOW) { // Active-low (pressed = LOW)
    unsigned long current_time = millis();
    if (current_time - last_button_time < DEBOUNCE_DELAY) {
      delay(50); // Simple debounce
      return;
    }

    Serial.println("Button pressed, opening gate...");
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("Button Pressed", ST7735_WHITE);
    delay(1000);

    digitalWrite(RELAY_PIN, HIGH); // Turn relay ON
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("Gate Open", ST7735_GREEN);
    delay(5000); // Keep relay ON for 5 seconds
    digitalWrite(RELAY_PIN, LOW); // Turn relay OFF
    Serial.println("Gate Closed");
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("Gate Closed", ST7735_WHITE);
    delay(2000);

    last_button_time = current_time;
    drawDefaultPrompt();
  }
}

// --------------------- CHECK RFID ------------------------------------------
void checkRFID() {
  while (RFIDSerial.available() > 0) {
    int ssvalue = RFIDSerial.read();
    if (ssvalue == -1) {
      Serial.println("Error: Invalid RFID read");
      tft.fillScreen(ST7735_BLACK);
      drawCenteredText("Invalid RFID", ST7735_WHITE);
      delay(1000);
      drawDefaultPrompt();
      return;
    }

    bool call_extract_tag = false;

    if (ssvalue == 2) {
      buffer_index = 0; // Start new buffer
    } else if (ssvalue == 3) {
      call_extract_tag = true;
    }

    if (buffer_index >= BUFFER_SIZE) {
      Serial.println("Error: RFID buffer overflow, resetting");
      tft.fillScreen(ST7735_BLACK);
      drawCenteredText("RFID Overflow", ST7735_WHITE);
      delay(1000);
      drawDefaultPrompt();
      buffer_index = 0;
      return;
    }

    buffer[buffer_index++] = ssvalue;

    if (call_extract_tag && buffer_index == BUFFER_SIZE) {
      extract_tag();
      while (RFIDSerial.available() > 0) {
        RFIDSerial.read(); // Clear serial buffer
      }
      buffer_index = 0; // Reset buffer
      return;
    } else if (call_extract_tag) {
      Serial.println("Error: Incomplete RFID frame, resetting");
      tft.fillScreen(ST7735_BLACK);
      drawCenteredText("Incomplete RFID", ST7735_WHITE);
      delay(1000);
      drawDefaultPrompt();
      buffer_index = 0;
    }
  }
}

// --------------------- EXTRACT TAG ------------------------------------------
void extract_tag() {
  unsigned long current_time = millis();
  if (current_time - last_read_time < DEBOUNCE_DELAY) {
    Serial.println("Debounce: RFID read ignored (within 6s window)");
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("RFID Debounced", ST7735_WHITE);
    delay(1000);
    drawDefaultPrompt();
    return;
  }

  uint8_t* msg_data_tag = buffer + 3;
  long tag = hexstr_to_value((char*)msg_data_tag, DATA_TAG_SIZE);

  Serial.print("RFID Tag: ");
  Serial.println(tag);
  tft.fillScreen(ST7735_BLACK);
  String tagStr = "RFID: " + String(tag);
  drawCenteredText(tagStr.c_str(), ST7735_WHITE);
  
  is_processing = true;
  
  if (tag == ADMIN_RFID_ID) {
    Serial.println("Admin RFID detected, entering enrollment mode...");
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("Admin RFID", ST7735_WHITE, -20);
    drawCenteredText("Enrollment Mode", ST7735_WHITE, 20);
    delay(2000);
    uint8_t id = getNextAvailableID();
    if (id == 0xFF) {
      Serial.println("No available ID slots for fingerprint enrollment!");
      tft.fillScreen(ST7735_BLACK);
      drawCenteredText("No ID Slots", ST7735_WHITE);
      delay(2000);
    } else {
      Serial.print("Enrolling to ID #"); Serial.println(id);
      tft.fillScreen(ST7735_BLACK);
      String enrollStr = "Enroll ID #" + String(id);
      drawCenteredText(enrollStr.c_str(), ST7735_WHITE);
      enrollFingerprint(id);
    }
    last_read_time = current_time;
    is_processing = false;
    drawDefaultPrompt();
  } else {
    char rfid_str[12];
    snprintf(rfid_str, sizeof(rfid_str), "%ld", tag);
    sendToAPI(rfid_str, nullptr); // Send RFID, no fingerprint
    last_read_time = current_time;
    is_processing = false;
  }
}

// --------------------- CHECK FINGERPRINT ------------------------------------
void checkFingerprint() {
  int p = finger.getImage();
  if (p == FINGERPRINT_NOFINGER) {
    return; // No finger, exit quietly
  } else if (p != FINGERPRINT_OK) {
    Serial.println("Error: Fingerprint image capture failed");
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("Finger Capture Err", ST7735_WHITE);
    delay(1000);
    drawDefaultPrompt();
    return;
  }

  // Convert image to template
  if (finger.image2Tz() != FINGERPRINT_OK) {
    Serial.println("Error: Fingerprint image conversion failed");
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("Finger Convert Err", ST7735_WHITE);
    delay(1000);
    drawDefaultPrompt();
    return;
  }

  // Search for a match
  uint16_t fid, score;
  if (finger.fingerSearch() == FINGERPRINT_OK) {
    fid = finger.fingerID;
    score = finger.confidence;
    Serial.print("Fingerprint ID: ");
    Serial.print(fid);
    Serial.print(" (Confidence: ");
    Serial.print(score);
    Serial.println(")");
    tft.fillScreen(ST7735_BLACK);
    String fidStr = "ID: " + String(fid);
    drawCenteredText(fidStr.c_str(), ST7735_WHITE, -20);
    String scoreStr = "Conf: " + String(score);
    drawCenteredText(scoreStr.c_str(), ST7735_WHITE, 20);
    
    unsigned long current_time = millis();
    if (current_time - last_read_time < DEBOUNCE_DELAY) {
      Serial.println("Debounce: Fingerprint read ignored (within 6s window)");
      tft.fillScreen(ST7735_BLACK);
      drawCenteredText("Finger Debounced", ST7735_WHITE);
      delay(1000);
      drawDefaultPrompt();
      return;
    }
    
    // Open gate for enrolled fingerprint
    digitalWrite(RELAY_PIN, HIGH); // Turn relay ON
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("Gate Open", ST7735_GREEN);
    delay(5000); // Keep relay ON for 5 seconds
    digitalWrite(RELAY_PIN, LOW); // Turn relay OFF
    Serial.println("Gate Closed");
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("Gate Closed", ST7735_WHITE);
    delay(2000);

    // Send fingerprint ID to server
    char fid_str[12];
    snprintf(fid_str, sizeof(fid_str), "%d", fid);
    
    is_processing = true;
    sendToAPI(nullptr, fid_str); // Send fingerprint, no RFID
    last_read_time = current_time;
    is_processing = false;
  } else {
    Serial.println("Fingerprint not found in database");
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("Finger Not Found", ST7735_WHITE);
    delay(1000);
    drawDefaultPrompt();
  }
}

// --------------------- SEND TO API ------------------------------------------
void sendToAPI(const char* rfid, const char* fingerprint_id) {
  HTTPClient http;
  http.begin(apiEndpoint);
  http.addHeader("Content-Type", "application/json");

  // Create JSON payload
  StaticJsonDocument<200> doc;
  if (rfid) {
    doc["rfid"] = rfid;
    doc["fingerprint_id"] = nullptr;
  } else {
    doc["rfid"] = nullptr;
    doc["fingerprint_id"] = fingerprint_id;
  }
  doc["device_id"] = deviceId.c_str();

  String jsonPayload;
  serializeJson(doc, jsonPayload);

  Serial.print("Sending JSON: ");
  Serial.println(jsonPayload);

  // Send POST request
  int httpResponseCode = http.POST(jsonPayload);

  // Handle response
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    Serial.print("Response: ");
    Serial.println(response);

    // Parse JSON response
    StaticJsonDocument<200> responseDoc;
    DeserializationError error = deserializeJson(responseDoc, response);
    if (error) {
      Serial.print("JSON parsing failed: ");
      Serial.println(error.c_str());
      tft.fillScreen(ST7735_BLACK);
      drawCenteredText("JSON Parse Err", ST7735_WHITE);
      delay(2000);
    } else {
      const char* data = responseDoc["data"];
      int access = responseDoc["access"];
      tft.fillScreen(ST7735_BLACK);
      if (strcmp(data, "user not found") == 0) {
        Serial.println("No user data");
        drawCenteredText("No User Data", ST7735_RED);
        delay(2000);
      } else if (access == 1) {
        Serial.print("Welcome: ");
        Serial.print(data);
        Serial.println(", Access granted");
        String welcome = "Welcome: " + String(data);
        drawCenteredText(welcome.c_str(), ST7735_GREEN, -20);
        drawCenteredText("Access Granted", ST7735_GREEN, 20);
        // Open gate only for RFID with access:1
        if (rfid) {
          digitalWrite(RELAY_PIN, HIGH); // Turn relay ON
          tft.fillScreen(ST7735_BLACK);
          drawCenteredText("Gate Open", ST7735_GREEN);
          delay(5000); // Keep relay ON for 5 seconds
          digitalWrite(RELAY_PIN, LOW); // Turn relay OFF
          Serial.println("Gate Closed");
          tft.fillScreen(ST7735_BLACK);
          drawCenteredText("Gate Closed", ST7735_WHITE);
          delay(2000);
        }
        delay(2000);
      } else {
        Serial.println("Access denied");
        drawCenteredText("Access Denied", ST7735_RED);
        delay(2000);
      }
    }
  } else {
    Serial.print("Error on sending POST: ");
    Serial.println(httpResponseCode);
    tft.fillScreen(ST7735_BLACK);
    String err = "POST Err: " + String(httpResponseCode);
    drawCenteredText(err.c_str(), ST7735_WHITE);
    delay(2000);
  }

  http.end();
  drawDefaultPrompt();
}

// --------------------- GET NEXT AVAILABLE ID --------------------------------
uint8_t getNextAvailableID() {
  for (uint8_t id = 1; id < 127; id++) { // ID range: 1–127
    if (finger.loadModel(id) != FINGERPRINT_OK) {
      return id; // ID is available
    }
  }
  return 0xFF; // No available slot
}

// --------------------- ENROLL FINGERPRINT -----------------------------------
void enrollFingerprint(uint8_t id) {
  int p = -1;
  Serial.println("Place your finger on the sensor...");
  tft.fillScreen(ST7735_BLACK);
  drawCenteredText("Place Finger", ST7735_WHITE);

  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      Serial.print(".");
      delay(100);
    } else if (p == FINGERPRINT_OK) {
      Serial.println("Image taken");
      tft.fillScreen(ST7735_BLACK);
      drawCenteredText("Image Taken", ST7735_WHITE);
    } else {
      Serial.println("Error capturing image");
      tft.fillScreen(ST7735_BLACK);
      drawCenteredText("Capture Err", ST7735_WHITE);
      delay(2000);
      drawDefaultPrompt();
      return;
    }
  }

  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    Serial.println("Image conversion failed");
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("Convert Err", ST7735_WHITE);
    delay(2000);
    drawDefaultPrompt();
    return;
  }

  Serial.println("Remove your finger...");
  tft.fillScreen(ST7735_BLACK);
  drawCenteredText("Remove Finger", ST7735_WHITE);
  delay(2000);
  while (finger.getImage() != FINGERPRINT_NOFINGER);

  Serial.println("Place the same finger again...");
  tft.fillScreen(ST7735_BLACK);
  drawCenteredText("Place Again", ST7735_WHITE);
  p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER) {
      Serial.print(".");
      delay(100);
    } else if (p == FINGERPRINT_OK) {
      Serial.println("Image taken");
      tft.fillScreen(ST7735_BLACK);
      drawCenteredText("Image Taken", ST7735_WHITE);
    } else {
      Serial.println("Error capturing image");
      tft.fillScreen(ST7735_BLACK);
      drawCenteredText("Capture Err", ST7735_WHITE);
      delay(2000);
      drawDefaultPrompt();
      return;
    }
  }

  if (finger.image2Tz(2) != FINGERPRINT_OK) {
    Serial.println("Second image conversion failed");
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("2nd Convert Err", ST7735_WHITE);
    delay(2000);
    drawDefaultPrompt();
    return;
  }

  if (finger.createModel() != FINGERPRINT_OK) {
    Serial.println("Could not create fingerprint model");
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("Model Err", ST7735_WHITE);
    delay(2000);
    drawDefaultPrompt();
    return;
  }

  if (finger.storeModel(id) == FINGERPRINT_OK) {
    Serial.print("Successfully stored fingerprint with ID #");
    Serial.println(id);
    tft.fillScreen(ST7735_BLACK);
    String idStr = "Stored ID #" + String(id);
    drawCenteredText(idStr.c_str(), ST7735_WHITE);
    delay(2000);
  } else {
    Serial.println("Failed to store fingerprint");
    tft.fillScreen(ST7735_BLACK);
    drawCenteredText("Store Failed", ST7735_WHITE);
    delay(2000);
  }
  drawDefaultPrompt();
}

// --------------------- HEX TO DECIMAL ---------------------------------------
long hexstr_to_value(char* str, unsigned int length) {
  char copy[11];
  strncpy(copy, str, length);
  copy[length] = '\0';
  return strtol(copy, NULL, 16);
}
