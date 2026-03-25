/*
 * ESP32-2432S024 + LVGL 9.2.0 + EEZ Studio
 * ARCHITECTURE: FreeRTOS Multi-Task Design
 * WiFi, Access Point, NTP, IP Setting - Core Logic Only
 */

#define PRODUCTION 1

#include "Wire.h"
#include "RTClib.h"
#include "LittleFS.h"
#include "WiFi.h"
#include "ESPAsyncWebServer.h"
#include "TimeLib.h"
#include "time.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"

// ================================
// DEFINISI PIN
// ================================

#define RGB_R_PIN      4
#define RGB_G_PIN      16
#define RGB_B_PIN      17

// ================================
// KONFIGURASI RTOS
// ================================
#define WIFI_TASK_STACK_SIZE  3072
#define NTP_TASK_STACK_SIZE   4096
#define WEB_TASK_STACK_SIZE   4096
#define RTC_TASK_STACK_SIZE   2048
#define CLOCK_TASK_STACK_SIZE 2048

#define WIFI_TASK_PRIORITY  2
#define NTP_TASK_PRIORITY   2
#define WEB_TASK_PRIORITY   1
#define RTC_TASK_PRIORITY   1
#define CLOCK_TASK_PRIORITY 2

TaskHandle_t rtcTaskHandle  = NULL;
TaskHandle_t wifiTaskHandle = NULL;
TaskHandle_t ntpTaskHandle  = NULL;
TaskHandle_t webTaskHandle  = NULL;

// ================================
// SEMAPHORES & MUTEXES
// ================================
SemaphoreHandle_t timeMutex;
SemaphoreHandle_t wifiMutex;
SemaphoreHandle_t settingsMutex;
SemaphoreHandle_t i2cMutex;

// ================================
// OBJEK GLOBAL
// ================================

RTC_DS3231 rtc;
bool rtcAvailable = false;

// ================================
// KONFIGURASI AP DEFAULT
// ================================
String DEFAULT_AP_SSID = "WifiManager-" + String(ESP.getEfuseMac(), HEX);
#define DEFAULT_AP_PASSWORD "12345678"
String hostname = "WifiManager-" + String(ESP.getEfuseMac(), HEX);

// ================================
// GRUP EVENT WIFI
// ================================
EventGroupHandle_t wifiEventGroup;

#define WIFI_CONNECTED_BIT    BIT0
#define WIFI_DISCONNECTED_BIT BIT1
#define WIFI_GOT_IP_BIT       BIT2

// ================================
// SERVER NTP
// ================================
const char *ntpServers[] = {
  "pool.ntp.org",
  "time.google.com",
  "time.windows.com"
};
const int NTP_SERVER_COUNT = 3;

// ================================
// STRUKTUR KONFIGURASI
// ================================
struct WiFiConfig {
  char apSSID[33];
  char apPassword[65];
  String routerSSID;
  String routerPassword;
  bool isConnected;
  IPAddress localIP;
  IPAddress apIP;
  IPAddress apGateway;
  IPAddress apSubnet;
};

struct TimeConfig {
  time_t currentTime;
  bool ntpSynced;
  unsigned long lastNTPUpdate;
  String ntpServer;
};

struct CountdownState {
  bool isActive;
  unsigned long startTime;
  int totalSeconds;
  String message;
  String reason;
};

CountdownState countdownState = {false, 0, 0, "", ""};
SemaphoreHandle_t countdownMutex = NULL;

WiFiConfig wifiConfig;
TimeConfig timeConfig;
int timezoneOffset = 7;

int reconnectAttempts = 0;
const int MAX_RECONNECT_ATTEMPTS = 5;
unsigned long wifiFailedTime = 0;
int wifiRetryCount = 0;
unsigned long wifiDisconnectedTime = 0;

// ================================
// WEB SERVER
// ================================
AsyncWebServer server(80);

// ================================
// STATUS
// ================================
enum WiFiState { WIFI_IDLE, WIFI_CONNECTING, WIFI_CONNECTED, WIFI_FAILED };
volatile WiFiState wifiState = WIFI_IDLE;
volatile bool ntpSyncInProgress = false;
volatile bool ntpSyncCompleted = false;

TaskHandle_t restartTaskHandle = NULL;
TaskHandle_t resetTaskHandle = NULL;

static SemaphoreHandle_t wifiRestartMutex = NULL;
static bool wifiRestartInProgress = false;
static bool apRestartInProgress = false;

static unsigned long lastWiFiRestartRequest = 0;
static unsigned long lastAPRestartRequest = 0;
const unsigned long RESTART_DEBOUNCE_MS = 3000;

// ================================
// RGB LED
// ================================
static bool rgbBootBlinking = true;
static bool internetAvailable = false;
static unsigned long rgbLastToggle = 0;
static bool rgbLedState = false;

void rgbSetColor(bool r, bool g, bool b) {
  digitalWrite(RGB_R_PIN, r ? LOW : HIGH);
  digitalWrite(RGB_G_PIN, g ? LOW : HIGH);
  digitalWrite(RGB_B_PIN, b ? LOW : HIGH);
}

void rgbOff() {
  digitalWrite(RGB_R_PIN, HIGH);
  digitalWrite(RGB_G_PIN, HIGH);
  digitalWrite(RGB_B_PIN, HIGH);
}

void rgbBootBlinkTask(void *parameter) {
  while (rgbBootBlinking) {
    rgbSetColor(true, false, false);
    vTaskDelay(pdMS_TO_TICKS(300));
    rgbOff();
    vTaskDelay(pdMS_TO_TICKS(300));
  }
  rgbOff();
  vTaskDelete(NULL);
}

void handleRGBLed() {
  unsigned long now = millis();

  if (countdownState.isActive &&
      (countdownState.reason == "device_restart" || countdownState.reason == "factory_reset")) {
    if (now - rgbLastToggle >= 500) {
      rgbLastToggle = now;
      rgbLedState = !rgbLedState;
      rgbSetColor(rgbLedState, false, false);
    }
    return;
  }

  if (wifiConfig.routerSSID.length() > 0) {
    switch (wifiState) {
      case WIFI_CONNECTED:
        if (internetAvailable) {
          rgbSetColor(false, true, false);
        } else {
          if (now - rgbLastToggle >= 500) {
            rgbLastToggle = now;
            rgbLedState = !rgbLedState;
            rgbSetColor(false, rgbLedState, false);
          }
        }
        break;
      case WIFI_CONNECTING:
        if (now - rgbLastToggle >= 300) {
          rgbLastToggle = now;
          rgbLedState = !rgbLedState;
          rgbSetColor(false, rgbLedState, false);
        }
        break;
      case WIFI_FAILED:
        rgbSetColor(true, false, false);
        break;
      case WIFI_IDLE:
      default:
        rgbOff();
        break;
    }
    return;
  }
  rgbOff();
}

void rgbBootDone() {
  rgbBootBlinking = false;
  vTaskDelay(pdMS_TO_TICKS(100));
  rgbOff();
}

// ================================
// FORWARD DECLARATIONS
// ================================
void startCountdown(String reason, String message, int seconds);
void stopCountdown();
int getRemainingSeconds();
void saveWiFiCredentials();
void loadWiFiCredentials();
void saveAPCredentials();
void setupWiFiEvents();
void saveTimezoneConfig();
void loadTimezoneConfig();
bool initRTC();
bool isRTCValid();
bool isRTCTimeValid(DateTime dt);
void saveTimeToRTC();
void setupServerRoutes();
void sendJSONResponse(AsyncWebServerRequest *request, const String &json);
bool init_littlefs();
void createDefaultConfigFiles();
void printStackReport();
void wifiTask(void *parameter);
void ntpTask(void *parameter);
void webTask(void *parameter);
void rtcSyncTask(void *parameter);
void clockTickTask(void *parameter);
void restartWiFiTask(void *parameter);
void restartAPTask(void *parameter);
void internetCheckTask(void *parameter);
int asyncScanNetworks();
void connectToBestAP();

