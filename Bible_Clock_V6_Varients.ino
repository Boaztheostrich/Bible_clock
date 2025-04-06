#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <WebServer.h>  // Built-in webserver library
#include "EPD.h"        // E-Paper Display driver
#include "EPD_GUI.h"    // EPD GUI functions
#include <DNSServer.h>
#include <map>  // <<< ADDED FOR MAPPING FILE

// --- Configuration File Handling ---
String wifiSSID = "";
String wifiPassword = "";
String timeZone = "";  // Time zone offset in seconds (as a string from config)

// --- WiFi and NTP Settings ---
const char *ntpServer = "pool.ntp.org";
// Use a variable (not const) so it can be updated from config:
long gmtOffset_sec = -21600;       // Default to CST (-6*3600)
const int daylightOffset_sec = 0;  // No DST

// --- SD Card SPI pin definitions ---
#define SD_CS_PIN 10    // Chip Select (IO10)
#define SD_MOSI_PIN 40  // MOSI (IO40)
#define SD_MISO_PIN 13  // MISO (IO13)
#define SD_SCK_PIN 39   // Clock (IO39)

// --- Button Definitions ---
// Key assignments per your reminder:
#define HOME_KEY 2  // Home key (also used for triggering offline mode via long press)
#define EXIT_KEY 1  // Exit key
#define PRV_KEY 6   // Previous page key (decrement field)
#define NEXT_KEY 4  // Next page key (increment field)
#define OK_KEY 5    // Confirm key

// --- Global objects ---
SPIClass sdSPI(HSPI);
WebServer server(80);                    // Web server on port 80
std::map<String, String> timeToFileMap;  // <<< ADDED GLOBAL MAP

// --- Global Variables (Online Mode) ---
uint8_t Image_BW[15000];  // E-paper image buffer
int lastMinute = -1;      // For time update tracking
bool waitingForTimeCheck = false;
unsigned long homePressTime = 0;
unsigned long lastSerialCheck = 0;
const unsigned long serialInterval = 50;  // for non-blocking serial handling
bool configMode = false;                  // Online configuration (webserver) mode flag

// --- Global Variables for Reset Prompt ---
bool resetPromptActive = false;
unsigned long resetPromptStart = 0;

// --- Global Variables for Offline Configuration ---
// offlineState: 0 = editing hour (1–12), 1 = editing tens-of-minutes (0–5), 2 = editing ones-of-minutes (0–9)
bool offlineConfigMode = false;
bool offlineOperating = false;
int offlineState = 0;
int offlineHour = 12;                 // offline hour (1-12)
int offlineTens = 0;                  // tens-of-minutes (0-5)
int offlineOnes = 0;                  // ones-of-minutes (0-9)
unsigned long offlineBaseTime = 0;    // computed as offlineHour*3600 + ((offlineTens*10+offlineOnes)*60)
unsigned long offlineLastUpdate = 0;  // millis() when offline mode started
int offlineLastDisplayedMinute = -1;  // For updating only when minute changes

// --- Global Variable for Offline Mode Trigger ---
unsigned long lastExitPress = 0;

// --- Volatile flags for button interrupts ---
volatile bool homeButtonInterrupt = false;
volatile bool exitButtonInterrupt = false;
volatile bool okButtonInterrupt = false;
volatile bool prvButtonInterrupt = false;
volatile bool nextButtonInterrupt = false;

// --- ISR functions for buttons ---
// ISR for HOME button
IRAM_ATTR void homeButtonISR() {
  homeButtonInterrupt = true;
}

// ISR for EXIT button
IRAM_ATTR void exitButtonISR() {
  exitButtonInterrupt = true;
}

// ISR for OK button
IRAM_ATTR void okButtonISR() {
  okButtonInterrupt = true;
}

// ISR for PRV button
IRAM_ATTR void prvButtonISR() {
  prvButtonInterrupt = true;
}

// ISR for NEXT button
IRAM_ATTR void nextButtonISR() {
  nextButtonInterrupt = true;
}

bool isDST(struct tm timeinfo) {
  // Most of the US observes DST from the second Sunday in March to the first Sunday in November
  if (timeinfo.tm_mon < 2 || timeinfo.tm_mon > 10) {
    return false;  // January, February, December are not DST
  }
  if (timeinfo.tm_mon > 2 && timeinfo.tm_mon < 10) {
    return true;  // April through October are DST
  }

  // March: DST starts on the second Sunday
  if (timeinfo.tm_mon == 2) {
    // Calculate the second Sunday (first Sunday is 1-7, second is 8-14)
    int secondSunday = 8;
    while (secondSunday < 15) {
      // Simple check for Sunday (assuming tm_wday is 0 for Sunday)
      struct tm march_day = timeinfo;
      march_day.tm_mday = secondSunday;
      mktime(&march_day);  // Normalize the structure to get tm_wday
      if (march_day.tm_wday == 0)
        break;
      secondSunday++;
    }
    return (timeinfo.tm_mday >= secondSunday);
  }

  // November: DST ends on the first Sunday
  if (timeinfo.tm_mon == 10) {
    // Calculate the first Sunday (1-7)
    int firstSunday = 1;
    while (firstSunday < 8) {
      struct tm nov_day = timeinfo;
      nov_day.tm_mday = firstSunday;
      mktime(&nov_day);  // Normalize the structure to get tm_wday
      if (nov_day.tm_wday == 0)
        break;
      firstSunday++;
    }
    return (timeinfo.tm_mday < firstSunday);
  }

  return false;
}

// --- Helper Functions for Online Display ---
void clear_all() {
  EPD_Clear();
  Paint_NewImage(Image_BW, EPD_W, EPD_H, 0, BLACK);
  EPD_Full(WHITE);
  EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
}

void displayConfigInstructions(String ipAddress) {
  clear_all();
  EPD_ShowString(10, 10, "CONFIG MODE", 16, BLACK);
  if (wifiSSID.length() > 0) {
    EPD_ShowString(10, 30, ("Connect to: " + wifiSSID).c_str(), 16, BLACK);
    EPD_ShowString(10, 50, ("Open: http://" + ipAddress).c_str(), 16, BLACK);
  } else {
    EPD_ShowString(10, 30, "Connect to: Bible_Clock_Config", 16, BLACK);
    EPD_ShowString(10, 50, "Open: http://192.168.4.1", 16, BLACK);
  }
  EPD_ShowString(10, 70, "Reset: Hold EXIT 3 sec", 16, BLACK);
  EPD_ShowString(10, 90, "then press OK within 5 sec", 16, BLACK);
  EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
}

void displayFile(const char *filename) {
  // Check for empty filename before proceeding
  if (filename == nullptr || strlen(filename) == 0) {
    Serial.println("ERROR: displayFile called with empty filename.");
    return;
  }
  if (filename[0] != '/') {
    Serial.println("ERROR: displayFile called without leading '/' in filename.");
    // Optionally prepend '/' here if desired, but better to fix callers
    return;
  }

  Serial.print("Attempting to open file: ");
  Serial.println(filename);

  unsigned long startTime_displayFile = millis();  // Start time for displayFile()

  File file = SD.open(filename, FILE_READ);
  if (!file) {
    Serial.print("ERROR: File not found: ");
    Serial.println(filename);
    // Optional: Display error on EPD
    // clear_all();
    // EPD_ShowString(10, 10, "ERROR: File not found", 16, BLACK);
    // EPD_ShowString(10, 30, filename, 16, BLACK);
    // EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
    return;
  }

  // Check file size (optional but good practice)
  size_t fileSize = file.size();
  if (fileSize == 0) {
    Serial.println("ERROR: File is empty.");
    file.close();
    return;
  }
  if (fileSize > sizeof(Image_BW)) {
    Serial.printf("WARNING: File size (%d) > buffer size (%d). Truncating.\n",
                  fileSize, sizeof(Image_BW));
  }

  size_t bytesRead = file.read(Image_BW, sizeof(Image_BW));
  Serial.printf("Bytes read: %d\n", bytesRead);
  file.close();

  // Only proceed if some bytes were read
  if (bytesRead > 0) {
    EPD_Init_Fast(Fast_Seconds_1_5s);

    unsigned long startTime_displayFast = millis();  // Start time for EPD_Display_Fast()
    EPD_Display_Fast(Image_BW);
    unsigned long endTime_displayFast = millis();  // End time for EPD_Display_Fast()
    unsigned long duration_displayFast =
      endTime_displayFast - startTime_displayFast;

    EPD_Sleep();
    unsigned long endTime_displayFile = millis();  // End time for displayFile()
    unsigned long duration_displayFile =
      endTime_displayFile - startTime_displayFile;

    Serial.printf(
      "displayFile(%s) - Total time: %lu ms, EPD_Display_Fast time: %lu ms\n",
      filename, duration_displayFile, duration_displayFast);
  } else {
    Serial.println("ERROR: Failed to read any bytes from file.");
  }
}

