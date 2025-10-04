// Cloud Attendance System (C A S)
#include <ESP8266WiFi.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>
#include <WiFiManager.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <SPI.h>
#include <MFRC522.h>
#include <WiFiUdp.h>
#include <NTPClient.h>

// Google Apps Script Web App URL
const char* script_url = "https://script.google.com/macros/s/AKfycbwRfXjV5J34Jplf4Yki46kE8EZ8vHTGBZTaCpiGOrwSywSSIU76IICP1Iw9q0NJ27zo/exec";

// Secure HTTPS client
WiFiClientSecure client;

// NTP Client for IST (GMT+5:30)
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 19800, 60000);

// RFID
#define RST_PIN D3
#define SS_PIN  D4
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Indicator Pins
#define BUZZER_PIN D2
#define GREEN_LED_PIN D6
#define RED_LED_PIN D7

// Scan debounce
unsigned long lastScanTime = 0;
String lastScannedUID = "";

// State handling
bool scanInProgress = false;
unsigned long scanStartTime = 0;
int scannedStudentIndex = -1;
bool awaitingPost = false;
bool posted = false;
bool isEntry = true;

// Student structure
struct Student {
  String rfid;
  String id;
  String name;
  String branch;
  String time_in;
  String time_out;
  bool isIn;
};

// Your students
Student students[5] = {
  { "0A21FA1C", "24IUT0010217", "Rohan Sen", "ECE", "", "", false },
  { "106DAF30", "24UIT0010218", "Debajyoti Kar", "ECE", "", "", false },
  { "C3D4E5F6", "003", "Amit Patel", "EEE", "", "", false },
  { "D4E5F6G7", "004", "Sneha Roy", "MECH", "", "", false },
  { "E5F6G7H8", "005", "Vikas Gupta", "CIVIL", "", "", false }
};

// Get UID as string
String getUID(MFRC522::Uid uid) {
  String s = "";
  for (byte i = 0; i < uid.size; i++) {
    char buffer[3];
    sprintf(buffer, "%02X", uid.uidByte[i]);
    s += buffer;
  }
  return s;
}

// Get date string
String getDate() {
  unsigned long epochTime = timeClient.getEpochTime();
  time_t rawTime = (time_t)epochTime;
  struct tm *timeinfo = gmtime(&rawTime);
  char buf[15];
  sprintf(buf, "%02d-%02d-%04d", timeinfo->tm_mday, timeinfo->tm_mon + 1, timeinfo->tm_year + 1900);
  return String(buf);
}

// Get time string
String getTime() {
  return timeClient.getFormattedTime();
}

// Send attendance to Google Sheet
void postToGoogleSheet(String date, String id, String name, String branch, String time_in, String time_out) {
  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;
    http.begin(client, script_url);
    http.addHeader("Content-Type", "application/json");

    String jsonPayload = "{\"date\":\"" + date + "\",\"id\":\"" + id + "\",\"name\":\"" + name + "\",\"branch\":\"" + branch + "\"";
    if (time_out != "") {
      jsonPayload += ",\"time_out\":\"" + time_out + "\"";
    } else {
      jsonPayload += ",\"time_in\":\"" + time_in + "\"";
    }
    jsonPayload += "}";

    Serial.println("Sending JSON:");
    Serial.println(jsonPayload);

    int httpResponseCode = http.POST(jsonPayload);

    if (httpResponseCode > 0) {
      String response = http.getString();
      Serial.printf("POST success: %d, response: %s\n", httpResponseCode, response.c_str());
    } else {
      Serial.printf("POST failed, error: %s\n", http.errorToString(httpResponseCode).c_str());
    }

    http.end();
  } else {
    Serial.println("WiFi not connected");
  }
}

void setup() {
  Serial.begin(115200);
  delay(10);

  // Buzzer & LEDs
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, HIGH);

  // WiFi Manager
  WiFiManager wifiManager;
  // Uncomment to clear previous WiFi
  // wifiManager.resetSettings();

  if (!wifiManager.autoConnect("AutoConnectAP")) {
    Serial.println("Failed to connect, restarting...");
    ESP.restart();
  }

  Serial.println("Connected to WiFi!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());

  client.setInsecure();

  // NTP
  timeClient.begin();
  while (!timeClient.update()) {
    timeClient.forceUpdate();
  }

  // RFID
  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("Scan your RFID card:");
}

void loop() {
  // Handle non-blocking feedback timer
  if (scanInProgress) {
    if (millis() - scanStartTime >= 2000) {
      digitalWrite(BUZZER_PIN, LOW);
      digitalWrite(GREEN_LED_PIN, LOW);
      scanInProgress = false;
      awaitingPost = true;
    }
    return;
  }

  // Post data after scan feedback
  if (awaitingPost && scannedStudentIndex != -1 && !posted) {
    String date = getDate();
    String timeNow = getTime();
    Student &s = students[scannedStudentIndex];

    if (isEntry) {
      postToGoogleSheet(date, s.id, s.name, s.branch, s.time_in, "");
    } else {
      postToGoogleSheet(date, s.id, s.name, s.branch, "", s.time_out);
    }

    posted = true;
    scannedStudentIndex = -1;
    return;
  }

  // Check for new RFID card
  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) return;

  String currentUID = getUID(mfrc522.uid);
  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  if (currentUID == lastScannedUID && millis() - lastScanTime < 3000) return;
  lastScannedUID = currentUID;
  lastScanTime = millis();

  Serial.println("Card scanned UID: " + currentUID);

  // Match UID
  int idx;
  for (idx = 0; idx < 5; idx++) {
    if (students[idx].rfid == currentUID) break;
  }

  if (idx == 5) {
    Serial.println("Unknown RFID card scanned");
    return;
  }

  if (!timeClient.update()) {
    timeClient.forceUpdate();
  }

  String date = getDate();
  String timeNow = getTime();

  Student &s = students[idx];
  scannedStudentIndex = idx;
  posted = false;

  if (!s.isIn) {
    s.time_in = timeNow;
    s.time_out = "";
    s.isIn = true;
    isEntry = true;
    Serial.printf("IN  | %s | %s | %s | %s | IN: %s\n", date.c_str(), s.id.c_str(), s.name.c_str(), s.branch.c_str(), timeNow.c_str());
  } else {
    s.time_out = timeNow;
    s.isIn = false;
    isEntry = false;
    Serial.printf("OUT | %s | %s | %s | %s | OUT: %s\n", date.c_str(), s.id.c_str(), s.name.c_str(), s.branch.c_str(), timeNow.c_str());
  }

  // Start 2-second feedback (non-blocking)
  digitalWrite(BUZZER_PIN, HIGH);
  digitalWrite(GREEN_LED_PIN, HIGH);
  scanStartTime = millis();
  scanInProgress = true;
}