// ============================================
// COUNTDOWN
// ============================================
void startCountdown(String reason, String message, int seconds) {
  if (countdownMutex == NULL) countdownMutex = xSemaphoreCreateMutex();
  if (xSemaphoreTake(countdownMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    countdownState.isActive = true;
    countdownState.startTime = millis();
    countdownState.totalSeconds = seconds;
    countdownState.message = message;
    countdownState.reason = reason;
    xSemaphoreGive(countdownMutex);
  }
}

void stopCountdown() {
  if (countdownMutex != NULL && xSemaphoreTake(countdownMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    countdownState.isActive = false;
    countdownState.reason = "";
    xSemaphoreGive(countdownMutex);
  }
}

int getRemainingSeconds() {
  if (!countdownState.isActive) return 0;
  unsigned long elapsed = (millis() - countdownState.startTime) / 1000;
  int remaining = countdownState.totalSeconds - elapsed;
  if (remaining <= 0) { stopCountdown(); return 0; }
  return remaining;
}

// ============================================
// WIFI CONFIG
// ============================================
void saveWiFiCredentials() {
  if (xSemaphoreTake(settingsMutex, portMAX_DELAY) == pdTRUE) {
    fs::File file = LittleFS.open("/wifi_creds.txt", "w");
    if (file) {
      file.println(wifiConfig.routerSSID);
      file.println(wifiConfig.routerPassword);
      file.flush();
      file.close();
      Serial.println("KREDENSIAL WIFI TERSIMPAN");
    }
    xSemaphoreGive(settingsMutex);
  }
}

void loadWiFiCredentials() {
  if (xSemaphoreTake(settingsMutex, portMAX_DELAY) == pdTRUE) {
    if (LittleFS.exists("/wifi_creds.txt")) {
      fs::File file = LittleFS.open("/wifi_creds.txt", "r");
      if (file) {
        wifiConfig.routerSSID = file.readStringUntil('\n');
        wifiConfig.routerPassword = file.readStringUntil('\n');
        wifiConfig.routerSSID.trim();
        wifiConfig.routerPassword.trim();
        file.close();
        Serial.println("KREDENSIAL WIFI DIMUAT");
      }
    }
    if (LittleFS.exists("/ap_creds.txt")) {
      fs::File file = LittleFS.open("/ap_creds.txt", "r");
      if (file) {
        String ssid = file.readStringUntil('\n');
        String pass = file.readStringUntil('\n');
        ssid.trim(); pass.trim();
        ssid.toCharArray(wifiConfig.apSSID, 33);
        pass.toCharArray(wifiConfig.apPassword, 65);

        if (file.available()) {
          String ipStr = file.readStringUntil('\n'); ipStr.trim();
          wifiConfig.apIP.fromString(ipStr);
        } else { wifiConfig.apIP = IPAddress(192, 168, 100, 1); }

        if (file.available()) {
          String gwStr = file.readStringUntil('\n'); gwStr.trim();
          wifiConfig.apGateway.fromString(gwStr);
        } else { wifiConfig.apGateway = IPAddress(192, 168, 100, 1); }

        if (file.available()) {
          String snStr = file.readStringUntil('\n'); snStr.trim();
          wifiConfig.apSubnet.fromString(snStr);
        } else { wifiConfig.apSubnet = IPAddress(255, 255, 255, 0); }

        file.close();
        Serial.println("KONFIGURASI AP DIMUAT: " + String(wifiConfig.apSSID));
      }
    } else {
      DEFAULT_AP_SSID.toCharArray(wifiConfig.apSSID, 33);
      strcpy(wifiConfig.apPassword, DEFAULT_AP_PASSWORD);
      wifiConfig.apIP = IPAddress(192, 168, 100, 1);
      wifiConfig.apGateway = IPAddress(192, 168, 100, 1);
      wifiConfig.apSubnet = IPAddress(255, 255, 255, 0);
    }
    xSemaphoreGive(settingsMutex);
  }
}

void saveAPCredentials() {
  if (xSemaphoreTake(settingsMutex, portMAX_DELAY) == pdTRUE) {
    fs::File file = LittleFS.open("/ap_creds.txt", "w");
    if (file) {
      file.println(wifiConfig.apSSID);
      file.println(wifiConfig.apPassword);
      file.println(wifiConfig.apIP.toString());
      file.println(wifiConfig.apGateway.toString());
      file.println(wifiConfig.apSubnet.toString());
      file.flush();
      file.close();
      Serial.println("KREDENSIAL AP TERSIMPAN");
    }
    xSemaphoreGive(settingsMutex);
  }
}

void setupWiFiEvents() {
    wifiEventGroup = xEventGroupCreate();

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info) {
        if (wifiRestartInProgress || apRestartInProgress) return;

        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_CONNECTED:
                xEventGroupSetBits(wifiEventGroup, WIFI_CONNECTED_BIT);
                break;

            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                Serial.println("WIFI TERHUBUNG - IP: " + WiFi.localIP().toString());
                xEventGroupSetBits(wifiEventGroup, WIFI_GOT_IP_BIT);
                if (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    wifiConfig.isConnected = true;
                    wifiConfig.localIP = WiFi.localIP();
                    wifiState = WIFI_CONNECTED;
                    reconnectAttempts = 0;
                    wifiRetryCount = 0;
                    xSemaphoreGive(wifiMutex);
                }
                if (ntpTaskHandle != NULL) {
                    ntpSyncInProgress = false;
                    ntpSyncCompleted = false;
                    xTaskNotifyGive(ntpTaskHandle);
                }
                break;

            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
                Serial.println("WIFI TERPUTUS (kode: " + String(info.wifi_sta_disconnected.reason) + ")");
                xEventGroupClearBits(wifiEventGroup, WIFI_CONNECTED_BIT | WIFI_GOT_IP_BIT);
                xEventGroupSetBits(wifiEventGroup, WIFI_DISCONNECTED_BIT);
                wifiDisconnectedTime = millis();
                if (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    wifiConfig.isConnected = false;
                    wifiState = WIFI_IDLE;
                    xSemaphoreGive(wifiMutex);
                }
                break;

            case ARDUINO_EVENT_WIFI_AP_START:
                Serial.println("AP DIMULAI: " + WiFi.softAPIP().toString());
                break;
        }
    });
}

// ============================================
// TIMEZONE
// ============================================
void saveTimezoneConfig() {
  if (xSemaphoreTake(settingsMutex, portMAX_DELAY) == pdTRUE) {
    fs::File file = LittleFS.open("/timezone.txt", "w");
    if (file) {
      file.println(timezoneOffset);
      file.flush(); file.close();
    }
    xSemaphoreGive(settingsMutex);
  }
  vTaskDelay(pdMS_TO_TICKS(50));
}

void loadTimezoneConfig() {
  if (xSemaphoreTake(settingsMutex, portMAX_DELAY) == pdTRUE) {
    if (LittleFS.exists("/timezone.txt")) {
      fs::File file = LittleFS.open("/timezone.txt", "r");
      if (file) {
        String offsetStr = file.readStringUntil('\n');
        offsetStr.trim();
        timezoneOffset = offsetStr.toInt();
        if (timezoneOffset < -12 || timezoneOffset > 14) timezoneOffset = 7;
        file.close();
      }
    } else {
      timezoneOffset = 7;
    }
    Serial.println("TIMEZONE: UTC" + String(timezoneOffset >= 0 ? "+" : "") + String(timezoneOffset));
    xSemaphoreGive(settingsMutex);
  }
}