// Add near the top with other global variables
int currentVariantIndex = 0;  // Track which variant we're showing
String lastTimeKey = "";      // Track the last time we displayed
String currentChain = "";     // Track which chain we're currently in (e.g., "s1")
int currentChainPosition = 0; // Track position in current chain (e.g., 1 for p1)
int currentChainTotal = 0;    // Track total length of current chain

// <<< MODIFIED FUNCTION: Gets the mapped filename for a given time >>>
String getFilenameForTime(int hour12, int minute, bool isQR) {
  // Create the lookup key, e.g., "6_57"
  String timeKey = String(hour12) + "_";
  if (minute < 10) {
    timeKey += "0";
  }
  timeKey += String(minute);

  // Enhanced debug output
  Serial.print("Looking up time key: ");
  Serial.println(timeKey);
  Serial.print("Last time key was: ");
  Serial.println(lastTimeKey);
  Serial.print("Current variant index: ");
  Serial.println(currentVariantIndex);
  Serial.print("Current chain: ");
  Serial.println(currentChain);
  Serial.print("Chain position: ");
  Serial.println(currentChainPosition);

  // Find the key in the map
  auto it = timeToFileMap.find(timeKey);

  if (it != timeToFileMap.end()) {
    // Key found, get the full entry (e.g., "6:1_01_v1,1_01_v2,...")
    String fullEntry = it->second;

    // Debug output to see what we're working with
    Serial.print("Raw mapping entry: ");
    Serial.println(fullEntry);

    int colonIndex = fullEntry.indexOf(':');
    if (colonIndex == -1) {
      Serial.println("ERROR: Invalid mapping format (no variant count found)");
      return "";
    }

    int variants = fullEntry.substring(0, colonIndex).toInt();
    if (variants <= 0) {
      Serial.println("ERROR: Invalid variant count in mapping");
      return "";
    }

    // Use everything after the first colon as the variant list
    String variantList = fullEntry.substring(colonIndex + 1);

    Serial.printf("Found %d variants in mapping, list: %s\n", variants, variantList.c_str());

    // Check if we're continuing a chain from the previous minute
    bool continuingChain = false;
    if (currentChain.length() > 0) {
      // Calculate the expected next position in the chain
      int expectedPosition = currentChainPosition + 1;
      // Look for a variant that continues our current chain
      for (int i = 0; i < variants; i++) {
        int startPos = 0;
        int endPos;
        for (int j = 0; j < i; j++) {
          startPos = variantList.indexOf(',', startPos) + 1;
        }
        endPos = variantList.indexOf(',', startPos);
        if (endPos == -1) endPos = variantList.length();
        String variant = variantList.substring(startPos, endPos);
        
        if (variant.indexOf("_" + currentChain + "p" + String(expectedPosition)) != -1) {
          // Found the next position in our chain
          currentVariantIndex = i;
          currentChainPosition = expectedPosition;
          continuingChain = true;
          Serial.printf("Continuing chain %s at position %d\n", currentChain.c_str(), expectedPosition);
          break;
        }
      }
    }

    // If we're not continuing a chain, check if this time has any chain variants
    if (!continuingChain) {
      // Only reset if we're showing a different time
      if (timeKey != lastTimeKey) {
        // First check if any variant in this time starts a new chain
        bool foundNewChain = false;
        for (int i = 0; i < variants; i++) {
          int startPos = 0;
          int endPos;
          for (int j = 0; j < i; j++) {
            startPos = variantList.indexOf(',', startPos) + 1;
          }
          endPos = variantList.indexOf(',', startPos);
          if (endPos == -1) endPos = variantList.length();
          String variant = variantList.substring(startPos, endPos);
          
          if (variant.indexOf("_s") != -1) {
            // Extract chain info (e.g., "s1p1-3" -> chain="s1", pos=1, total=3)
            int chainStart = variant.indexOf("_s") + 1;
            int chainEnd = variant.indexOf("p", chainStart);
            if (chainEnd != -1) {
              String chain = variant.substring(chainStart, chainEnd);
              int posStart = chainEnd + 1;
              int posEnd = variant.indexOf("-", posStart);
              if (posEnd != -1) {
                int pos = variant.substring(posStart, posEnd).toInt();
                int total = variant.substring(posEnd + 1).toInt();
                
                // Only start tracking if this is position 1 in the chain
                if (pos == 1) {
                  currentChain = chain;
                  currentChainPosition = pos;
                  currentChainTotal = total;
                  currentVariantIndex = i;
                  foundNewChain = true;
                  Serial.printf("Starting new chain %s at position %d of %d\n", 
                               chain.c_str(), pos, total);
                  break;
                }
              }
            }
          }
        }
        
        // If we didn't find a new chain to start, reset chain info
        if (!foundNewChain) {
          currentVariantIndex = 0;
          currentChain = "";
          currentChainPosition = 0;
          currentChainTotal = 0;
          Serial.println("New time detected, no chain continuation found, resetting variant index and chain info");
        }
        lastTimeKey = timeKey;
      }

      // Get the specific variant
      String selectedVariant;
      int startPos = 0;
      int endPos;
      int currentVariant = 0;

      // Find the nth variant (where n is currentVariantIndex)
      while (currentVariant <= currentVariantIndex) {
        endPos = variantList.indexOf(',', startPos);

        if (endPos == -1) {
          // Last variant or only variant
          selectedVariant = variantList.substring(startPos);
          break;
        } else if (currentVariant == currentVariantIndex) {
          // Found our variant
          selectedVariant = variantList.substring(startPos, endPos);
          break;
        }
        startPos = endPos + 1;
        currentVariant++;
      }

      Serial.printf("Current variant index: %d, Selected variant: %s\n",
                    currentVariantIndex, selectedVariant.c_str());

      // Increment for next time, wrapping around if needed
      currentVariantIndex = (currentVariantIndex + 1) % variants;
      Serial.printf("Next variant index will be: %d\n", currentVariantIndex);

      String fullPath = "/" + selectedVariant + (isQR ? "_qr.bin" : ".bin");
      Serial.printf("Final path: %s\n", fullPath.c_str());
      return fullPath;
    } else {
      // We're continuing a chain, get the current variant
      String selectedVariant;
      int startPos = 0;
      int endPos;
      for (int i = 0; i < currentVariantIndex; i++) {
        startPos = variantList.indexOf(',', startPos) + 1;
      }
      endPos = variantList.indexOf(',', startPos);
      if (endPos == -1) endPos = variantList.length();
      selectedVariant = variantList.substring(startPos, endPos);

      String fullPath = "/" + selectedVariant + (isQR ? "_qr.bin" : ".bin");
      Serial.printf("Final path (chain continuation): %s\n", fullPath.c_str());
      return fullPath;
    }
  } else {
    Serial.printf("ERROR: No mapping found for time key: %s\n", timeKey.c_str());
    return "";  // Return empty string to indicate failure
  }
}

