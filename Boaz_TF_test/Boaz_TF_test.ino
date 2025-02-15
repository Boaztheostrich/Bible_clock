#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include "EPD.h"          // E-Paper Display driver
#include "EPD_GUI.h"      // EPD GUI functions

// WiFi credentials
const char* ssid = "BURNETT";
const char* password = "21Burnett!";

// NTP configuration for Central Standard Time (UTC–6)
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -21600;   // -6 * 3600 = -21600 seconds for CST
const int daylightOffset_sec = 0;      // No daylight saving adjustment

// SD Card SPI pin definitions
#define SD_CS_PIN    10    // Chip Select (IO10)
#define SD_MOSI_PIN  40    // MOSI (IO40)
#define SD_MISO_PIN  13    // MISO (IO13)
#define SD_SCK_PIN   39    // Clock (IO39)

// Create a dedicated SPI instance for the SD card (HSPI)
SPIClass sdSPI(HSPI);

// Allocate an image buffer for the e-paper display
uint8_t Image_BW[15000];

// Global variable to track the last minute displayed
int lastMinute = -1;

// Clears the e-paper display and initializes the image buffer to BLACK (inverted clear).
void clear_all() {
  EPD_Clear();
  Paint_NewImage(Image_BW, EPD_W, EPD_H, 0, BLACK); // Initialize buffer with BLACK background
  EPD_Full(WHITE);                              // Fill display with BLACK initially
  EPD_Display_Part(0, 0, EPD_W, EPD_H, Image_BW);
}

// Loads the specified binary file from the SD card into Image_BW and updates the display.
void displayFile(const char* filename) {
  Serial.print("Attempting to open file: ");
  Serial.println(filename);

  File file = SD.open(filename, FILE_READ);
  if (!file) {
    Serial.print("ERROR: File not found: ");
    Serial.println(filename);
    return;
  }
  size_t bytesRead = file.read(Image_BW, sizeof(Image_BW));
  Serial.printf("Bytes read: %d\n", bytesRead);
  file.close();

  // Prepare and update the e-paper display.
  EPD_Init_Fast(Fast_Seconds_1_5s);
  EPD_Display_Fast(Image_BW);
  EPD_Sleep();
}

void setup() {
  Serial.begin(115200);
  Serial.println("Starting time-sync display demo...");

  // --- Connect to WiFi ---
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Connected! IP Address: ");
  Serial.println(WiFi.localIP());
  delay(2000); // Increased delay after WiFi

  // --- Synchronize time via NTP using Central Standard Time ---
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  Serial.print("Waiting for time synchronization");
  while (!getLocalTime(&timeinfo)) {
    delay(2000);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("Time set: ");
  Serial.println(&timeinfo, "%F %T");
  delay(2000); // Increased delay after NTP sync

  // --- Initialize SD Card ---
  // Turn on SD card power (if applicable)
  pinMode(42, OUTPUT);
  digitalWrite(42, HIGH);
  delay(10);
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN);
  if (!SD.begin(SD_CS_PIN, sdSPI, 80000000)) {
    Serial.println("ERROR: SD Mount Failed!");
    while (1) { delay(1000); }
  } else {
    uint64_t sdSizeMB = SD.cardSize() / (1024 * 1024);
    Serial.printf("SD Size: %lluMB\n", sdSizeMB);
    // clear_all(); //  Initially we could clear to white, but for inverted, maybe no clear here or clear to black
    delay(2000);
  }
  delay(2000); // Increased delay after SD init

  // --- Initialize the E-Paper Display for INVERTED colors ---
  pinMode(7, OUTPUT);
  digitalWrite(7, HIGH);
  EPD_GPIOInit();
  EPD_Clear();
  Paint_NewImage(Image_BW, EPD_W, EPD_H, 0, BLACK); // Initialize buffer with BLACK background
  EPD_Full(BLACK);                              // Fill display with BLACK initially
  EPD_Init_Fast(Fast_Seconds_1_5s);
  delay(2000); // Increased delay after EPD init

  // ---  Commented out Initial Display in Setup ---
  // if (getLocalTime(&timeinfo)) {
  //   lastMinute = timeinfo.tm_min;
  //   int hour24 = timeinfo.tm_hour;
  //   int minute = timeinfo.tm_min;
  //   // Convert 24-hour to 12-hour format (0 becomes 12)
  //   int hour12 = (hour24 % 12 == 0) ? 12 : (hour24 % 12);
  //   char filename[16];
  //   sprintf(filename, "/%d_%02d.bin", hour12, minute);
  //   Serial.print("Displaying initial file: ");
  //   Serial.println(filename);
  //   displayFile(filename);
  // }

  Serial.println("Setup complete. Waiting for serial commands to update display."); // Indicate setup completion
}

void loop() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    int currentMinute = timeinfo.tm_min;
   // --- Commented out Automatic Update Logic ---
   // Check every second: if the minute has changed, update the display.
    if (currentMinute != lastMinute) {
      lastMinute = currentMinute;
      int hour24 = timeinfo.tm_hour;
      int minute = timeinfo.tm_min;
      int hour12 = (hour24 % 12 == 0) ? 12 : (hour24 % 12);
      char filename[16];
      sprintf(filename, "/%d_%02d.bin", hour12, minute);
      Serial.print("Time changed. Updating display with file: ");
      Serial.println(filename);
      displayFile(filename);
    }
  }

  // Check if user typed a file name in the Serial Monitor to force display.
  if (Serial.available() > 0) {
    String fileName = Serial.readStringUntil('\n');
    fileName.trim();  // Remove any leading/trailing whitespace.
    if (fileName.length() > 0) {
      // Ensure the file name starts with a '/'
      if (!fileName.startsWith("/")) {
        fileName = "/" + fileName;
      }
      Serial.print("Forced display of file via Serial Command: ");
      Serial.println(fileName);
      displayFile(fileName.c_str());
    }
  }
  delay(500); // Increased delay to 10 seconds (or even higher for longer testing if needed)
}