// ============================================
// RTC
// ============================================
bool initRTC() {
    if (!rtc.begin()) {
        Serial.println("RTC TIDAK DITEMUKAN - BERJALAN TANPA RTC");
        if (xSemaphoreTake(timeMutex, portMAX_DELAY) == pdTRUE) {
            const time_t EPOCH_2000 = 946684800;
            setTime(0, 0, 0, 1, 1, 2000);
            timeConfig.currentTime = EPOCH_2000;
            xSemaphoreGive(timeMutex);
        }
        return false;
    }

    DateTime test = rtc.now();
    bool isValid = (test.year() >= 2000 && test.year() <= 2100 &&
                    test.month() >= 1 && test.month() <= 12 &&
                    test.day() >= 1 && test.day() <= 31);

    if (!isValid) {
        Serial.println("RTC HARDWARE RUSAK - BERJALAN TANPA RTC");
        if (xSemaphoreTake(timeMutex, portMAX_DELAY) == pdTRUE) {
            const time_t EPOCH_2000 = 946684800;
            setTime(0, 0, 0, 1, 1, 2000);
            timeConfig.currentTime = EPOCH_2000;
            xSemaphoreGive(timeMutex);
        }
        return false;
    }

    if (rtc.lostPower()) {
        Serial.println("RTC KEHILANGAN DAYA - WAKTU AKAN DIRESET");
        rtc.adjust(DateTime(2000, 1, 1, 0, 0, 0));
    }

    if (xSemaphoreTake(timeMutex, portMAX_DELAY) == pdTRUE) {
        setTime(test.hour(), test.minute(), test.second(),
               test.day(), test.month(), test.year());
        timeConfig.currentTime = now();
        xSemaphoreGive(timeMutex);
    }

    Serial.printf("RTC OK: %02d:%02d:%02d %02d/%02d/%04d\n",
                 test.hour(), test.minute(), test.second(),
                 test.day(), test.month(), test.year());
    return true;
}

bool isRTCTimeValid(DateTime dt) {
    return (dt.year() >= 2000 && dt.year() <= 2100 &&
            dt.month() >= 1 && dt.month() <= 12 &&
            dt.day() >= 1 && dt.day() <= 31 &&
            dt.hour() <= 23 && dt.minute() <= 59 && dt.second() <= 59);
}

bool isRTCValid() {
    if (!rtcAvailable) return false;
    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (rtc.lostPower()) { xSemaphoreGive(i2cMutex); return false; }
        DateTime rtcNow = rtc.now();
        xSemaphoreGive(i2cMutex);
        return isRTCTimeValid(rtcNow);
    }
    return false;
}

void saveTimeToRTC() {
    if (!rtcAvailable) return;
    time_t currentTime;
    if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        currentTime = timeConfig.currentTime;
        xSemaphoreGive(timeMutex);
    } else return;
    if (currentTime < 946684800) return;

    uint16_t y = year(currentTime); uint8_t m = month(currentTime);
    uint8_t d = day(currentTime);   uint8_t h = hour(currentTime);
    uint8_t mn = minute(currentTime); uint8_t s = second(currentTime);

    if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
        rtc.adjust(DateTime(y, m, d, h, mn, s));
        delay(100);
        xSemaphoreGive(i2cMutex);
    }
}

// ============================================
// JSON RESPONSE HELPER
// ============================================
void sendJSONResponse(AsyncWebServerRequest *request, const String &json) {
    AsyncWebServerResponse *resp = request->beginResponse(200, "application/json", json);
    resp->addHeader("Content-Length", String(json.length()));
    resp->addHeader("Connection", "keep-alive");
    resp->addHeader("Cache-Control", "no-cache");
    request->send(resp);
}