// <<< MODIFIED FUNCTION >>>
void updateDisplayToTimeFile() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    lastMinute = timeinfo.tm_min;
    int hour24 = timeinfo.tm_hour;
    int minute = timeinfo.tm_min;
    int hour12 = (hour24 % 12 == 0) ? 12 : (hour24 % 12);

    // *** MODIFIED PART ***
    // Get filename from mapping
    String filename = getFilenameForTime(hour12, minute, false);

    if (filename.length() > 0) {
      Serial.print("Updating display with mapped time file: ");
      Serial.println(filename);
      displayFile(filename.c_str());

      // If we're in a chain, check if we need to update the next minute's display
      if (currentChain.length() > 0 && currentChainPosition < currentChainTotal) {
        // Calculate next minute
        int nextMinute = (minute + 1) % 60;
        int nextHour12 = (nextMinute == 0) ? ((hour12 % 12) + 1) : hour12;
        
        // Get the next file in the chain
        String nextFilename = getFilenameForTime(nextHour12, nextMinute, false);
        if (nextFilename.length() > 0) {
          Serial.print("Next file in chain: ");
          Serial.println(nextFilename);
          // Store the next filename to be displayed when the minute changes
          lastTimeKey = String(nextHour12) + "_" + (nextMinute < 10 ? "0" : "") + String(nextMinute);
        }
      }
    } else {
      Serial.println("Failed to get mapped filename for current time.");
    }
    // *** END MODIFIED PART ***

  } else {
    Serial.println("Failed to get local time for update.");
  }
}

// --- Offline Configuration Display Functions ---
// offlineState 0: editing hour (1–12), 1 = editing tens-of-minutes (0–5), 2 = editing ones-of-minutes (0–9)
void displayOfflineConfig() {
  clear_all();
  char buf[32];
  int minuteValue = offlineTens * 10 + offlineOnes;
  sprintf(buf, "Set Time: %02d:%02d", offlineHour, minuteValue);
  EPD_ShowString(10, 10, "Offline Config Mode", 16, BLACK);
  EPD_ShowString(10, 30, buf, 16, BLACK);
  if (offlineState == 0) {
    EPD_ShowString(10, 50, "Editing Hours (1-12)", 16, BLACK);
  } else if (offlineState == 1) {
    EPD_ShowString(10, 50, "Editing 10s of Mins (0-5)", 16, BLACK);
  } else {
    EPD_ShowString(10, 50, "Editing Mins (0-9)", 16, BLACK);
  }
  EPD_ShowString(10, 70, "PRV: dec, NEXT: inc", 16, BLACK);
  EPD_ShowString(10, 90, "Press OK to toggle field", 16, BLACK);
  EPD_ShowString(10, 110, "Hold OK 2 sec to confirm", 16, BLACK);
  EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
}

// <<< MODIFIED FUNCTION >>>
// In offline operating mode, we want to use the bin files like online mode.
// This function calculates the current offline time, converts it to 12-hour format,
// builds the filename (e.g., "/1_02.bin"), and displays that file.
void displayOfflineBin() {
  unsigned long elapsed = (millis() - offlineLastUpdate) / 1000;
  unsigned long currentSec = offlineBaseTime + elapsed;
  int hour24 = (currentSec / 3600) % 24;
  int minute = (currentSec / 60) % 60;
  int hour12 = (hour24 % 12 == 0) ? 12 : (hour24 % 12);

  // *** MODIFIED PART ***
  // Get filename from mapping
  String filename = getFilenameForTime(hour12, minute, false);

  if (filename.length() > 0) {
    Serial.print("Offline bin mode: Displaying mapped file: ");
    Serial.println(filename);
    displayFile(filename.c_str());

    // If we're in a chain, check if we need to update the next minute's display
    if (currentChain.length() > 0 && currentChainPosition < currentChainTotal) {
      // Calculate next minute
      int nextMinute = (minute + 1) % 60;
      int nextHour12 = (nextMinute == 0) ? ((hour12 % 12) + 1) : hour12;
      
      // Get the next file in the chain
      String nextFilename = getFilenameForTime(nextHour12, nextMinute, false);
      if (nextFilename.length() > 0) {
        Serial.print("Next file in chain: ");
        Serial.println(nextFilename);
        // Store the next filename to be displayed when the minute changes
        lastTimeKey = String(nextHour12) + "_" + (nextMinute < 10 ? "0" : "") + String(nextMinute);
      }
    }
  } else {
    Serial.println("Failed to get mapped filename for offline time.");
  }
  // *** END MODIFIED PART ***
}

// --- SD Card Configuration File Functions ---

// <<< NEW FUNCTION: Loads the mapping.txt file >>>
bool loadMappingFile() {
  File mappingFile = SD.open("/mapping.txt", FILE_READ);
  if (!mappingFile) {
    Serial.println("ERROR: mapping.txt not found!");
    // Optional: Display an error on the EPD
    clear_all();
    EPD_ShowString(10, 10, "ERROR:", 16, BLACK);
    EPD_ShowString(10, 30, "mapping.txt", 16, BLACK);
    EPD_ShowString(10, 50, "not found!", 16, BLACK);
    EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
    return false;
  }

  Serial.println("Loading mapping.txt...");
  timeToFileMap.clear();  // Clear any previous mapping

  while (mappingFile.available()) {
    String line = mappingFile.readStringUntil('\n');
    line.trim();
    int firstColon = line.indexOf(':');
    if (firstColon != -1 && firstColon > 0) {  // Ensure colon exists and isn't first char
      String timeKey = line.substring(0, firstColon);
      String restOfLine = line.substring(firstColon + 1);  // Get everything after first colon

      // Store the entire rest of the line (count:variants) in the map
      timeToFileMap[timeKey] = restOfLine;
      Serial.printf("Mapped %s -> %s\n", timeKey.c_str(), restOfLine.c_str());
    } else if (line.length() > 0) {  // Ignore empty lines but warn about malformed ones
      Serial.printf("WARNING: Malformed line in mapping.txt: %s\n", line.c_str());
    }
  }
  mappingFile.close();
  Serial.printf("Loaded %d time mappings.\n", timeToFileMap.size());

  if (timeToFileMap.empty()) {
    Serial.println("ERROR: mapping.txt was empty or contained no valid mappings.");
    // Optional: Display error
    clear_all();
    EPD_ShowString(10, 10, "ERROR:", 16, BLACK);
    EPD_ShowString(10, 30, "mapping.txt", 16, BLACK);
    EPD_ShowString(10, 50, "empty/invalid!", 16, BLACK);
    EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
    return false;
  }

  // Debug: Print all mappings
  Serial.println("\nCurrent mappings:");
  for (const auto &pair : timeToFileMap) {
    Serial.printf("%s -> %s\n", pair.first.c_str(), pair.second.c_str());
  }
  Serial.println();

  return true;
}

void loadConfig() {
  if (!SD.exists("/config.txt")) {
    File configFile = SD.open("/config.txt", FILE_WRITE);
    if (configFile) {
      configFile.println("SSID=");
      configFile.println("PASSWORD=");
      configFile.println("TIMEZONE=-21600");  // Default to CST
      configFile.close();
      Serial.println("Created default config.txt");
    } else {
      Serial.println("Failed to create config.txt");
    }
  }
  File configFile = SD.open("/config.txt", FILE_READ);
  if (configFile) {
    while (configFile.available()) {
      String line = configFile.readStringUntil('\n');
      line.trim();
      if (line.startsWith("SSID=")) {
        wifiSSID = line.substring(5);
      } else if (line.startsWith("PASSWORD=")) {
        wifiPassword = line.substring(9);
      } else if (line.startsWith("TIMEZONE=")) {
        timeZone = line.substring(9);
      }
    }
    configFile.close();
    Serial.println("Loaded config:");
    Serial.println("SSID: " + wifiSSID);
    // Avoid printing password directly to serial
    Serial.println(wifiPassword.length() > 0 ? "Password: [set]" : "Password: [not set]");
    Serial.println("Timezone: " + timeZone);
  } else {
    Serial.println("Failed to open config.txt for reading.");
  }

  if (timeZone.length() == 0) {
    timeZone = "-21600";  // default to CST if not set
    Serial.println("Timezone not found in config, defaulting to CST (-21600).");
  }
  gmtOffset_sec = timeZone.toInt();
}

// --- Web Server Handlers ---
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><title>ESP32 Configuration</title>";
  html += "<style>";
  html += "body {font-family: Arial, sans-serif; text-align: center; margin-top: "
          "50px; font-size: 20px; background-color: #f0f0f0;}";
  html += "h1 {font-size: 36px; margin-bottom: 30px;}";
  html += "label {font-size: 22px; margin-top: 20px; display: block;}";
  html += "input[type='text'], select { font-size: 20px; padding: 10px; margin: "
          "10px auto; width: 80%; display: block; }";
  html += "input[type='submit'] { font-size: 24px; padding: 10px 20px; "
          "margin-top: 30px; }";
  html += "</style></head><body>";
  html += "<h1>ESP32 Configuration</h1>";
  if (wifiSSID.length() > 0) {
    html += "<p>Connect to network: " + wifiSSID + "</p>";
  } else {
    html += "<p>Connect to network: Bible_Clock_Config</p>";
  }
  html += "<form action='/save' method='POST'>";
  html += "<label>WiFi SSID:</label>";
  html += "<input type='text' name='ssid' value='" + wifiSSID + "'>";
  html += "<label>WiFi Password:</label>";
  html += "<input type='text' name='password' value='" + wifiPassword + "'>";
  html += "<label>Time Zone:</label>";
  html += "<select name='timezone'>";
  html += String("<option value='-21600'") + (timeZone == "-21600" ? " selected" : "") + String(">Central Standard Time</option>");
  html += String("<option value='-18000'") + (timeZone == "-18000" ? " selected" : "") + String(">Eastern Standard Time</option>");
  html += String("<option value='-25200'") + (timeZone == "-25200" ? " selected" : "") + String(">Mountain Standard Time</option>");
  html += String("<option value='-28800'") + (timeZone == "-28800" ? " selected" : "") + String(">Pacific Standard Time</option>");
  html += String("<option value='0'") + (timeZone == "0" ? " selected" : "") + String(">UTC</option>");
  html += "</select>";
  html += "<input type='submit' value='Save'>";
  html += "</form>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("password") && server.hasArg("timezone")) {
    String newSSID = server.arg("ssid");
    String newPassword = server.arg("password");
    String newTimezone = server.arg("timezone");
    File configFile = SD.open("/config.txt", FILE_WRITE);
    if (configFile) {
      configFile.println("SSID=" + newSSID);
      configFile.println("PASSWORD=" + newPassword);
      configFile.println("TIMEZONE=" + newTimezone);
      configFile.close();
      server.send(200, "text/html",
                  "<html><body><h1>Configuration Saved. "
                  "Restarting...</h1></body></html>");
      delay(2000);
      ESP.restart();
    } else {
      server.send(500, "text/html",
                  "<html><body><h1>Failed to write "
                  "configuration.</h1></body></html>");
    }
  } else {
    server.send(400, "text/html",
                "<html><body><h1>Missing parameters.</h1></body></html>");
  }
}

// Add these global variables
const byte DNS_PORT = 53;
DNSServer dnsServer;
const IPAddress apIP(192, 168, 4, 1);  // IP address for AP mode

void startConfigMode() {
  configMode = true;
  String ipDisplay;

  if (wifiSSID.length() == 0) {
    // Configure AP with fixed IP
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
    WiFi.softAP("Bible_Clock_Config");

    // Start DNS server for captive portal
    dnsServer.start(DNS_PORT, "*", apIP);

    ipDisplay = WiFi.softAPIP().toString();
    Serial.println(
      "No WiFi credentials. Running in AP mode with captive portal.");
    Serial.print("AP IP address: ");
    Serial.println(ipDisplay);
  } else {
    // If already connected, use that IP, otherwise might be starting config mode manually
    if (WiFi.status() == WL_CONNECTED) {
      ipDisplay = WiFi.localIP().toString();
      Serial.print("Device IP address: ");
      Serial.println(ipDisplay);
    } else {
      // If starting config mode manually without AP mode needed
      ipDisplay = "N/A";  // Or some other indicator
      Serial.println("Starting config mode while potentially disconnected.");
    }
  }

  // Add captive portal handling
  server.on("/generate_204", handleRoot);  // Android captive portal
  server.on("/fwlink", handleRoot);        // Microsoft captive portal
  server.onNotFound(handleRoot);           // Catch-all handler

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("Configuration web server started.");
  displayConfigInstructions(ipDisplay);
}

void stopConfigMode() {
  configMode = false;
  if (wifiSSID.length() == 0) {
    dnsServer.stop();
  }
  server.stop();
  Serial.println("Exiting configuration mode.");
  // Clear display and show updated instructions
  clear_all();
  EPD_ShowString(10, 10, "Press EXIT to", 16, BLACK);
  EPD_ShowString(10, 30, "configure WiFi", 16, BLACK);
  EPD_ShowString(10, 50, "or hold HOME 3 sec", 16, BLACK);
  EPD_ShowString(10, 70, "to set time offline", 16, BLACK);
  EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
}

// --- Reset Configuration Function ---
void resetConfig() {
  Serial.println("Resetting configuration...");
  if (SD.exists("/config.txt")) {
    SD.remove("/config.txt");
    Serial.println("Config file removed.");
  }
  // Also remove mapping? Maybe not, user might want to keep images
  // if (SD.exists("/mapping.txt")) {
  //   SD.remove("/mapping.txt");
  //   Serial.println("Mapping file removed.");
  // }
  delay(500);
  ESP.restart();
}

// --- Offline Configuration Functions ---
// In offline config mode, we allow editing three fields:
// State 0: editing hour (1–12)
// State 1: editing tens-of-minutes (0–5)
// State 2: editing ones-of-minutes (0–9)
void startOfflineConfigMode() {
  offlineConfigMode = true;
  offlineHour = 12;
  offlineTens = 0;
  offlineOnes = 0;
  offlineState = 0;  // Start with editing hours
  Serial.println("Entering offline configuration mode.");
  displayOfflineConfig();
}