// ============================================
// WEB SERVER ROUTES
// ============================================
void setupServerRoutes() {
  // Halaman utama
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!LittleFS.exists("/index.html")) { request->send(404, "text/plain", "index.html not found"); return; }
    AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/index.html", "text/html");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    response->addHeader("Content-Type", "text/html; charset=utf-8");
    request->send(response);
  });

  server.on("/css/foundation.min.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!LittleFS.exists("/css/foundation.min.css")) { request->send(404, "text/plain", "CSS not found"); return; }
    AsyncWebServerResponse *response = request->beginResponse(LittleFS, "/css/foundation.min.css", "text/css");
    response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    request->send(response);
  });

  // Status perangkat
  server.on("/devicestatus", HTTP_GET, [](AsyncWebServerRequest *request) {
    char timeStr[20], dateStr[20];
    time_t now_t;
    struct tm timeinfo;

    if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      now_t = timeConfig.currentTime; xSemaphoreGive(timeMutex);
    } else { time(&now_t); }

    localtime_r(&now_t, &timeinfo);
    sprintf(timeStr, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    sprintf(dateStr, "%02d/%02d/%04d", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);

    bool isWiFiConnected = (WiFi.status() == WL_CONNECTED && wifiConfig.isConnected);
    String wifiStateStr;
    switch (wifiState) {
      case WIFI_IDLE:       wifiStateStr = "idle"; break;
      case WIFI_CONNECTING: wifiStateStr = "connecting"; break;
      case WIFI_CONNECTED:  wifiStateStr = "connected"; break;
      case WIFI_FAILED:     wifiStateStr = "failed"; break;
      default:              wifiStateStr = "unknown"; break;
    }

    char jsonBuffer[512];
    snprintf(jsonBuffer, sizeof(jsonBuffer),
      "{\"connected\":%s,\"wifiState\":\"%s\",\"ssid\":\"%s\","
      "\"ip\":\"%s\",\"rssi\":%d,\"ntpSynced\":%s,\"ntpServer\":\"%s\","
      "\"currentTime\":\"%s\",\"currentDate\":\"%s\",\"uptime\":%lu,\"freeHeap\":%d}",
      isWiFiConnected ? "true" : "false",
      wifiStateStr.c_str(),
      isWiFiConnected ? WiFi.SSID().c_str() : "",
      isWiFiConnected ? wifiConfig.localIP.toString().c_str() : "0.0.0.0",
      isWiFiConnected ? WiFi.RSSI() : 0,
      timeConfig.ntpSynced ? "true" : "false",
      timeConfig.ntpServer.c_str(),
      timeStr, dateStr,
      millis() / 1000,
      ESP.getFreeHeap()
    );
    sendJSONResponse(request, String(jsonBuffer));
  });

  // Restart perangkat
  server.on("/restart", HTTP_POST, [](AsyncWebServerRequest *request) {
      if (restartTaskHandle != NULL) { vTaskDelete(restartTaskHandle); restartTaskHandle = NULL; }
      startCountdown("device_restart", "Memulai Ulang Perangkat", 60);
      request->send(200, "text/plain", "OK");
      xTaskCreate([](void* param) {
          for (int i = 60; i > 0; i--) {
              if (i == 35) {
                  WiFi.mode(WIFI_OFF); vTaskDelay(pdMS_TO_TICKS(500));
                  if (ntpTaskHandle != NULL) vTaskSuspend(ntpTaskHandle);
                  if (wifiTaskHandle != NULL) vTaskSuspend(wifiTaskHandle);
                  vTaskDelay(pdMS_TO_TICKS(1000));
                  server.end(); vTaskDelay(pdMS_TO_TICKS(500));
                  if (rtcAvailable) saveTimeToRTC();
              }
              vTaskDelay(pdMS_TO_TICKS(1000));
          }
          ESP.restart();
      }, "DeviceRestartTask", 5120, NULL, 1, &restartTaskHandle);
  });

  // Konfigurasi WiFi
  server.on("/getwificonfig", HTTP_GET, [](AsyncWebServerRequest *request) {
    char routerSSID[64] = "", routerPassword[64] = "";
    if (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      strncpy(routerSSID,     wifiConfig.routerSSID.c_str(),     sizeof(routerSSID) - 1);
      strncpy(routerPassword, wifiConfig.routerPassword.c_str(), sizeof(routerPassword) - 1);
      xSemaphoreGive(wifiMutex);
    }
    String currentAPSSID = WiFi.softAPSSID();
    if (currentAPSSID.length() == 0) currentAPSSID = String(wifiConfig.apSSID);
    if (currentAPSSID.length() == 0) currentAPSSID = DEFAULT_AP_SSID;
    String apPassword = String(wifiConfig.apPassword);
    if (apPassword.length() == 0) apPassword = DEFAULT_AP_PASSWORD;

    char buf[512];
    snprintf(buf, sizeof(buf),
      "{\"routerSSID\":\"%s\",\"routerPassword\":\"%s\","
      "\"apSSID\":\"%s\",\"apPassword\":\"%s\","
      "\"apIP\":\"%s\",\"apGateway\":\"%s\",\"apSubnet\":\"%s\"}",
      routerSSID, routerPassword,
      currentAPSSID.c_str(), apPassword.c_str(),
      wifiConfig.apIP.toString().c_str(),
      wifiConfig.apGateway.toString().c_str(),
      wifiConfig.apSubnet.toString().c_str()
    );
    sendJSONResponse(request, String(buf));
  });

  server.on("/setwifi", HTTP_POST, [](AsyncWebServerRequest *request) {
    unsigned long now = millis();
    if (now - lastWiFiRestartRequest < RESTART_DEBOUNCE_MS) {
      request->send(429, "text/plain", "Please wait before retrying");
      return;
    }
    if (request->hasParam("ssid", true) && request->hasParam("password", true)) {
      if (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        wifiConfig.routerSSID = request->getParam("ssid", true)->value();
        wifiConfig.routerPassword = request->getParam("password", true)->value();
        xSemaphoreGive(wifiMutex);
      }
      saveWiFiCredentials();
      request->send(200, "text/plain", "OK");
      xTaskCreate(restartWiFiTask, "WiFiRestart", 5120, NULL, 1, NULL);
    } else {
      request->send(400, "text/plain", "Missing parameters");
    }
  });

  // Konfigurasi Access Point
  server.on("/setap", HTTP_POST, [](AsyncWebServerRequest *request) {
      unsigned long now = millis();
      if (now - lastAPRestartRequest < RESTART_DEBOUNCE_MS) {
          request->send(429, "text/plain", "Please wait");
          return;
      }

      bool updateNetworkConfig = false;
      if (request->hasParam("updateNetworkConfig", true)) {
          updateNetworkConfig = (request->getParam("updateNetworkConfig", true)->value() == "true");
      }

      if (updateNetworkConfig) {
          if (request->hasParam("apIP", true)) {
              String ipStr = request->getParam("apIP", true)->value(); ipStr.trim();
              IPAddress tempIP;
              if (tempIP.fromString(ipStr)) wifiConfig.apIP = tempIP;
          }
          if (request->hasParam("gateway", true)) {
              String gwStr = request->getParam("gateway", true)->value(); gwStr.trim();
              IPAddress tempGW;
              if (tempGW.fromString(gwStr)) wifiConfig.apGateway = tempGW;
          }
          if (request->hasParam("subnet", true)) {
              String snStr = request->getParam("subnet", true)->value(); snStr.trim();
              IPAddress tempSN;
              if (tempSN.fromString(snStr)) wifiConfig.apSubnet = tempSN;
          }
          saveAPCredentials();
      } else {
          if (!request->hasParam("ssid", true) || !request->hasParam("password", true)) {
              request->send(400, "text/plain", "Missing ssid or password"); return;
          }
          String newSSID = request->getParam("ssid", true)->value(); newSSID.trim();
          String newPass = request->getParam("password", true)->value(); newPass.trim();
          if (newSSID.length() == 0) { request->send(400, "text/plain", "SSID cannot be empty"); return; }
          if (newPass.length() > 0 && newPass.length() < 8) { request->send(400, "text/plain", "Password min 8 chars"); return; }
          newSSID.toCharArray(wifiConfig.apSSID, 33);
          newPass.toCharArray(wifiConfig.apPassword, 65);
          saveAPCredentials();
      }

      IPAddress clientIP = request->client()->remoteIP();
      IPAddress apIP = WiFi.softAPIP();
      IPAddress apSubnet = WiFi.softAPSubnetMask();
      IPAddress apNetwork(apIP[0]&apSubnet[0], apIP[1]&apSubnet[1], apIP[2]&apSubnet[2], apIP[3]&apSubnet[3]);
      IPAddress clientNetwork(clientIP[0]&apSubnet[0], clientIP[1]&apSubnet[1], clientIP[2]&apSubnet[2], clientIP[3]&apSubnet[3]);

      if (apNetwork == clientNetwork) {
          startCountdown("ap_restart", "Memulai Ulang Access Point", 60);
      }
      request->send(200, "text/plain", "OK");
      xTaskCreate(restartAPTask, "APRestart", 5120, NULL, 1, NULL);
  });

  // Tipe koneksi
  server.on("/api/connection-type", HTTP_GET, [](AsyncWebServerRequest *request) {
    IPAddress clientIP = request->client()->remoteIP();
    IPAddress apIP = WiFi.softAPIP();
    IPAddress apSubnet = WiFi.softAPSubnetMask();
    IPAddress apNetwork(apIP[0]&apSubnet[0], apIP[1]&apSubnet[1], apIP[2]&apSubnet[2], apIP[3]&apSubnet[3]);
    IPAddress clientNetwork(clientIP[0]&apSubnet[0], clientIP[1]&apSubnet[1], clientIP[2]&apSubnet[2], clientIP[3]&apSubnet[3]);
    bool isLocalAP = (apNetwork == clientNetwork);

    char buf[192];
    snprintf(buf, sizeof(buf),
      "{\"isLocalAP\":%s,\"clientIP\":\"%s\",\"apIP\":\"%s\",\"apSubnet\":\"%s\"}",
      isLocalAP ? "true" : "false",
      clientIP.toString().c_str(),
      apIP.toString().c_str(),
      apSubnet.toString().c_str()
    );
    sendJSONResponse(request, String(buf));
  });

  // Sinkronisasi waktu manual
  server.on("/synctime", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("y",true) && request->hasParam("m",true) &&
        request->hasParam("d",true) && request->hasParam("h",true) &&
        request->hasParam("i",true) && request->hasParam("s",true)) {

      int y = request->getParam("y",true)->value().toInt();
      int m = request->getParam("m",true)->value().toInt();
      int d = request->getParam("d",true)->value().toInt();
      int h = request->getParam("h",true)->value().toInt();
      int i = request->getParam("i",true)->value().toInt();
      int s = request->getParam("s",true)->value().toInt();

      if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        setTime(h, i, s, d, m, y);
        timeConfig.currentTime = now();
        timeConfig.ntpSynced = true;
        xSemaphoreGive(timeMutex);
      }
      if (rtcAvailable) saveTimeToRTC();
      request->send(200, "text/plain", "Waktu berhasil di-sync!");
    } else {
      request->send(400, "text/plain", "Data waktu tidak lengkap");
    }
  });

  // Timezone
  server.on("/gettimezone", HTTP_GET, [](AsyncWebServerRequest *request) {
    char buf[32];
    if (xSemaphoreTake(settingsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      snprintf(buf, sizeof(buf), "{\"offset\":%d}", timezoneOffset);
      xSemaphoreGive(settingsMutex);
    } else {
      snprintf(buf, sizeof(buf), "{\"offset\":7}");
    }
    sendJSONResponse(request, String(buf));
  });

  server.on("/settimezone", HTTP_POST, [](AsyncWebServerRequest *request) {
      if (!request->hasParam("offset", true)) {
        request->send(400, "application/json", "{\"error\":\"Missing offset\"}"); return;
      }
      int offset = request->getParam("offset", true)->value().toInt();
      if (offset < -12 || offset > 14) {
        request->send(400, "application/json", "{\"error\":\"Invalid offset\"}"); return;
      }
      if (xSemaphoreTake(settingsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        timezoneOffset = offset; xSemaphoreGive(settingsMutex);
      }
      bool ntpTriggered = false;
      if (wifiConfig.isConnected && ntpTaskHandle != NULL) {
        if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          timeConfig.ntpSynced = false; xSemaphoreGive(timeMutex);
        }
        xTaskNotifyGive(ntpTaskHandle);
        ntpTriggered = true;
      }
      char buf[64];
      snprintf(buf, sizeof(buf),
        "{\"success\":true,\"offset\":%d,\"ntpTriggered\":%s}",
        offset, ntpTriggered ? "true" : "false"
      );
      request->send(200, "application/json", buf);
      vTaskDelay(pdMS_TO_TICKS(50));
      saveTimezoneConfig();
  });

  // API data umum
  server.on("/api/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    char timeStr[20], dateStr[20], dayStr[15];
    time_t now_t;
    struct tm timeinfo;
    if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      now_t = timeConfig.currentTime; xSemaphoreGive(timeMutex);
    } else { time(&now_t); }

    localtime_r(&now_t, &timeinfo);
    sprintf(timeStr, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    sprintf(dateStr, "%02d/%02d/%04d", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
    const char *dayNames[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
    strcpy(dayStr, dayNames[timeinfo.tm_wday]);

    bool isWiFiConnected = (WiFi.status() == WL_CONNECTED && wifiConfig.isConnected);
    String wifiStateStr;
    switch (wifiState) {
      case WIFI_IDLE: wifiStateStr = "idle"; break;
      case WIFI_CONNECTING: wifiStateStr = "connecting"; break;
      case WIFI_CONNECTED: wifiStateStr = "connected"; break;
      case WIFI_FAILED: wifiStateStr = "failed"; break;
      default: wifiStateStr = "unknown"; break;
    }

    char jsonBuffer[512];
    snprintf(jsonBuffer, sizeof(jsonBuffer),
      "{\"time\":\"%s\",\"date\":\"%s\",\"day\":\"%s\",\"timestamp\":%lu,"
      "\"device\":{\"wifiConnected\":%s,\"wifiState\":\"%s\",\"rssi\":%d,"
      "\"apIP\":\"%s\",\"ntpSynced\":%s,\"ntpServer\":\"%s\",\"freeHeap\":%d,\"uptime\":%lu}}",
      timeStr, dateStr, dayStr, (unsigned long)now_t,
      isWiFiConnected ? "true" : "false", wifiStateStr.c_str(),
      isWiFiConnected ? WiFi.RSSI() : 0,
      WiFi.softAPIP().toString().c_str(),
      timeConfig.ntpSynced ? "true" : "false",
      timeConfig.ntpServer.c_str(),
      ESP.getFreeHeap(), millis() / 1000
    );
    sendJSONResponse(request, String(jsonBuffer));
  });

  // Countdown status
  server.on("/api/countdown", HTTP_GET, [](AsyncWebServerRequest *request) {
      char buf[256];
      if (countdownMutex != NULL && xSemaphoreTake(countdownMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        snprintf(buf, sizeof(buf),
          "{\"active\":%s,\"remaining\":%d,\"total\":%d,"
          "\"message\":\"%s\",\"reason\":\"%s\",\"serverTime\":%lu}",
          countdownState.isActive ? "true" : "false",
          getRemainingSeconds(),
          countdownState.totalSeconds,
          countdownState.message.c_str(),
          countdownState.reason.c_str(),
          millis()
        );
        xSemaphoreGive(countdownMutex);
      } else {
        snprintf(buf, sizeof(buf),
          "{\"active\":false,\"remaining\":0,\"total\":0,"
          "\"message\":\"\",\"reason\":\"\",\"serverTime\":%lu}",
          millis()
        );
      }
      sendJSONResponse(request, String(buf));
  });

  // Reset pabrik (wifi, ap, timezone)
  server.on("/reset", HTTP_POST, [](AsyncWebServerRequest *request) {
      if (resetTaskHandle != NULL) { vTaskDelete(resetTaskHandle); resetTaskHandle = NULL; }

      if (LittleFS.exists("/wifi_creds.txt"))  LittleFS.remove("/wifi_creds.txt");
      if (LittleFS.exists("/ap_creds.txt"))    LittleFS.remove("/ap_creds.txt");
      if (LittleFS.exists("/timezone.txt"))    LittleFS.remove("/timezone.txt");

      if (xSemaphoreTake(settingsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          wifiConfig.routerSSID = "";
          wifiConfig.routerPassword = "";
          wifiConfig.isConnected = false;
          DEFAULT_AP_SSID.toCharArray(wifiConfig.apSSID, 33);
          strcpy(wifiConfig.apPassword, DEFAULT_AP_PASSWORD);
          wifiConfig.apIP = IPAddress(192, 168, 100, 1);
          wifiConfig.apGateway = IPAddress(192, 168, 100, 1);
          wifiConfig.apSubnet = IPAddress(255, 255, 255, 0);
          xSemaphoreGive(settingsMutex);
      }

      timezoneOffset = 7;

      if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          const time_t EPOCH_2000 = 946684800;
          setTime(0, 0, 0, 1, 1, 2000);
          timeConfig.currentTime = EPOCH_2000;
          timeConfig.ntpSynced = false;
          timeConfig.ntpServer = "";
          xSemaphoreGive(timeMutex);
      }

      if (rtcAvailable) saveTimeToRTC();
      startCountdown("factory_reset", "Pengaturan Ulang Perangkat", 60);
      request->send(200, "text/plain", "OK");

      xTaskCreate([](void* param) {
          for (int i = 60; i > 0; i--) {
              if (i == 35) {
                  WiFi.disconnect(true); vTaskDelay(pdMS_TO_TICKS(500));
                  WiFi.mode(WIFI_OFF); vTaskDelay(pdMS_TO_TICKS(500));
                  if (ntpTaskHandle != NULL) vTaskSuspend(ntpTaskHandle);
                  if (wifiTaskHandle != NULL) vTaskSuspend(wifiTaskHandle);
                  vTaskDelay(pdMS_TO_TICKS(1000));
                  server.end(); vTaskDelay(pdMS_TO_TICKS(500));
              }
              vTaskDelay(pdMS_TO_TICKS(1000));
          }
          ESP.restart();
      }, "FactoryResetTask", 5120, NULL, 1, &resetTaskHandle);
  });

  server.on("/notfound", HTTP_GET, [](AsyncWebServerRequest *request) {
      String html = "<!DOCTYPE html><html><head><meta charset=\'UTF-8\'><title>404</title>"
        "<style>body{font-family:sans-serif;display:flex;align-items:center;justify-content:center;min-height:100vh;margin:0;background:#f0f2f5}"
        ".card{background:#fff;border-radius:16px;padding:50px;text-align:center;box-shadow:0 2px 12px rgba(0,0,0,.08)}"
        ".code{font-size:80px;font-weight:700;color:#4a90d9}.btn{padding:11px 36px;background:#4a90d9;color:#fff;text-decoration:none;border-radius:6px}</style></head>"
        "<body><div class=\'card\'><div class=\'code\'>404</div><h2>Halaman Tidak Ditemukan</h2>"
        "<a href=\'/\' class=\'btn\'>Beranda</a></div></body></html>";
      request->send(404, "text/html", html);
  });

  server.onNotFound([](AsyncWebServerRequest *request) {
    String url = request->url();
    if (url.endsWith(".css") || url.endsWith(".js") || url.endsWith(".png") || url.endsWith(".ico")) {
      request->send(404, "text/plain", "File not found"); return;
    }
    request->redirect("/notfound");
  });
}

// ============================================
// UTILITAS
// ============================================
bool init_littlefs() {
  if (!LittleFS.begin(true)) { Serial.println("MOUNT LITTLEFS GAGAL"); return false; }
  Serial.println("LITTLEFS TERPASANG");
  return true;
}

void createDefaultConfigFiles() {
  if (!LittleFS.exists("/ap_creds.txt")) {
    fs::File f = LittleFS.open("/ap_creds.txt", "w");
    if (f) {
      f.println(DEFAULT_AP_SSID); f.println(DEFAULT_AP_PASSWORD);
      f.println("192.168.100.1"); f.println("192.168.100.1"); f.println("255.255.255.0");
      f.flush(); f.close();
    }
  }
  if (!LittleFS.exists("/timezone.txt")) {
    fs::File f = LittleFS.open("/timezone.txt", "w");
    if (f) { f.println("7"); f.flush(); f.close(); }
  }
}

void printStackReport() {
  struct { TaskHandle_t h; const char *n; uint32_t s; } tasks[] = {
    {webTaskHandle,"Web",WEB_TASK_STACK_SIZE},
    {wifiTaskHandle,"WiFi",WIFI_TASK_STACK_SIZE},
    {ntpTaskHandle,"NTP",NTP_TASK_STACK_SIZE},
    {rtcTaskHandle,"RTC",RTC_TASK_STACK_SIZE}
  };
  for (int i = 0; i < 4; i++) {
    if (tasks[i].h) {
      UBaseType_t hwm = uxTaskGetStackHighWaterMark(tasks[i].h);
      uint32_t free = hwm * sizeof(StackType_t);
      uint32_t used = tasks[i].s - free;
      Serial.printf("%-10s: %5d/%5d (%.1f%%)\n", tasks[i].n, used, tasks[i].s, (used*100.0)/tasks[i].s);
    }
  }
}

// ============================================
// FREERTOS TASKS
// ============================================
int asyncScanNetworks() {
    esp_task_wdt_reset();
    WiFi.scanNetworks(true, false);
    unsigned long scanStart = millis();
    int result = WIFI_SCAN_RUNNING;
    while (result == WIFI_SCAN_RUNNING) {
        if (millis() - scanStart > 8000) { WiFi.scanDelete(); return -1; }
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(200));
        result = WiFi.scanComplete();
    }
    return result;
}

void connectToBestAP() {
    int found = asyncScanNetworks();
    if (found <= 0) {
        WiFi.begin(wifiConfig.routerSSID.c_str(), wifiConfig.routerPassword.c_str());
        return;
    }
    int bestIndex = -1, bestRSSI = -999;
    for (int i = 0; i < found; i++) {
        if (WiFi.SSID(i) == wifiConfig.routerSSID && WiFi.RSSI(i) > bestRSSI) {
            bestRSSI = WiFi.RSSI(i); bestIndex = i;
        }
    }
    uint8_t bestBSSID[6];
    bool hasBSSID = (bestIndex >= 0);
    if (hasBSSID) memcpy(bestBSSID, WiFi.BSSID(bestIndex), 6);
    WiFi.scanDelete();

    esp_netif_set_hostname(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), hostname.c_str());
    if (hasBSSID) {
        WiFi.begin(wifiConfig.routerSSID.c_str(), wifiConfig.routerPassword.c_str(), 0, bestBSSID);
    } else {
        WiFi.begin(wifiConfig.routerSSID.c_str(), wifiConfig.routerPassword.c_str());
    }
}