void processOfflineConfigMode() {
  static unsigned long lastKeyPress = 0;
  unsigned long now = millis();

  // --- Button Handling within Offline Config ---
  // Note: Interrupt flags are set, but we read digitalRead here for simplicity
  // within this specific mode's logic. Debounce is handled by lastKeyPress.

  if (now - lastKeyPress > 200) {  // 200ms debounce period
    // Use PRV_KEY to decrement the selected field.
    if (prvButtonInterrupt || digitalRead(PRV_KEY) == LOW) {
      prvButtonInterrupt = false;  // Consume flag
      lastKeyPress = now;
      if (offlineState == 0) {  // editing hour: 1 to 12
        offlineHour = (offlineHour == 1) ? 12 : offlineHour - 1;
        Serial.print("Offline Hour decreased: ");
        Serial.println(offlineHour);
      } else if (offlineState == 1) {  // tens-of-minutes: 0-5
        offlineTens = (offlineTens + 5) % 6;
        Serial.print("Offline Tens decreased: ");
        Serial.println(offlineTens);
      } else {  // ones-of-minutes: 0-9
        offlineOnes = (offlineOnes + 9) % 10;
        Serial.print("Offline Ones decreased: ");
        Serial.println(offlineOnes);
      }
      displayOfflineConfig();
    }
    // Use NEXT_KEY to increment the selected field.
    else if (nextButtonInterrupt || digitalRead(NEXT_KEY) == LOW) {
      nextButtonInterrupt = false;  // Consume flag
      lastKeyPress = now;
      if (offlineState == 0) {  // editing hour: 1 to 12
        offlineHour = (offlineHour % 12) + 1;
        Serial.print("Offline Hour increased: ");
        Serial.println(offlineHour);
      } else if (offlineState == 1) {  // tens-of-minutes: 0-5
        offlineTens = (offlineTens + 1) % 6;
        Serial.print("Offline Tens increased: ");
        Serial.println(offlineTens);
      } else {  // ones-of-minutes: 0-9
        offlineOnes = (offlineOnes + 1) % 10;
        Serial.print("Offline Ones increased: ");
        Serial.println(offlineOnes);
      }
      displayOfflineConfig();
    }
  }

  // OK Button Handling (Short press = toggle field, Long press = confirm)
  static bool okWasPressed = false;
  static unsigned long okPressStart = 0;
  bool confirmOffline = false;  // Flag to indicate confirmation

  // Check if OK button is currently pressed (using interrupt flag OR digitalRead)
  if (okButtonInterrupt || digitalRead(OK_KEY) == LOW) {
    okButtonInterrupt = false;  // Consume flag if set
    if (!okWasPressed) {
      okWasPressed = true;
      okPressStart = now;
      Serial.println("OK press started in offline config.");
    } else if (okWasPressed && (now - okPressStart >= 2000)) {  // Held for at least 2 seconds
      // Check if it's *still* pressed before confirming
      if (digitalRead(OK_KEY) == LOW) {
        Serial.println("OK long press detected - confirming offline time.");
        confirmOffline = true;  // Set confirmation flag
        okWasPressed = false;   // Prevent re-triggering toggle on release
      }
    }
  } else {  // OK_KEY is HIGH (released or wasn't pressed via interrupt)
    if (okWasPressed) {
      // OK button was released
      unsigned long pressDuration = now - okPressStart;
      okWasPressed = false;  // Reset pressed state
      Serial.printf("OK released after %lu ms.\n", pressDuration);
      // Only toggle if it wasn't a long press that led to confirmation
      if (!confirmOffline && pressDuration < 2000) {
        offlineState = (offlineState + 1) % 3;
        Serial.print("Toggled offline field to state: ");
        Serial.println(offlineState);
        displayOfflineConfig();
      }
    }
  }

  // Process confirmation if the flag was set
  if (confirmOffline) {
    confirmOffline = false;  // Reset the flag immediately after processing
    // Confirm offline configuration.
    offlineConfigMode = false;
    offlineOperating = true;
    int offlineMinute = offlineTens * 10 + offlineOnes;
    // Convert 12-hour input to 24-hour base for calculations if needed,
    // though base time calculation seems okay as is for elapsed time.
    // Example: If offlineHour is 12 AM -> 0, 1 PM -> 13 etc.
    // For simplicity, current calculation assumes 12 maps to 12 for base time.
    offlineBaseTime = offlineHour * 3600UL + offlineMinute * 60UL;
    offlineLastUpdate = millis();
    Serial.println("Offline configuration confirmed:");
    Serial.printf("Set Time: %02d:%02d\n", offlineHour, offlineMinute);
    Serial.printf("Base Seconds: %lu\n", offlineBaseTime);

    clear_all();
    EPD_ShowString(10, 10, "Offline Time Set", 16, BLACK);
    EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
    delay(1500);          // Give user time to see confirmation
    displayOfflineBin();  // Show the first image for the set time
  }
}

// Add near the top with other global variables
const String HELP_COMMAND = "help";
const String DELETE_WIFI_COMMAND = "delete_wifi";
const String TIME_STATUS_COMMAND = "time_status";
const String SET_TIME_COMMAND = "set_time";
const String SHOW_MAPPING_COMMAND = "show_mapping";
const String COMMANDS_HELP = R"(
Available Commands:
------------------
help         : Show this help message
delete_wifi  : Delete saved WiFi credentials
time_status  : Show current time and DST information
set_time     : Set time for testing (format: set_time HH:MM)
show_mapping : Show contents of mapping.txt file
<filename>   : Display specific image file from SD card (e.g., /1_00_v1.bin)
)";

// Add this function
void processSerialCommand(String command) {
  command.trim();  // Remove any whitespace

  if (command == HELP_COMMAND) {
    Serial.println(COMMANDS_HELP);
  } else if (command == SHOW_MAPPING_COMMAND) {
    File mappingFile = SD.open("/mapping.txt", FILE_READ);
    if (!mappingFile) {
      Serial.println("ERROR: mapping.txt not found!");
      return;
    }
    Serial.println("\nCurrent mapping.txt contents:");
    Serial.println("----------------------------");
    while (mappingFile.available()) {
      String line = mappingFile.readStringUntil('\n');
      line.trim();
      if (line.length() > 0) {
        Serial.println(line);
      }
    }
    Serial.println("----------------------------");
    mappingFile.close();
  } else if (command == DELETE_WIFI_COMMAND) {
    if (SD.exists("/config.txt")) {
      if (SD.remove("/config.txt")) {
        Serial.println("WiFi credentials deleted successfully.");
        Serial.println("Device will restart in 3 seconds...");
        delay(3000);
        ESP.restart();
      } else {
        Serial.println("Error: Failed to delete WiFi credentials.");
      }
    } else {
      Serial.println("No WiFi credentials found.");
    }
  } else if (command == TIME_STATUS_COMMAND) {
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      Serial.print("Current time: ");
      Serial.println(&timeinfo, "%F %T");  // Use standard format codes
      Serial.print("DST in effect: ");
      Serial.println(isDST(timeinfo) ? "Yes" : "No");
      Serial.print("Time zone offset (config): ");
      Serial.print(gmtOffset_sec / 3600);
      Serial.println(" hours");
      time_t now_utc;
      time(&now_utc);
      long current_offset = mktime(&timeinfo) - now_utc;
      Serial.print("Effective offset from UTC: ");
      Serial.print(current_offset / 3600);
      Serial.println(" hours");
    } else {
      Serial.println("Failed to get local time (maybe not synced yet?)");
    }
  } else if (command.startsWith(SET_TIME_COMMAND)) {
    // Format: set_time HH:MM
    String timeArg = command.substring(SET_TIME_COMMAND.length());
    timeArg.trim();  // Call trim() separately since it returns void
    if (timeArg.length() == 5 && timeArg[2] == ':') {
      int hour = timeArg.substring(0, 2).toInt();
      int minute = timeArg.substring(3, 5).toInt();

      if (hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59) {
        // Convert to 12-hour format for display
        int hour12 = (hour % 12 == 0) ? 12 : (hour % 12);

        // Force offline mode for testing
        if (!offlineOperating) {
          offlineOperating = true;
          configMode = false;
          offlineConfigMode = false;
        }

        // Set the offline time variables
        offlineHour = hour12;
        offlineTens = minute / 10;
        offlineOnes = minute % 10;
        offlineBaseTime = hour * 3600UL + minute * 60UL;
        offlineLastUpdate = millis();
        
        // Set offlineLastDisplayedMinute to current minute to prevent immediate cycling
        offlineLastDisplayedMinute = minute;

        // Force lastTimeKey to match current time to ensure proper variant cycling
        String timeKey = String(hour12) + "_" + (minute < 10 ? "0" : "") + String(minute);
        lastTimeKey = timeKey;

        Serial.printf("Time set to %02d:%02d (12-hour: %02d:%02d)\n",
                      hour, minute, hour12, minute);

        // Update display immediately
        displayOfflineBin();
      } else {
        Serial.println("Invalid time format. Use HH:MM (00-23:00-59)");
      }
    } else {
      Serial.println("Invalid format. Use: set_time HH:MM");
    }
  } else if (command.length() > 0) {
    // Existing file display functionality
    if (!command.startsWith("/")) {
      command = "/" + command;
    }
    // Basic validation for .bin extension
    if (command.endsWith(".bin")) {
      Serial.print("Displaying file via serial command: ");
      Serial.println(command);
      displayFile(command.c_str());
    } else {
      Serial.println("Invalid command or filename (must end with .bin)");
    }
  }
}