void wifiTask(void *parameter) {
    esp_task_wdt_add(NULL);
    bool autoUpdateDone = false;
    unsigned long lastMonitor = 0;

    while (true) {
        esp_task_wdt_reset();
        EventBits_t bits = xEventGroupWaitBits(wifiEventGroup,
            WIFI_CONNECTED_BIT | WIFI_DISCONNECTED_BIT | WIFI_GOT_IP_BIT,
            pdFALSE, pdFALSE, pdMS_TO_TICKS(5000));

        if (bits & WIFI_DISCONNECTED_BIT) {
            xEventGroupClearBits(wifiEventGroup, WIFI_DISCONNECTED_BIT);
            if (wifiRestartInProgress || apRestartInProgress) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }

            autoUpdateDone = false; ntpSyncInProgress = false; ntpSyncCompleted = false;

            IPAddress apIP = WiFi.softAPIP();
            if (apIP == IPAddress(0,0,0,0)) {
                WiFi.softAPdisconnect(false); vTaskDelay(pdMS_TO_TICKS(500));
                WiFi.softAPConfig(wifiConfig.apIP, wifiConfig.apGateway, wifiConfig.apSubnet);
                WiFi.softAP(wifiConfig.apSSID, wifiConfig.apPassword);
                vTaskDelay(pdMS_TO_TICKS(500));
            }

            if (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                wifiConfig.isConnected = false; wifiState = WIFI_IDLE; xSemaphoreGive(wifiMutex);
            }
            if (wifiConfig.routerSSID.length() > 0) {
                reconnectAttempts++;
                if (reconnectAttempts > MAX_RECONNECT_ATTEMPTS) {
                    wifiState = WIFI_FAILED; wifiFailedTime = millis();
                } else { connectToBestAP(); wifiState = WIFI_CONNECTING; }
            }
        }

        if (bits & WIFI_GOT_IP_BIT) {
            if (!autoUpdateDone && wifiConfig.isConnected) {
                if (ntpSyncCompleted && timeConfig.ntpSynced) autoUpdateDone = true;
            }
            if (millis() - lastMonitor > 60000) {
                lastMonitor = millis();
                Serial.printf("[MONITOR WIFI] RSSI: %d | IP: %s\n", WiFi.RSSI(), WiFi.localIP().toString().c_str());
            }
        }

        if (wifiState == WIFI_IDLE && wifiConfig.routerSSID.length() > 0) {
            if (!(bits & WIFI_CONNECTED_BIT)) { connectToBestAP(); wifiState = WIFI_CONNECTING; }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void ntpTask(void *parameter) {
    esp_task_wdt_add(NULL);
    while (true) {
        esp_task_wdt_reset();
        uint32_t notifyValue = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
        esp_task_wdt_reset();

        if (restartTaskHandle != NULL || resetTaskHandle != NULL) { vTaskSuspend(NULL); continue; }
        if (notifyValue == 0) { esp_task_wdt_reset(); continue; }

        ntpSyncInProgress = true; ntpSyncCompleted = false;
        Serial.println("NTP SINKRONISASI DIMULAI...");

        configTzTime("UTC0", ntpServers[0], ntpServers[1], ntpServers[2]);

        time_t now = 0;
        struct tm timeinfo = {0};
        int retry = 0;

        while (timeinfo.tm_year < (2000 - 1900) && ++retry < 40) {
            if (restartTaskHandle != NULL || resetTaskHandle != NULL) {
                ntpSyncInProgress = false; ntpSyncCompleted = false; vTaskSuspend(NULL); break;
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            time(&now); gmtime_r(&now, &timeinfo);
            esp_task_wdt_reset();
        }

        bool syncSuccess = (timeinfo.tm_year >= (2000 - 1900));

        if (syncSuccess) {
            int currentOffset;
            if (xSemaphoreTake(settingsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                currentOffset = timezoneOffset; xSemaphoreGive(settingsMutex);
            } else { currentOffset = 7; }

            time_t localTime = now + (currentOffset * 3600);

            if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                timeConfig.currentTime = localTime;
                setTime(timeConfig.currentTime);
                timeConfig.ntpSynced = true;
                timeConfig.ntpServer = String(ntpServers[0]);
                xSemaphoreGive(timeMutex);
            }

            if (rtcAvailable && isRTCValid()) saveTimeToRTC();
            Serial.println("NTP OK: UTC" + String(currentOffset >= 0 ? "+" : "") + String(currentOffset));
        } else {
            Serial.println("NTP GAGAL");
        }

        ntpSyncInProgress = false; ntpSyncCompleted = syncSuccess;
        esp_task_wdt_reset();
    }
}

void webTask(void *parameter) {
  esp_task_wdt_add(NULL);
  setupServerRoutes();
  server.begin();

  unsigned long lastStackReport = 0, lastMemCheck = 0, lastAPCheck = 0, lastCleanup = 0;
  size_t initialHeap = ESP.getFreeHeap();
  size_t lowestHeap = initialHeap;

  while (true) {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(5000));
    unsigned long now = millis();

    if (now - lastCleanup > 300000) {
      lastCleanup = now;
      if (restartTaskHandle && eTaskGetState(restartTaskHandle) == eDeleted) restartTaskHandle = NULL;
      if (resetTaskHandle && eTaskGetState(resetTaskHandle) == eDeleted) resetTaskHandle = NULL;
    }

    if (now - lastStackReport > 120000) { lastStackReport = now; printStackReport(); }

    if (now - lastMemCheck > 30000) {
      lastMemCheck = now;
      size_t currentHeap = ESP.getFreeHeap();
      if (currentHeap < lowestHeap) lowestHeap = currentHeap;
      Serial.printf("HEAP: %d BYTE (min: %d)\n", currentHeap, lowestHeap);
    }

    if (now - lastAPCheck > 5000) {
      lastAPCheck = now;
      if (!apRestartInProgress && !wifiRestartInProgress) {
        wifi_mode_t mode; esp_wifi_get_mode(&mode);
        if (mode != WIFI_MODE_APSTA) {
          WiFi.mode(WIFI_AP_STA); delay(100);
          WiFi.softAPConfig(wifiConfig.apIP, wifiConfig.apGateway, wifiConfig.apSubnet);
          WiFi.softAP(wifiConfig.apSSID, wifiConfig.apPassword);
        }
      }
    }

    if (wifiState == WIFI_FAILED && wifiConfig.routerSSID.length() > 0) {
      unsigned long backoff = 10000UL * (1UL << min(wifiRetryCount, 4));
      if (backoff > 120000UL) backoff = 120000UL;
      if (now - wifiFailedTime >= backoff) {
        wifiRetryCount++; reconnectAttempts = 0;
        connectToBestAP(); wifiState = WIFI_CONNECTING; wifiFailedTime = now;
      }
    }
  }
}

void rtcSyncTask(void *parameter) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(60000);

    while (true) {
        if (rtcAvailable && isRTCValid()) {
            DateTime rtcTime;
            if (xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                rtcTime = rtc.now(); xSemaphoreGive(i2cMutex);
            } else { vTaskDelayUntil(&xLastWakeTime, xFrequency); continue; }

            if (!isRTCTimeValid(rtcTime)) { vTaskDelayUntil(&xLastWakeTime, xFrequency); continue; }

            time_t systemTime; bool ntpSynced;
            if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                systemTime = timeConfig.currentTime; ntpSynced = timeConfig.ntpSynced; xSemaphoreGive(timeMutex);
            } else { vTaskDelayUntil(&xLastWakeTime, xFrequency); continue; }

            time_t rtcUnix = rtcTime.unixtime();
            int timeDiff = abs((int)(systemTime - rtcUnix));

            if (!ntpSynced && timeDiff > 2) {
                if (xSemaphoreTake(timeMutex, portMAX_DELAY) == pdTRUE) {
                    timeConfig.currentTime = rtcUnix; setTime(rtcUnix); xSemaphoreGive(timeMutex);
                }
            }
        }
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void internetCheckTask(void *parameter) {
  const TickType_t checkInterval = pdMS_TO_TICKS(30000);
  while (true) {
    vTaskDelay(checkInterval);
    if (wifiState != WIFI_CONNECTED) { internetAvailable = false; continue; }
    WiFiClient client;
    bool result = client.connect(IPAddress(8,8,8,8), 53, 3000);
    if (result) client.stop();
    if (result != internetAvailable) {
      internetAvailable = result;
      Serial.println(internetAvailable ? "[Internet] Tersedia" : "[Internet] Terputus");
    }
  }
}

void clockTickTask(void *parameter) {
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1000);
    static int autoSyncCounter = 0;
    const time_t EPOCH_2000 = 946684800;

    while (true) {
        if (xSemaphoreTake(timeMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (timeConfig.currentTime < EPOCH_2000) {
                setTime(0, 0, 0, 1, 1, 2000); timeConfig.currentTime = EPOCH_2000;
            } else { timeConfig.currentTime++; }
            xSemaphoreGive(timeMutex);
        }
        if (wifiConfig.isConnected && ++autoSyncCounter >= 3600) {
            autoSyncCounter = 0;
            if (ntpTaskHandle != NULL) xTaskNotifyGive(ntpTaskHandle);
        }
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
    }
}

void restartWiFiTask(void *parameter) {
    unsigned long now = millis();
    if (now - lastWiFiRestartRequest < RESTART_DEBOUNCE_MS) { vTaskDelete(NULL); return; }
    lastWiFiRestartRequest = now;

    if (wifiRestartMutex == NULL) wifiRestartMutex = xSemaphoreCreateMutex();
    if (!wifiRestartMutex || xSemaphoreTake(wifiRestartMutex, pdMS_TO_TICKS(100)) != pdTRUE) { vTaskDelete(NULL); return; }
    if (wifiRestartInProgress || apRestartInProgress) { xSemaphoreGive(wifiRestartMutex); vTaskDelete(NULL); return; }

    wifiRestartInProgress = true;
    vTaskDelay(pdMS_TO_TICKS(3000));

    String ssid, password;
    if (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        ssid = wifiConfig.routerSSID; password = wifiConfig.routerPassword;
        wifiConfig.isConnected = false; wifiState = WIFI_IDLE; reconnectAttempts = 0;
        xSemaphoreGive(wifiMutex);
    } else { wifiRestartInProgress = false; xSemaphoreGive(wifiRestartMutex); vTaskDelete(NULL); return; }

    WiFi.disconnect(false, false);
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (WiFi.softAPIP() == IPAddress(0,0,0,0)) {
        WiFi.softAPdisconnect(false); vTaskDelay(pdMS_TO_TICKS(500));
        WiFi.softAPConfig(wifiConfig.apIP, wifiConfig.apGateway, wifiConfig.apSubnet);
        WiFi.softAP(wifiConfig.apSSID, wifiConfig.apPassword);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (ssid.length() > 0) connectToBestAP();

    vTaskDelay(pdMS_TO_TICKS(1000));
    wifiRestartInProgress = false;
    xSemaphoreGive(wifiRestartMutex);
    vTaskDelete(NULL);
}

void restartAPTask(void *parameter) {
    unsigned long now = millis();
    if (now - lastAPRestartRequest < RESTART_DEBOUNCE_MS) { vTaskDelete(NULL); return; }
    lastAPRestartRequest = now;

    if (wifiRestartMutex == NULL) wifiRestartMutex = xSemaphoreCreateMutex();
    if (!wifiRestartMutex || xSemaphoreTake(wifiRestartMutex, pdMS_TO_TICKS(100)) != pdTRUE) { vTaskDelete(NULL); return; }
    if (apRestartInProgress || wifiRestartInProgress) { xSemaphoreGive(wifiRestartMutex); vTaskDelete(NULL); return; }

    apRestartInProgress = true;

    for (int i = 60; i > 0; i--) {
        if (i == 35) {
            if (WiFi.softAPgetStationNum() > 0) { esp_wifi_deauth_sta(0); vTaskDelay(pdMS_TO_TICKS(1000)); }
            WiFi.mode(WIFI_MODE_STA); WiFi.softAPdisconnect(true);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    char savedSSID[33], savedPassword[65];
    IPAddress savedAPIP, savedGateway, savedSubnet;

    if (xSemaphoreTake(settingsMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        strncpy(savedSSID, wifiConfig.apSSID, sizeof(savedSSID));
        strncpy(savedPassword, wifiConfig.apPassword, sizeof(savedPassword));
        savedAPIP = wifiConfig.apIP; savedGateway = wifiConfig.apGateway; savedSubnet = wifiConfig.apSubnet;
        xSemaphoreGive(settingsMutex);
    } else {
        DEFAULT_AP_SSID.toCharArray(savedSSID, sizeof(savedSSID));
        strncpy(savedPassword, DEFAULT_AP_PASSWORD, sizeof(savedPassword));
        savedAPIP = IPAddress(192,168,100,1); savedGateway = savedAPIP; savedSubnet = IPAddress(255,255,255,0);
    }

    WiFi.mode(WIFI_MODE_APSTA);
    WiFi.softAPConfig(savedAPIP, savedGateway, savedSubnet); vTaskDelay(pdMS_TO_TICKS(500));
    bool apStarted = WiFi.softAP(savedSSID, savedPassword); vTaskDelay(pdMS_TO_TICKS(2000));

    if (!apStarted) {
        DEFAULT_AP_SSID.toCharArray(savedSSID, sizeof(savedSSID));
        strcpy(savedPassword, DEFAULT_AP_PASSWORD);
        savedAPIP = IPAddress(192,168,100,1);
        WiFi.softAPConfig(savedAPIP, savedAPIP, IPAddress(255,255,255,0)); vTaskDelay(pdMS_TO_TICKS(500));
        WiFi.softAP(savedSSID, savedPassword); vTaskDelay(pdMS_TO_TICKS(2000));
    }

    Serial.println("AP RESTART SELESAI: " + WiFi.softAPIP().toString());
    vTaskDelay(pdMS_TO_TICKS(2000));
    apRestartInProgress = false;
    xSemaphoreGive(wifiRestartMutex);
    vTaskDelete(NULL);
}

// ================================
// SETUP
// ================================
void setup() {
#if !PRODUCTION
  Serial.begin(115200); delay(1000);
#endif

  Serial.println("\n========================================");
  Serial.println("ESP32 - WiFi / AP / NTP / RTC CLOCK");
  Serial.println("========================================\n");
  pinMode(RGB_R_PIN, OUTPUT); pinMode(RGB_G_PIN, OUTPUT); pinMode(RGB_B_PIN, OUTPUT);
  rgbOff(); rgbBootBlinking = true;
  xTaskCreate(rgbBootBlinkTask, "RGBBoot", 1024, NULL, 1, NULL);

  timeMutex = xSemaphoreCreateMutex();
  wifiMutex = xSemaphoreCreateMutex();    settingsMutex = xSemaphoreCreateMutex();
  i2cMutex = xSemaphoreCreateMutex();

  if (wifiConfig.apIP == IPAddress(0,0,0,0)) {
    wifiConfig.apIP = IPAddress(192,168,4,1); wifiConfig.apGateway = wifiConfig.apIP;
    wifiConfig.apSubnet = IPAddress(255,255,255,0);
  }

  init_littlefs(); createDefaultConfigFiles();
  loadWiFiCredentials(); loadTimezoneConfig();

  Wire.begin(); delay(500);
  rtcAvailable = initRTC();

  if (!rtcAvailable) {
      if (xSemaphoreTake(timeMutex, portMAX_DELAY) == pdTRUE) {
          const time_t EPOCH_2000 = 946684800;
          setTime(0,0,0,1,1,2000); timeConfig.currentTime = EPOCH_2000;
          xSemaphoreGive(timeMutex);
      }
  }

  setupWiFiEvents();
  WiFi.mode(WIFI_OFF); delay(500);

  esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (sta_netif) esp_netif_set_hostname(sta_netif, hostname.c_str());

  WiFi.mode(WIFI_AP_STA); delay(100);

  esp_wifi_set_protocol(WIFI_IF_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
  esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT40);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  WiFi.setSleep(WIFI_PS_NONE); esp_wifi_set_ps(WIFI_PS_NONE);
  esp_wifi_set_max_tx_power(78);
  WiFi.setAutoReconnect(false); WiFi.persistent(false);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);

  WiFi.softAPConfig(wifiConfig.apIP, wifiConfig.apGateway, wifiConfig.apSubnet);
  WiFi.softAP(wifiConfig.apSSID, wifiConfig.apPassword);
  delay(100);

  Serial.printf("AP AKTIF: %s | IP: %s\n", wifiConfig.apSSID, WiFi.softAPIP().toString().c_str());

  timeConfig.ntpServer = "pool.ntp.org";
  timeConfig.ntpSynced = false;

  esp_task_wdt_deinit();
  esp_task_wdt_config_t wdt_config = {.timeout_ms=100000, .idle_core_mask=0, .trigger_panic=true};
  esp_task_wdt_init(&wdt_config);
  xTaskCreatePinnedToCore(wifiTask,      "WiFi",  WIFI_TASK_STACK_SIZE,  NULL, WIFI_TASK_PRIORITY,  &wifiTaskHandle, 0);
  xTaskCreatePinnedToCore(ntpTask,       "NTP",   NTP_TASK_STACK_SIZE,   NULL, NTP_TASK_PRIORITY,   &ntpTaskHandle,  0);
  xTaskCreatePinnedToCore(webTask,       "Web",   WEB_TASK_STACK_SIZE,   NULL, WEB_TASK_PRIORITY,   &webTaskHandle,  0);
  xTaskCreatePinnedToCore(clockTickTask, "Clock", CLOCK_TASK_STACK_SIZE, NULL, CLOCK_TASK_PRIORITY, NULL,            0);
  xTaskCreate(internetCheckTask, "InternetCheck", 3072, NULL, 1, NULL);

  if (rtcAvailable) {
    xTaskCreatePinnedToCore(rtcSyncTask, "RTC Sync", RTC_TASK_STACK_SIZE, NULL, RTC_TASK_PRIORITY, &rtcTaskHandle, 0);
  }

  vTaskDelay(pdMS_TO_TICKS(500));

  if (wifiTaskHandle)  esp_task_wdt_add(wifiTaskHandle);
  if (webTaskHandle)   esp_task_wdt_add(webTaskHandle);
  if (ntpTaskHandle)   esp_task_wdt_add(ntpTaskHandle);

  Serial.println("SISTEM SIAP\n");
  rgbBootDone();
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}