// --- Setup Function ---
void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 2000)
    ;  // Wait a bit for serial monitor
  Serial.println("\n\nStarting Bible Clock...");
  Serial.println(COMMANDS_HELP);

  // Display example mapping format
  Serial.println("\nExample mapping format:");
  Serial.println("12_30:2:12_30_v1,12_30_v2");
  Serial.println("Format: timeKey:variantCount:variant1,variant2,...\n");

  // Initialize button pins
  pinMode(HOME_KEY, INPUT_PULLUP);
  pinMode(EXIT_KEY, INPUT_PULLUP);
  pinMode(OK_KEY, INPUT_PULLUP);
  pinMode(PRV_KEY, INPUT_PULLUP);
  pinMode(NEXT_KEY, INPUT_PULLUP);

  // Attach interrupts for buttons (FALLING edge - button press)
  attachInterrupt(digitalPinToInterrupt(HOME_KEY), homeButtonISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(EXIT_KEY), exitButtonISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(OK_KEY), okButtonISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(PRV_KEY), prvButtonISR, FALLING);
  attachInterrupt(digitalPinToInterrupt(NEXT_KEY), nextButtonISR, FALLING);

  // --- Initialize SD Card ---
  pinMode(42, OUTPUT);  // SD Card Power Pin? Check your board schematic
  digitalWrite(42, HIGH);
  delay(100);  // Allow power to stabilize
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN);
  if (!SD.begin(SD_CS_PIN, sdSPI, 80000000)) {  // 80MHz SPI might be too fast for some cards/wiring
                                                // if (!SD.begin(SD_CS_PIN, sdSPI)) { // Try default speed if 80MHz fails
    Serial.println("ERROR: SD Mount Failed!");
    // Display error on EPD
    pinMode(7, OUTPUT);  // EPD Power
    digitalWrite(7, HIGH);
    EPD_GPIOInit();
    EPD_Clear();
    Paint_NewImage(Image_BW, EPD_W, EPD_H, 0, BLACK);
    EPD_ShowString(10, 10, "ERROR:", 16, BLACK);
    EPD_ShowString(10, 30, "SD Card Mount", 16, BLACK);
    EPD_ShowString(10, 50, "Failed!", 16, BLACK);
    EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
    while (1) {
      delay(1000);
    }  // Halt
  } else {
    uint64_t sdSizeMB = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD Size: %lluMB\n", sdSizeMB);
    delay(100);  // Short delay after successful mount
  }
  // delay(2000); // Removed long delay

  // --- Load Configuration ---
  loadConfig();

  // --- <<< LOAD MAPPING FILE >>> ---
  if (!loadMappingFile()) {
    Serial.println("Halting due to mapping file error.");
    // Error message is already displayed by loadMappingFile()
    while (1) {
      delay(1000);
    }  // Halt execution
  }
  // --- <<< END LOAD MAPPING FILE >>> ---

  // --- Connect to WiFi or Start AP Mode ---
  if (wifiSSID.length() > 0) {
    WiFi.begin(wifiSSID.c_str(), wifiPassword.c_str());
    Serial.print("Connecting to WiFi");
    unsigned long wifiConnectStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiConnectStart < 30000) {
      delay(500);
      Serial.print(".");
    }
    Serial.println();  // Newline after dots

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Failed to connect to WiFi. Please check your credentials.");
      // Don't start AP mode automatically here, let user trigger config mode
      configMode = false;       // Ensure not in config mode
      offlineOperating = true;  // Default to offline if WiFi fails? Or show error?
      Serial.println("Entering OFFLINE operating mode due to WiFi failure.");
      // Display WiFi error message
      pinMode(7, OUTPUT);  // EPD Power
      digitalWrite(7, HIGH);
      EPD_GPIOInit();
      clear_all();
      EPD_ShowString(10, 10, "WiFi Connect Failed", 16, BLACK);
      EPD_ShowString(10, 30, "Check credentials.", 16, BLACK);
      EPD_ShowString(10, 50, "Press EXIT for WiFi setup", 16, BLACK);
      EPD_ShowString(10, 70, "or HOME (3s) for offline.", 16, BLACK);
      EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
      // Don't start config mode webserver here, wait for EXIT press
    } else {
      Serial.print("Connected! IP Address: ");
      Serial.println(WiFi.localIP());
      offlineOperating = false;  // Ensure offline mode is off
      // delay(2000); // Removed long delay
    }
  } else {
    // No WiFi credentials found. Start in AP mode for configuration.
    Serial.println("No WiFi credentials found. Starting AP mode for configuration.");
    configMode = true;  // Set config mode flag
    // AP mode will be started by startConfigMode() later if configMode is true
  }

  // --- Synchronize Time via NTP (only if not in config/offline mode and WiFi connected) ---
  if (!configMode && !offlineOperating && WiFi.status() == WL_CONNECTED) {
    // First configure time without DST to get current date
    Serial.printf("Configuring time: NTP Server=%s, GMT Offset=%ld, DST Offset=%d\n", ntpServer, gmtOffset_sec, 0);
    configTime(gmtOffset_sec, 0, ntpServer);
    struct tm timeinfo;
    Serial.print("Waiting for time synchronization");
    unsigned long syncStart = millis();
    // Wait longer for initial sync, but not forever
    while (!getLocalTime(&timeinfo, 10000)) {  // Wait up to 10 seconds for a result
      if (millis() - syncStart > 60000) {      // Timeout after 60 seconds
        Serial.println("\nNTP Sync Failed!");
        // Consider falling back to offline mode or showing error
        offlineOperating = true;
        Serial.println("Entering OFFLINE operating mode due to NTP failure.");
        break;  // Exit sync loop
      }
      delay(1000);  // Check every second
      Serial.print(".");
    }
    Serial.println();

    // Only proceed if sync didn't fail
    if (!offlineOperating) {
      Serial.print("Initial time check: ");
      Serial.println(&timeinfo, "%F %T");

      // Check if DST is in effect and reconfigure if needed
      if (isDST(timeinfo)) {
        Serial.println("DST is in effect, reconfiguring time with +1 hour offset");
        configTime(gmtOffset_sec, 3600, ntpServer);  // Apply DST offset (3600 seconds)
        // Get updated time with DST applied
        delay(1000);                           // Give time for change to propagate
        if (!getLocalTime(&timeinfo, 5000)) {  // Wait up to 5 seconds
          Serial.println("Failed to get time after DST adjustment!");
          // Handle error?
        }
      } else {
        Serial.println("DST is not in effect");
      }

      Serial.print("Final time set: ");
      Serial.println(&timeinfo, "%F %T");
      lastMinute = timeinfo.tm_min;  // Initialize lastMinute
    }
    // delay(2000); // Removed long delay
  } else if (!configMode && !offlineOperating) {
    Serial.println("Skipping NTP sync (No WiFi connection).");
    offlineOperating = true;  // Ensure offline mode if no WiFi
    Serial.println("Entering OFFLINE operating mode.");
  }


  // --- Initialize the E-Paper Display ---
  pinMode(7, OUTPUT);
  digitalWrite(7, HIGH);
  EPD_GPIOInit();
  EPD_Clear();                                       // Clear display initially
  Paint_NewImage(Image_BW, EPD_W, EPD_H, 0, BLACK);  // Initialize buffer
  // Don't do a full black/white cycle here, just init fast mode
  EPD_Init_Fast(Fast_Seconds_1_5s);
  // delay(2000); // Removed long delay

  // --- Initial Display Update ---
  if (configMode) {
    startConfigMode();  // This will display config instructions
  } else if (offlineOperating) {
    Serial.println("Performing initial offline display update...");
    // If offline mode was entered due to error, show appropriate message first
    if (WiFi.status() != WL_CONNECTED && wifiSSID.length() > 0) {
      // Already displayed WiFi error message
    } else if (timeToFileMap.empty()) {
      // Already displayed mapping error message
    } else {
      // Normal offline start or manual offline mode
      clear_all();
      EPD_ShowString(10, 10, "OFFLINE MODE", 16, BLACK);
      EPD_ShowString(10, 30, "Set time via HOME (3s)", 16, BLACK);
      EPD_ShowString(10, 50, "or EXIT for WiFi setup", 16, BLACK);
      EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
      // Don't display a time yet, wait for user to set it or for loop
    }
  } else if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Performing initial online display update...");
    updateDisplayToTimeFile();  // This will now use the mapping
  } else {
    // Should not happen based on logic above, but as fallback:
    Serial.println("Unknown state at end of setup. Displaying default.");
    clear_all();
    EPD_ShowString(10, 10, "Starting...", 16, BLACK);
    EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
  }

  Serial.println("Setup complete.");
}

// --- Main Loop ---
void loop() {
  struct tm timeinfo;  // Declare here for wider scope if needed
  unsigned long currentMillis = millis();

  // Handle DNS requests if in AP config mode
  if (configMode && wifiSSID.length() == 0) {
    dnsServer.processNextRequest();
  }

  // Handle web server requests if in config mode
  if (configMode) {
    server.handleClient();
    // Don't return immediately, allow button checks below if needed
  }

  // --- Button Interrupt Handling ---
  // Process flags set by ISRs

  if (homeButtonInterrupt) {
    homeButtonInterrupt = false;  // Reset flag immediately
    Serial.println("HOME_KEY interrupt detected");
    unsigned long homePressStart = millis();

    // Debounce/Long press check
    delay(50);                           // Simple debounce delay
    if (digitalRead(HOME_KEY) == LOW) {  // Check if still pressed
      while (digitalRead(HOME_KEY) == LOW) {
        // Long press check for offline config mode trigger
        if (millis() - homePressStart >= 3000 && wifiSSID.length() == 0 && !configMode && !offlineOperating && !offlineConfigMode) {
          Serial.println("HOME: 3-second press detected - entering offline config mode");
          startOfflineConfigMode();
          return;  // Exit loop iteration to handle offline config mode
        }
        delay(10);  // Small delay while checking long press
      }
      // Button was released
      unsigned long homePressDuration = millis() - homePressStart;
      Serial.println("HOME: Press duration: " + String(homePressDuration) + "ms");

      // --- Process Short Press Action ---
      if (homePressDuration < 3000) {
        // Only process QR display if online or offline time is set
        if (!configMode && !offlineConfigMode && (WiFi.status() == WL_CONNECTED || offlineOperating)) {
          if (!waitingForTimeCheck) {
            Serial.println("HOME: Short Press - Showing QR Code");
            waitingForTimeCheck = true;
            homePressTime = currentMillis;  // Use currentMillis captured at loop start

            // Display QR code
            String qrFilename = "";  // Initialize empty
            int hour12 = 0;
            int minute = 0;

            if (offlineOperating) {
              // Offline mode: use offline time variables
              unsigned long elapsed = (millis() - offlineLastUpdate) / 1000;
              unsigned long currentSec = offlineBaseTime + elapsed;
              int hour24 = (currentSec / 3600) % 24;
              minute = (currentSec / 60) % 60;
              hour12 = (hour24 % 12 == 0) ? 12 : (hour24 % 12);
              Serial.print("Offline QR time: ");
            } else if (getLocalTime(&timeinfo)) {  // Online mode
              int hour24 = timeinfo.tm_hour;
              minute = timeinfo.tm_min;
              hour12 = (hour24 % 12 == 0) ? 12 : (hour24 % 12);
              Serial.print("Online QR time: ");
            } else {
              Serial.println("Cannot get time for QR code.");
              waitingForTimeCheck = false;  // Cancel wait
            }

            // If we have a valid time, get the mapped filename
            if (hour12 != 0) {
              Serial.printf("%d:%02d\n", hour12, minute);
              qrFilename = getFilenameForTime(hour12, minute, true);  // Get QR filename
            }

            // Display if filename is valid
            if (qrFilename.length() > 0) {
              Serial.print("Displaying QR file (mapped): ");
              Serial.println(qrFilename);
              displayFile(qrFilename.c_str());
            } else {
              Serial.println("Failed to get mapped QR filename.");
              // Optional: Display error or do nothing
              waitingForTimeCheck = false;  // Cancel waiting if QR file not found
            }
          } else {
            // Second short press while waiting: return to normal time display
            Serial.println("HOME: Second Short Press - Returning to Time Display");
            waitingForTimeCheck = false;
            if (offlineOperating) {
              displayOfflineBin();  // This now uses the mapping
            } else {
              updateDisplayToTimeFile();  // This now uses the mapping
            }
          }
        } else {
          Serial.println("HOME: Short press ignored (in config mode or no time source).");
        }
      }
    } else {
      Serial.println("HOME: Press too short (bounce?) - ignored.");
    }
  }

  if (exitButtonInterrupt) {
    exitButtonInterrupt = false;  // Reset flag immediately
    Serial.println("EXIT_KEY interrupt detected");
    unsigned long exitPressStart = millis();

    // Debounce/Long press check
    delay(50);                           // Simple debounce
    if (digitalRead(EXIT_KEY) == LOW) {  // Check if still pressed
      while (digitalRead(EXIT_KEY) == LOW) {
        // Check for long press for reset prompt
        if (millis() - exitPressStart >= 3000 && !resetPromptActive) {
          Serial.println("EXIT: Long press DETECTED - Activating Reset Prompt");
          resetPromptActive = true;
          resetPromptStart = millis();
          clear_all();
          EPD_ShowString(10, 10, "Reset Config?", 16, BLACK);
          EPD_ShowString(10, 30, "Hold EXIT 3 sec then", 16, BLACK);
          EPD_ShowString(10, 50, "press OK within 5 sec", 16, BLACK);
          EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
          // No need to break, just activate the prompt state
        }
        delay(10);  // Small delay while checking
      }
      // Button was released
      unsigned long exitPressDuration = millis() - exitPressStart;
      Serial.println("EXIT: Press duration: " + String(exitPressDuration) + "ms");

      // --- Process Short Press Action ---
      if (exitPressDuration < 3000) {
        Serial.println("EXIT: Short press detected.");
        if (resetPromptActive) {
          // Short press while reset prompt is active cancels it
          Serial.println("Reset prompt cancelled by EXIT short press.");
          resetPromptActive = false;
          // Restore appropriate display
          if (offlineOperating) displayOfflineBin();
          else if (configMode) displayConfigInstructions(WiFi.localIP().toString());  // Re-show config
          else updateDisplayToTimeFile();
        } else if (configMode) {
          Serial.println("Exiting configuration mode via EXIT short press.");
          stopConfigMode();  // Stop web server etc.
          // Decide what state to enter after exiting config
          if (wifiSSID.length() > 0 && WiFi.status() == WL_CONNECTED) {
            offlineOperating = false;
            updateDisplayToTimeFile();  // Go to online mode if possible
          } else {
            offlineOperating = true;  // Otherwise default to offline
            // Display offline instructions
            clear_all();
            EPD_ShowString(10, 10, "OFFLINE MODE", 16, BLACK);
            EPD_ShowString(10, 30, "Set time via HOME (3s)", 16, BLACK);
            EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
          }
        } else if (offlineOperating || offlineConfigMode) {
          Serial.println("Exiting offline mode via EXIT short press -> Entering Config Mode.");
          offlineOperating = false;
          offlineConfigMode = false;
          configMode = true;  // Enter config mode
          startConfigMode();  // Start web server, display instructions
        } else if (!configMode && !offlineOperating && !offlineConfigMode) {
          // Normal online mode -> Enter Config Mode
          Serial.println("Entering config mode from normal operation via EXIT short press.");
          configMode = true;
          startConfigMode();
        }
      }
    } else {
      Serial.println("EXIT: Press too short (bounce?) - ignored.");
    }
  }

  if (okButtonInterrupt) {
    okButtonInterrupt = false;  // Reset flag immediately
    Serial.println("OK_KEY interrupt detected");
    delay(50);  // Debounce

    if (digitalRead(OK_KEY) == LOW) {  // Check if still pressed
      if (resetPromptActive) {
        if (millis() - resetPromptStart < 5000) {  // Check within 5 sec window
          Serial.println("OK pressed during reset prompt. Resetting configuration...");
          resetConfig();  // This function restarts the ESP
                          // Code below won't execute after restart
        } else {
          // OK pressed *after* timeout - ignore reset, cancel prompt
          Serial.println("OK pressed after reset prompt timed out. Canceling.");
          resetPromptActive = false;
          // Restore appropriate display
          if (offlineOperating) displayOfflineBin();
          else if (configMode) displayConfigInstructions(WiFi.localIP().toString());
          else updateDisplayToTimeFile();
        }
      } else if (offlineConfigMode) {
        // OK press in offline config is handled within processOfflineConfigMode
        // We call it below in the main state machine section
        Serial.println("OK press detected - will be handled by processOfflineConfigMode.");
      } else if (!configMode && !offlineConfigMode && !resetPromptActive) {
        // Force a variant change for the current time
        struct tm timeinfo;
        int hour12, minute;

        if (offlineOperating) {
          // Get current offline time
          unsigned long elapsed = (millis() - offlineLastUpdate) / 1000;
          unsigned long currentSec = offlineBaseTime + elapsed;
          int hour24 = (currentSec / 3600) % 24;
          minute = (currentSec / 60) % 60;
          hour12 = (hour24 % 12 == 0) ? 12 : (hour24 % 12);
        } else if (getLocalTime(&timeinfo)) {
          int hour24 = timeinfo.tm_hour;
          minute = timeinfo.tm_min;
          hour12 = (hour24 % 12 == 0) ? 12 : (hour24 % 12);
        } else {
          Serial.println("Cannot get time for manual variant cycle");
          return;
        }

        // Construct the time key
        String timeKey = String(hour12) + "_";
        if (minute < 10) {
          timeKey += "0";
        }
        timeKey += String(minute);

        // Force lastTimeKey to match current time
        // This prevents index reset in getFilenameForTime
        lastTimeKey = timeKey;

        Serial.print("Manual variant cycle for time: ");
        Serial.println(timeKey);

        // Display the variant
        if (offlineOperating) {
          displayOfflineBin();
        } else {
          updateDisplayToTimeFile();
        }
      } else {
        Serial.println("OK press ignored (no active prompt/mode).");
      }
    } else {
      Serial.println("OK: Press too short (bounce?) - ignored.");
    }
  }

  if (prvButtonInterrupt) {
    prvButtonInterrupt = false;  // Reset flag immediately
    Serial.println("PRV_KEY interrupt detected");
    delay(50);  // Debounce
    if (digitalRead(PRV_KEY) == LOW) {
      if (offlineConfigMode) {
        // PRV press in offline config is handled within processOfflineConfigMode
        Serial.println("PRV press detected - will be handled by processOfflineConfigMode.");
      } else {
        Serial.println("PRV press ignored (not in offline config mode).");
      }
    } else {
      Serial.println("PRV: Press too short (bounce?) - ignored.");
    }
  }

  if (nextButtonInterrupt) {
    nextButtonInterrupt = false;  // Reset flag immediately
    Serial.println("NEXT_KEY interrupt detected");
    delay(50);  // Debounce
    if (digitalRead(NEXT_KEY) == LOW) {
      if (offlineConfigMode) {
        // NEXT press in offline config is handled within processOfflineConfigMode
        Serial.println("NEXT press detected - will be handled by processOfflineConfigMode.");
      } else {
        Serial.println("NEXT press ignored (not in offline config mode).");
      }
    } else {
      Serial.println("NEXT: Press too short (bounce?) - ignored.");
    }
  }

  // --- State Machine Logic ---

  // Process Offline Configuration Mode if active.
  if (offlineConfigMode) {
    processOfflineConfigMode();  // Handles its own button presses internally now
    return;                      // Don't do other time updates while configuring
  }

  // Handle reset prompt timeout
  if (resetPromptActive && (millis() - resetPromptStart >= 5000)) {
    resetPromptActive = false;
    Serial.println("Reset prompt timed out. Canceling reset.");
    // Restore appropriate display
    if (offlineOperating) displayOfflineBin();
    else if (configMode) displayConfigInstructions(WiFi.localIP().toString());
    else updateDisplayToTimeFile();
  }

  // If operating in offline mode, update the bin file display only when minute changes.
  if (offlineOperating && !configMode && !offlineConfigMode) {
    unsigned long elapsed = (millis() - offlineLastUpdate) / 1000;
    unsigned long currentSec = offlineBaseTime + elapsed;
    int currentMinute = (currentSec / 60) % 60;

    // Use offlineLastDisplayedMinute for offline mode updates
    if (currentMinute != offlineLastDisplayedMinute) {
      offlineLastDisplayedMinute = currentMinute;
      Serial.printf("Offline time changed to minute %d. Updating display.\n", currentMinute);
      displayOfflineBin();  // This now uses the mapping
    }
  }

  // --- Waiting Period Check for HOME_KEY (QR Code Timeout) ---
  if (waitingForTimeCheck && (currentMillis - homePressTime >= 10000)) {
    waitingForTimeCheck = false;
    Serial.println("QR display timeout (10 seconds). Updating display to current time file.");
    if (offlineOperating) {
      Serial.println("Returning to offline time display");
      displayOfflineBin();  // Uses mapping
    } else if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Returning to online time display");
      updateDisplayToTimeFile();  // Uses mapping
    } else {
      Serial.println("Cannot return to time display (no time source).");
      // Optionally show an error/instruction screen
    }
  }

  // --- Automatic Time Update (Online Mode) ---
  // Only run if not waiting for QR, not offline, not in config, and WiFi connected
  if (!waitingForTimeCheck && !offlineOperating && !configMode && !offlineConfigMode && WiFi.status() == WL_CONNECTED) {
    if (getLocalTime(&timeinfo)) {  // Check if time sync is valid
      int currentMinute = timeinfo.tm_min;
      if (currentMinute != lastMinute) {
        Serial.printf("Online time changed from %d to %d. Updating display.\n", lastMinute, currentMinute);
        lastMinute = currentMinute;  // Update lastMinute *after* successful display potentially
        updateDisplayToTimeFile();   // This function now uses the mapping
      }
    } else if (lastMinute != -1) {  // Only print error if time was previously synced
      Serial.println("Failed to get local time for periodic update check.");
      // Consider trying to resync or indicate error on display?
      // Maybe set lastMinute = -1 to avoid repeated errors?
      // lastMinute = -1;
    }
  }

  // Serial Command Handling (Non-blocking)
  if (currentMillis - lastSerialCheck >= serialInterval) {
    lastSerialCheck = currentMillis;
    if (Serial.available() > 0) {
      String command = Serial.readStringUntil('\n');
      processSerialCommand(command);
    }
  }

  // No blocking delay at the end of loop.
  // yield(); // Optional: give RTOS scheduler time, useful if loops are very tight
}
