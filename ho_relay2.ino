#include <Arduino.h>
#include <WiFi.h>
#include <EEPROM.h>
#include <PubSubClient.h> // 引入 MQTT 庫
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>  // 添加這行
#include <Update.h>       // 引入 Update 庫
#include <HTTPClient.h>   // 添加 HTTPClient 庫
#include <WiFiClientSecure.h>  // 添加 WiFiClientSecure 庫
#include <esp_wifi.h>          // ESP32 WiFi 底層 API（PMF 設定等）

const char* firmwareVersion = "1.7.0"; // 當前韌體版本
// uPesy ESP32 WROOM DevKit
// LED 閃爍模式定義
const unsigned long SHORT_BLINK = 200;  // 短閃持續時間 (毫秒)
const unsigned long LONG_BLINK = 800;   // 長閃持續時間 (毫秒)
const unsigned long PATTERN_PAUSE = 2000; // 模式間暫停時間 (毫秒)
const unsigned long QUICK_BLINK = 300;   // 快閃間隔時間 (毫秒)
const unsigned long PAIRING_BLINK = 200; // BLE 配對／設定已清除時的快閃間隔 (毫秒)

// LED 閃爍狀態變數
unsigned long lastBlinkTime = 0;
int blinkPattern = 0;  // 用於追蹤當前閃爍模式位置
bool ledState = false;

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"

BLEServer *pServer = NULL;
BLECharacteristic *pCharacteristic = NULL;
bool deviceConnected = false;


const char* deviceModel = "hoRelay2"; // 設備型號

// ESP32-C3 GPIO 定義
const int bootButton = 9;     // BOOT 按鈕在 GPIO 9
const int resetButton = 1;        // 

const int ledOnBoard = 3;    // 第二個按鈕在 GPIO 8
const int ledOnFace = 0;        // 
// 繼電器腳位：兩版板子分別接在不同腳位
//   341305A_P25_250814 → GPIO 7
//   341305A_Y176_250318 → GPIO 4
// 兩支同時驅動，未接 MOS 的那支為空接腳，輸出不會有副作用。
// 這樣可避免腳位設錯時該腳未初始化而浮空，導致 MOS 誤導通、繼電器恆閉燒毀設備。
const int relayPins[] = {4, 7};
const int relayPinCount = sizeof(relayPins) / sizeof(relayPins[0]);

// 其他全域變數
unsigned long buttonPressTime = 0;    // 記錄按下的時間
unsigned long button2PressTime = 0;   // 第二個按鈕按下的時間
unsigned long ledBlinkStart = 0;      // LED 開始閃爍的時間
const int LONG_PRESS_TIME = 3000;     // 長按 3 秒進入閃爍確認階段
const int BLINK_CONFIRM_TIME = 2000;  // 閃爍確認階段再按住 2 秒才清除設定
const int BLINK_INTERVAL = 250;       // 確認階段 LED 閃爍週期 (毫秒，亮/滅各半)
const int CONFIRM_SOLID_TIME = 700;   // 快閃結束後長亮 0.7 秒表示確認重置
bool isBlinking = false;              // LED 閃爍狀態
bool lastBootButtonState = HIGH;      // BOOT 按鈕上次狀態
bool lastResetButtonState = HIGH;     // RESET 按鈕上次狀態

// ── 開機按鈕自檢 ──
// 防止「開機即自動清除 WiFi 設定 → 重啟 → 再清除」的無限迴圈。
// 成因：某支按鈕腳從開機第一刻就是 LOW，而 lastXxxButtonState 初值為 HIGH，
// 會被誤判成「使用者剛按下」，5 秒後就把 EEPROM 清光並重啟，設備永遠無法上線。
// 2026-08 實際發生過：RESET 按鈕內部短路，把 GPIO 1 恆定拉到 GND。
// 對策：開機時短暫取樣兩支腳，整段都是 LOW 就判定卡住，本次開機停用該腳的重置功能。
const unsigned long BTN_SELFTEST_DURATION = 500;  // 自檢取樣總長度 (毫秒)
const unsigned long BTN_SELFTEST_INTERVAL = 50;   // 取樣間隔 (毫秒)
bool bootButtonUsable = true;                     // BOOT 按鈕是否可用於重置
bool resetButtonUsable = true;                    // RESET 按鈕是否可用於重置
String deviceIdString;                // 儲存格式化後的設備 ID
String legacyDeviceIdString;          // 舊版（MAC 反序）設備 ID，僅用於相容尚未更新的 App
bool relayState = false;              // 繼電器狀態
bool bleConfigMode = false;           // BLE 配對模式標誌
unsigned long wifiDisconnectStart = 0; // WiFi 斷線起始時間（用於 30 秒後熄燈）
const unsigned long LED_TIMEOUT = 30000; // 30 秒後停止閃爍

// ── MQTT 連線速度 ──
// 只用來印警告，不用來否決連線。詳見 quickConnectToIndex() 裡的說明。
const unsigned long MQTT_SLOW_CONNECT_WARN_MS = 1000;
// MQTT 斷線後每隔多久嘗試「一台」broker。詳見 smartConnectStep()。
const unsigned long MQTT_RETRY_INTERVAL_MS = 10000;

// ── WiFi 重連節奏 ──
// 設計理由寫在 loop() 的 WiFi 管理區段，改任何一個值之前先讀那段長註釋。
const unsigned long WIFI_CHECK_INTERVAL_MS = 5000;         // loop 檢查 WiFi 狀態的間隔
const unsigned long WIFI_KICK_INTERVAL_MS = 10000;         // 多久補送一次非阻塞的 esp_wifi_connect()
const unsigned long WIFI_FULL_PROBE_AFTER_MS = 60000;      // 斷線超過多久才動用阻塞式完整探測
const unsigned long WIFI_FULL_PROBE_COOLDOWN_MS = 120000;  // 兩次完整探測之間的最短間隔
const unsigned long WIFI_STATUS_PRINT_MS = 60000;          // 定期印出 WiFi 狀態的間隔
const unsigned long WIFI_SCAN_TIMEOUT_MS = 8000;           // scanNetworks() 的上限（core 預設 60 秒）

// WiFi 斷線原因碼（用於診斷）
volatile uint8_t lastWifiDisconnectReason = 0;

// WiFi 事件回調：取得底層斷線原因碼
void onWiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    lastWifiDisconnectReason = info.wifi_sta_disconnected.reason;
    Serial.printf("WiFi 斷線原因碼: %d\n", lastWifiDisconnectReason);
    // 常見原因碼：
    // 2=AUTH_EXPIRE, 6=NOT_ASSOCED, 7=NOT_AUTHED
    // 14=MIC_FAILURE, 15=4WAY_HANDSHAKE_TIMEOUT
    // 200=BEACON_TIMEOUT, 201=NO_AP_FOUND, 202=AUTH_FAIL
    // 203=ASSOC_FAIL, 204=HANDSHAKE_TIMEOUT
  }
}

// WiFi 設定（預設值）
char ssid[32] = "HBTech";
char password[32] = "94051311";

// 自訂 MQTT 伺服器設定（透過 App 配置，儲存在 EEPROM）
char mqttServer[32] = "";
char mqttUsername[16] = "";
char mqttPassword[16] = "";
int mqttPort = 1883;

// 預設 MQTT 伺服器清單（與 Flutter App 一致）
struct MqttServerConfig {
  const char* server;
  int port;
  const char* username;
  const char* password;
};

const MqttServerConfig DEFAULT_SERVERS[] = {
  {"mqttgo.io",               1883, NULL,         NULL},
  {"broker.hoban.tw",         1883, "hoban_user", "hoban_pass"},
  {"mqtt.eclipseprojects.io", 1883, NULL,         NULL},
  {"broker.emqx.io",          1883, NULL,         NULL},
  {"broker.hivemq.com",       1883, NULL,         NULL},
};
const int DEFAULT_SERVER_COUNT = sizeof(DEFAULT_SERVERS) / sizeof(DEFAULT_SERVERS[0]);
int currentServerIndex = 0;  // 當前連接的預設伺服器索引

bool useCustomServer = false;       // 是否使用自訂伺服器

// 目前**實際**連上的 MQTT 伺服器位址，由連線成功的那一刻寫入。
//
// 【不可以用「useCustomServer 為真就填 mqttServer」來推斷】
// smartConnectStep() 會在自訂伺服器連不上時 fallback 到預設伺服器，那時
// useCustomServer 仍然是 true。舊寫法會讓每 3 秒的保活狀態用 retained 訊息
// 把正確的 server_changed 事件蓋掉，App 上永久顯示一台它其實沒連的 broker。
// 指向的都是持久儲存（DEFAULT_SERVERS 的字串常數或全域 mqttServer 陣列），不會懸空。
const char* activeMqttServer = nullptr;

WiFiClient espClient; // MQTT 客戶端
PubSubClient mqttClient(espClient);

// 韌體更新相關
bool isUpdating = false;
int updateProgress = 0;



// BLE 連接回調
class MyServerCallbacks: public BLEServerCallbacks {
    
    void onConnect(BLEServer* pServer) {
      deviceConnected = true;
    }

    void onDisconnect(BLEServer* pServer) {
      deviceConnected = false;
    }
};



// 函數前向宣告
void saveWiFiConfig();
void loadWiFiConfig();
void clearWiFiConfig();
void waitForResetConfirm();
void checkStuckButtons();
bool anyResetButtonPressed();
void publishStatus();
void smartConnectStep();
void resetMqttProbe();

// WiFi 設定相關函數實作
void saveWiFiConfig() {
  EEPROM.begin(128);
  // 儲存 WiFi 設定
  for (int i = 0; i < 32; i++) {
    EEPROM.write(i, ssid[i]);
    EEPROM.write(i + 32, password[i]);
  }
  // 儲存自訂 MQTT 伺服器設定
  for (int i = 0; i < 32; i++) {
    EEPROM.write(i + 64, mqttServer[i]);
  }
  // 儲存 MQTT 認證資訊
  for (int i = 0; i < 16; i++) {
    EEPROM.write(i + 98, mqttUsername[i]);   // 98-113: MQTT 帳號
    EEPROM.write(i + 114, mqttPassword[i]);  // 114-129: MQTT 密碼
  }
  // 儲存 MQTT Port (2 bytes)
  EEPROM.write(126, mqttPort & 0xFF);        // 低位元組
  EEPROM.write(127, (mqttPort >> 8) & 0xFF); // 高位元組
  
  // 儲存 useCustomServer 標誌
  EEPROM.write(96, useCustomServer ? 1 : 0);

  EEPROM.commit();
}

void loadWiFiConfig() {
  EEPROM.begin(128);

  // 檢查 EEPROM 是否已初始化（檢查第一個字元是否為可列印字元或 NULL）
  char firstChar = EEPROM.read(0);
  bool isEEPROMValid = (firstChar >= 32 && firstChar <= 126) || firstChar == 0;

  if (!isEEPROMValid) {
    // EEPROM 未初始化或資料無效，使用預設值並儲存
    Serial.println("EEPROM 未初始化，使用預設 WiFi 設定");
    // ssid 和 password 已經有預設值（HBTech / 94051311）
    // mqttServer 等保持空白
    saveWiFiConfig();  // 將預設值寫入 EEPROM
    return;
  }

  // 讀取 WiFi 設定
  for (int i = 0; i < 32; i++) {
    ssid[i] = EEPROM.read(i);
    password[i] = EEPROM.read(i + 32);
  }
  // 讀取自訂 MQTT 伺服器設定
  for (int i = 0; i < 32; i++) {
    mqttServer[i] = EEPROM.read(i + 64);
  }
  // 讀取 MQTT 認證資訊
  for (int i = 0; i < 16; i++) {
    mqttUsername[i] = EEPROM.read(i + 98);   // 98-113: MQTT 帳號
    mqttPassword[i] = EEPROM.read(i + 114);  // 114-129: MQTT 密碼
  }
  // 讀取 MQTT Port (2 bytes)
  mqttPort = EEPROM.read(126) | (EEPROM.read(127) << 8);
  if (mqttPort == 0 || mqttPort == 0xFFFF) {
    mqttPort = 1883;  // 預設值
  }

  // 設置字串結尾
  ssid[31] = '\0';
  password[31] = '\0';
  mqttServer[31] = '\0';
  mqttUsername[15] = '\0';
  mqttPassword[15] = '\0';

  Serial.printf("已從 EEPROM 載入 WiFi 設定: %s\n", ssid);
}

void clearWiFiConfig() {
  // 如果已連接到 MQTT，發送重置狀態
  if (mqttClient.connected()) {
    const char* deviceId = getDeviceId();
    String statusTopic = String("hoban/") + deviceId + "/status";
    
    StaticJsonDocument<200> doc;
    doc["device_id"] = deviceId;
    doc["status"] = "reset";
    doc["server"] = mqttServer;
    doc["timestamp"] = millis() / 1000;
    
    char buffer[200];
    serializeJson(doc, buffer);
    
    mqttClient.publish(statusTopic.c_str(), buffer, true);
    Serial.println("已發送重置狀態到 MQTT");
    delay(1000); // 確保訊息有時間發送
  }

  EEPROM.begin(128);  // 增加 EEPROM 大小
  for (int i = 0; i < 128; i++) {  // 清除所有設定包括 MQTT
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  Serial.println("WiFi 設定已清除。重新啟動中...");
  delay(2000);
  ESP.restart();
}

// 開機按鈕自檢：短暫取樣兩支按鈕腳，整段都是 LOW 即判定卡住並停用其重置功能
// 必須在 pinMode(..., INPUT_PULLUP) 之後、進入任何重置流程之前呼叫
// 注意：這也會擋掉「按住重置鍵再上電」的操作，但那本來就不是合法流程
//（正常重置是設備運作中才長按），放開後重新上電即恢復
void checkStuckButtons() {
  const int totalSamples = BTN_SELFTEST_DURATION / BTN_SELFTEST_INTERVAL;
  int bootLowCount = 0;
  int resetLowCount = 0;

  for (int i = 0; i < totalSamples; i++) {
    if (digitalRead(bootButton) == LOW) bootLowCount++;
    if (digitalRead(resetButton) == LOW) resetLowCount++;
    delay(BTN_SELFTEST_INTERVAL);
  }

  bootButtonUsable = (bootLowCount < totalSamples);
  resetButtonUsable = (resetLowCount < totalSamples);

  if (bootButtonUsable && resetButtonUsable) {
    Serial.println("按鈕自檢: 正常");
    return;
  }

  if (!bootButtonUsable) {
    Serial.printf("⚠ 按鈕自檢: BOOT(GPIO %d) 恆為 LOW，本次開機停用其重置功能\n", bootButton);
  }
  if (!resetButtonUsable) {
    Serial.printf("⚠ 按鈕自檢: RESET(GPIO %d) 恆為 LOW，本次開機停用其重置功能\n", resetButton);
  }
  Serial.println("  若非按住按鈕開機，代表該腳短路或未接，請檢查硬體");
}

// 是否有「可用的」按鈕正被按下；診斷判定卡住的腳一律視為未按下
bool anyResetButtonPressed() {
  if (bootButtonUsable && digitalRead(bootButton) == LOW) return true;
  if (resetButtonUsable && digitalRead(resetButton) == LOW) return true;
  return false;
}

// 阻塞式重置確認流程（尚未設定 WiFi 的等待期間共用）
// 按住滿 3 秒 → LED 以 250ms 週期閃爍 → 閃爍期間再按住 2 秒 → 長亮 0.7 秒 → 清除設定並重啟
// 中途放開則取消，函式返回讓呼叫端繼續原流程
void waitForResetConfirm() {
  unsigned long pressStart = millis();
  bool confirmBlinking = false;
  Serial.println("偵測到按鈕按下，開始計時...");

  while (anyResetButtonPressed()) {
    unsigned long pressDuration = millis() - pressStart;

    if (pressDuration >= LONG_PRESS_TIME) {
      if (!confirmBlinking) {
        confirmBlinking = true;
        Serial.println("長按 3 秒達成，開始 LED 閃爍確認...");
      }

      unsigned long blinkDuration = pressDuration - LONG_PRESS_TIME;
      if (blinkDuration >= BLINK_CONFIRM_TIME) {
        Serial.println("確認重置，LED 長亮 0.7 秒後清除 WiFi 設定...");
        digitalWrite(ledOnFace, HIGH);
        digitalWrite(ledOnBoard, HIGH);
        delay(CONFIRM_SOLID_TIME);
        digitalWrite(ledOnFace, LOW);
        digitalWrite(ledOnBoard, LOW);
        clearWiFiConfig();  // 內含重啟，不會返回
        return;
      }

      bool shouldLedBeOn = (blinkDuration % BLINK_INTERVAL) < (BLINK_INTERVAL / 2);
      digitalWrite(ledOnFace, shouldLedBeOn ? HIGH : LOW);
      digitalWrite(ledOnBoard, shouldLedBeOn ? HIGH : LOW);
    }
    delay(20);
  }

  if (confirmBlinking) {
    digitalWrite(ledOnFace, LOW);
    digitalWrite(ledOnBoard, LOW);
  }
  Serial.println("按鈕放開，取消重置");
}

void blinkLED() {
  unsigned long currentTime = millis();

  if (bleConfigMode) {
    // BLE 配對模式（含長按清除設定後）：持續快閃 (200ms 間隔)，不設熄燈 timeout
    if (currentTime - lastBlinkTime >= PAIRING_BLINK) {
      ledState = !ledState;
      digitalWrite(ledOnFace, ledState);
      digitalWrite(ledOnBoard, ledState);
      lastBlinkTime = currentTime;
    }
  } else if (WiFi.status() != WL_CONNECTED) {
    // WiFi 未連接模式：記錄斷線時間，30 秒內快速閃爍，之後熄燈
    if (wifiDisconnectStart == 0) {
      wifiDisconnectStart = currentTime;
    }
    if (currentTime - wifiDisconnectStart < LED_TIMEOUT) {
      // 30 秒內：快速閃爍
      if (currentTime - lastBlinkTime >= QUICK_BLINK) {
        ledState = !ledState;
        digitalWrite(ledOnFace, ledState);
        digitalWrite(ledOnBoard, ledState);
        lastBlinkTime = currentTime;
      }
    } else {
      // 超過 30 秒：熄燈省電
      digitalWrite(ledOnFace, LOW);
      digitalWrite(ledOnBoard, LOW);
    }
  } else if (WiFi.status() == WL_CONNECTED && !mqttClient.connected()) {
    wifiDisconnectStart = 0;  // WiFi 已連上，重置斷線計時
    // WiFi 已連接但 MQTT 未連接：一長二短模式
    unsigned long patternTime = currentTime % (LONG_BLINK + SHORT_BLINK * 2 + SHORT_BLINK * 2 + SHORT_BLINK * 2 + PATTERN_PAUSE);

    if (patternTime < LONG_BLINK) {
      // 長閃
      digitalWrite(ledOnFace, HIGH);
      digitalWrite(ledOnBoard, HIGH);
    } else if (patternTime < LONG_BLINK + SHORT_BLINK) {
      // 長閃後暫停
      digitalWrite(ledOnFace, LOW);
      digitalWrite(ledOnBoard, LOW);
    } else if (patternTime < LONG_BLINK + SHORT_BLINK * 2) {
      // 第一個短閃
      digitalWrite(ledOnFace, HIGH);
      digitalWrite(ledOnBoard, HIGH);
    } else if (patternTime < LONG_BLINK + SHORT_BLINK * 3) {
      // 第一個短閃暫停
      digitalWrite(ledOnFace, LOW);
      digitalWrite(ledOnBoard, LOW);
    } else if (patternTime < LONG_BLINK + SHORT_BLINK * 4) {
      // 第二個短閃
      digitalWrite(ledOnFace, HIGH);
      digitalWrite(ledOnBoard, HIGH);
    } else {
      // 模式間暫停
      digitalWrite(ledOnFace, LOW);
      digitalWrite(ledOnBoard, LOW);
    }
  } else {
    // WiFi 和 MQTT 都已連接：LED 關閉
    wifiDisconnectStart = 0;  // 重置斷線計時
    digitalWrite(ledOnFace, LOW);
    digitalWrite(ledOnBoard, LOW);
  }
}

// BLE 回調類別
class MyCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        Serial.println("onWrite");
        uint8_t* data = pCharacteristic->getData();
        size_t len = pCharacteristic->getLength();
        
        if (len > 0) {
            char* buffer = (char*)malloc(len + 1);
            memcpy(buffer, data, len);
            buffer[len] = '\0';
            
            Serial.print("收到的設定：");
            Serial.println(buffer);
            
            // 建立 JSON 文件
            StaticJsonDocument<512> doc;  // 增加容量以支援認證資訊
            DeserializationError error = deserializeJson(doc, buffer);

            if (!error) {
                // 檢查是否有 wifi 物件
                if (doc.containsKey("wifi")) {
                    const char* newSSID = doc["wifi"]["ssid"];
                    const char* newPassword = doc["wifi"]["password"];
                    const char* newMqttServer = doc["wifi"]["server"];
                    const char* newMqttUsername = doc["wifi"]["mqtt_username"];  // MQTT 帳號（選用）
                    const char* newMqttPassword = doc["wifi"]["mqtt_password"];  // MQTT 密碼（選用）
                    int newMqttPort = doc["wifi"]["mqtt_port"] | 1883;  // MQTT 埠（選用，預設 1883）

                    Serial.println("收到設定：");
                    Serial.printf("SSID: %s\n", newSSID);
                    Serial.printf("MQTT Server: %s\n", newMqttServer);
                    Serial.printf("MQTT Port: %d\n", newMqttPort);
                    if (newMqttUsername) Serial.printf("MQTT Username: %s\n", newMqttUsername);

                    if (newSSID && newPassword && newMqttServer) {
                        // 複製 WiFi 設定到全域變數
                        strncpy(ssid, newSSID, sizeof(ssid) - 1);
                        strncpy(password, newPassword, sizeof(password) - 1);
                        strncpy(mqttServer, newMqttServer, sizeof(mqttServer) - 1);
                        ssid[sizeof(ssid) - 1] = '\0';
                        password[sizeof(password) - 1] = '\0';
                        mqttServer[sizeof(mqttServer) - 1] = '\0';

                        // 複製 MQTT 認證資訊（如果提供）
                        if (newMqttUsername) {
                            strncpy(mqttUsername, newMqttUsername, sizeof(mqttUsername) - 1);
                            mqttUsername[sizeof(mqttUsername) - 1] = '\0';
                        } else {
                            mqttUsername[0] = '\0';  // 清空
                        }

                        if (newMqttPassword) {
                            strncpy(mqttPassword, newMqttPassword, sizeof(mqttPassword) - 1);
                            mqttPassword[sizeof(mqttPassword) - 1] = '\0';
                        } else {
                            mqttPassword[0] = '\0';  // 清空
                        }

                        mqttPort = newMqttPort;
                        useCustomServer = true;  // 標記使用自訂伺服器

                        saveWiFiConfig();

                        // 建立回應 JSON
                        StaticJsonDocument<350> response;
                        response["status"] = "success";
                        response["message"] = "WiFi 和 MQTT 設定已儲存";
                        response["data"]["device_id"] = getDeviceId();  // 加入設備 ID
                        response["data"]["ssid"] = ssid;
                        response["data"]["mqttServer"] = mqttServer;
                        response["data"]["mqttPort"] = mqttPort;
                        response["data"]["hasAuth"] = (strlen(mqttUsername) > 0);

                        // 序列化 JSON 到字串
                        char responseBuffer[350];
                        serializeJson(response, responseBuffer);

                        // 印出回應
                        Serial.println("回應：");
                        Serial.println(responseBuffer);

                        // 回傳 JSON 回應
                        pCharacteristic->setValue((uint8_t*)responseBuffer, strlen(responseBuffer));
                        pCharacteristic->notify();

                        free(buffer);
                        delay(2000);
                        ESP.restart();
                    } else {
                        // 錯誤回應
                        StaticJsonDocument<200> response;
                        response["status"] = "error";
                        response["message"] = "SSID、密碼或伺服器格式錯誤";

                        char responseBuffer[200];
                        serializeJson(response, responseBuffer);
                        pCharacteristic->setValue((uint8_t*)responseBuffer, strlen(responseBuffer));
                        pCharacteristic->notify();
                    }
                } else {
                    // 錯誤回應
                    StaticJsonDocument<200> response;
                    response["status"] = "error";
                    response["message"] = "無效的JSON格式";
                    
                    char responseBuffer[200];
                    serializeJson(response, responseBuffer);
                    pCharacteristic->setValue((uint8_t*)responseBuffer, strlen(responseBuffer));
                    pCharacteristic->notify();
                }
            }
            free(buffer);
        }
    }
};

const char* getDeviceId() {
  if (deviceIdString.length() == 0) {  // 如果還沒有產生過
    uint64_t chipId = ESP.getEfuseMac();
    uint8_t* chipIdBytes = (uint8_t*)&chipId;

    // ESP.getEfuseMac() 以小端序把 mac[0]..mac[5] 寫進 uint64 的最低 6 個位元組，
    // 因此 chipIdBytes[0] 就是 mac[0]。由 [0] 印到 [5] 才是網路順序（與 WiFi.macAddress() 一致）
    char tempId[23];
    snprintf(tempId, 23, "hoban-%02x%02x%02x%02x%02x%02x",
      chipIdBytes[0],  // mac[0]（廠商 OUI 開頭）
      chipIdBytes[1],
      chipIdBytes[2],
      chipIdBytes[3],
      chipIdBytes[4],
      chipIdBytes[5]   // mac[5]
    );

    deviceIdString = String(tempId);
  }
  return deviceIdString.c_str();
}

// 舊版設備 ID（MAC 反序輸出，1.2.1 以前的格式）
// 只用來額外訂閱舊的 control 主題，讓尚未更新的 App 仍能控制設備，避免 OTA 後失聯。
// 狀態一律只發布到新的正序主題，待所有 App 完成遷移後可移除本函式。
const char* getLegacyDeviceId() {
  if (legacyDeviceIdString.length() == 0) {
    uint64_t chipId = ESP.getEfuseMac();
    uint8_t* chipIdBytes = (uint8_t*)&chipId;

    char tempId[23];
    snprintf(tempId, 23, "hoban-%02x%02x%02x%02x%02x%02x",
      chipIdBytes[5],
      chipIdBytes[4],
      chipIdBytes[3],
      chipIdBytes[2],
      chipIdBytes[1],
      chipIdBytes[0]
    );

    legacyDeviceIdString = String(tempId);
  }
  return legacyDeviceIdString.c_str();
}

// 初始化繼電器腳位並確保斷電
// 必須在 setup() 最開頭呼叫：ESP32-C3 的 GPIO 4/7 為 JTAG 腳（MTMS/MTDO），
// reset 後的狀態不保證為低電位，越晚拉低、MOS 誤導通的時間窗就越長
void initRelayPins() {
  for (int i = 0; i < relayPinCount; i++) {
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
  }
  relayState = false;
}

// 同時設定所有繼電器腳位的輸出
void setRelayPins(bool on) {
  for (int i = 0; i < relayPinCount; i++) {
    digitalWrite(relayPins[i], on ? HIGH : LOW);
  }
  relayState = on;
}

// 註：曾在此加過「讀繼電器腳判斷 MOS gate 是否被短路」的自檢，2026-08-17 撤回。
// 兩版板子的正常狀態相反——P25 版（繼電器在 GPIO 7）正常運作時 gate 本來就被拉在
// 高電位，Y176 版（GPIO 4）則是低電位，所以「讀到 HIGH 就是故障」在 P25 版上必然
// 誤報。要重做必須先能分辨板版，或改用不依賴絕對電位的判準。
// 詳見 .claude/rules/relay-stuck-on-diagnosis.md

// 印出目前驅動的繼電器腳位
void printRelayPins() {
  Serial.print("繼電器腳位: ");
  for (int i = 0; i < relayPinCount; i++) {
    if (i > 0) Serial.print(", ");
    Serial.printf("GPIO %d", relayPins[i]);
  }
  Serial.println();
}

// 打開繼電器和燈（不自動關閉）
void relayOn() {
  Serial.println("═══ 繼電器 ON ═══");
  printRelayPins();

  setRelayPins(true);
  digitalWrite(ledOnFace, HIGH);
  digitalWrite(ledOnBoard, HIGH);

  Serial.println("✓ 繼電器已開啟（長亮狀態）");

  // 使用 JSON 格式發布狀態
  publishStatus();
}

// 關閉繼電器和燈
void relayOff() {
  Serial.println("═══ 繼電器 OFF ═══");

  setRelayPins(false);
  digitalWrite(ledOnFace, LOW);
  digitalWrite(ledOnBoard, LOW);

  Serial.println("✓ 繼電器已關閉");

  // 使用 JSON 格式發布狀態
  publishStatus();
}

// 保留舊的 pulseRelay 函數以向後相容（如果有其他地方使用）
void pulseRelay() {
  Serial.println("═══ 觸發繼電器 ═══");
  printRelayPins();

  Serial.println("→ 繼電器 ON（點動）");
  setRelayPins(true);
  digitalWrite(ledOnFace, HIGH);
  digitalWrite(ledOnBoard, HIGH);

  // 先把「開啟中」發布出去，否則外部只看得到點動結束後的狀態，
  // relay 欄位永遠是 0，App 會誤以為繼電器從來沒有開過
  publishStatus();

  delay(2000);  // 改為 2 秒長亮

  Serial.println("→ 繼電器 OFF");
  setRelayPins(false);
  digitalWrite(ledOnFace, LOW);
  digitalWrite(ledOnBoard, LOW);

  Serial.println("✓ 繼電器觸發完成");

  // 使用 JSON 格式發布狀態
  publishStatus();
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String deviceId = getDeviceId();
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  Serial.println("═══ MQTT 訊息 ═══");
  Serial.print("主題: ");
  Serial.println(topic);
  Serial.print("內容: ");
  Serial.println(message);
  Serial.print("長度: ");
  Serial.println(length);

  String expectedTopic = String("hoban/") + deviceId + "/control";
  // 舊版主題（MAC 反序）仍然受理，避免尚未更新的 App 無法控制
  String legacyTopic = String("hoban/") + getLegacyDeviceId() + "/control";
  Serial.print("預期主題: ");
  Serial.println(expectedTopic);

  String topicStr = String(topic);
  if (topicStr == expectedTopic || topicStr == legacyTopic) {
    if (topicStr == legacyTopic) {
      Serial.println("⚠ 收到舊版主題指令（App 尚未更新設備 ID）");
    }
    Serial.println("✓ 主題匹配，處理指令...");

    if (message == "status") {
      Serial.println("→ 執行：發布狀態");
      publishStatus();  // 使用 JSON 格式發布狀態
    } else if (message == "ON") {
      Serial.println("→ 執行：打開繼電器（點動）");
      pulseRelay();
    } else if (message == "OFF") {
      Serial.println("→ 執行：關閉繼電器");
      relayOff();
    } else if (message == "reset") {
      Serial.println("收到重置命令，執行重置...");
      clearWiFiConfig();  // 清除 WiFi 設定並重啟
    } else if (message == "FIND_BEST_SERVER") {
      // 重新測試所有伺服器並選擇最快的。
      //
      // 【不可以在這裡直接呼叫 smartConnect()】這支 callback 是從 mqttClient.loop()
      // 裡面被分派的，而 smartConnect() 會一次試完 1 台自訂 + 5 台預設，
      // 每台 = 不受 caller timeout 管的 DNS + 3 秒 TCP + 15 秒等 CONNACK，
      // 單次 loop() 迭代阻塞 111 秒以上——跟這整輪重寫要消滅的舊行為一模一樣。
      // 改成只斷線並把探測狀態歸零，剩下的交給 loop() 的 smartConnectStep()
      // 依 MQTT_RETRY_INTERVAL_MS 的節奏一次試一台。
      //
      // 【必須順手推進 currentServerIndex】只做 resetMqttProbe() 是不夠的：
      // 它把 mqttProbeOffset 歸零，而下一次 smartConnectStep() 試的就是
      // (currentServerIndex + 0)，也就是剛剛那一台。本輪又拿掉了「<1 秒才接受」
      // 的門檻，於是那次重連必定成功 —— 指令就再也換不掉伺服器，變成原地重連。
      Serial.println("收到重新測試伺服器命令，換下一台並交由 loop 逐台重試");
      mqttClient.disconnect();
      resetMqttProbe();
      currentServerIndex = (currentServerIndex + 1) % DEFAULT_SERVER_COUNT;
      Serial.printf("下一輪從 [%d] %s 開始\n",
                    currentServerIndex, DEFAULT_SERVERS[currentServerIndex].server);
    } else if (message.startsWith("update:")) {
      // 解析更新命令
      StaticJsonDocument<200> doc;
      DeserializationError error = deserializeJson(doc, message.substring(7));

      if (!error) {
        const char* newVersion = doc["version"];
        const char* downloadUrl = doc["url"];

        if (newVersion && downloadUrl) {
          Serial.println("收到韌體更新請求");
          Serial.print("新版本：");
          Serial.println(newVersion);
          Serial.print("下載網址：");
          Serial.println(downloadUrl);

          // 開始更新程序
          startFirmwareUpdate(downloadUrl);
        }
      }
    } else {
      Serial.print("→ 未知指令: ");
      Serial.println(message);
    }
  } else {
    Serial.println("✗ 主題不匹配，忽略訊息");
  }
  Serial.println("═══════════════");
}





void setupBLE() {
  const char* deviceId = getDeviceId();
  BLEDevice::init(deviceId);
  
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
                      CHARACTERISTIC_UUID,
                      BLECharacteristic::PROPERTY_READ |
                      BLECharacteristic::PROPERTY_WRITE |
                      BLECharacteristic::PROPERTY_NOTIFY
                    );

  pCharacteristic->setCallbacks(new MyCallbacks());
  pCharacteristic->addDescriptor(new BLE2902());
  
  pService->start();
  BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);  
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();
  
  Serial.print("BLE 已啟動，名稱: ");
  Serial.println(deviceId);
}

void setup()
{
  // 最優先執行：立刻把繼電器腳位設為輸出並拉低，
  // 縮短上電後 MOS 可能誤導通的時間窗（開機瞬間繼電器咔一下的成因）
  initRelayPins();

  Serial.begin(115200);
  delay(1000); // 等待序列埠穩定

  Serial.println("齁控－動物管制遠端控制系統 v" + String(firmwareVersion));
  Serial.println("================");

  printRelayPins();

  // 設定並關閉內建 LED
  pinMode(ledOnBoard, INPUT);  // 初始化第二個按鈕
  digitalWrite(ledOnBoard, LOW);  // 關閉 LED
  
  pinMode(ledOnFace, OUTPUT);
  digitalWrite(ledOnFace, LOW);  // 關閉 LED

  pinMode(bootButton, INPUT_PULLUP);  // 改用 INPUT_PULLUP
  pinMode(resetButton, INPUT_PULLUP);  // 改用 INPUT_PULLUP
  delay(50);  // 等內部提升電阻把腳位拉穩再取樣

  checkStuckButtons();  // 必須早於任何重置流程，卡住的腳會在此被排除

  loadWiFiConfig();

  // 讀取使用自訂伺服器標誌
  EEPROM.begin(128);
  useCustomServer = (EEPROM.read(96) == 1);
  Serial.printf("使用自訂伺服器: %s\n", useCustomServer ? "是" : "否");

  const char* deviceId = getDeviceId();  // 獲取設備 ID

  // 配置 WiFi 設定以提高穩定性
  Serial.println("=== 初始化 WiFi 設定 ===");
  WiFi.onEvent(onWiFiEvent);     // 註冊 WiFi 事件回調（取得斷線原因碼）
  WiFi.mode(WIFI_STA);           // 先設定模式（ESP32-C3 必須先設定模式再做其他配置）
  WiFi.persistent(false);        // 不將 WiFi 配置寫入 Flash（減少寫入次數，延長壽命）
  WiFi.setAutoReconnect(true);   // 啟用自動重連（ESP32 底層會嘗試重連）
  WiFi.setSleep(false);          // 禁用 WiFi 睡眠模式（提高穩定性，避免斷線）

  // 設定 WiFi 電源模式為最大性能（犧牲一點耗電換取穩定性）
  WiFi.setTxPower(WIFI_POWER_19_5dBm);  // 設定最大發射功率

  Serial.printf("WiFi 模式: STA (Station)\n");
  Serial.printf("自動重連: 啟用\n");
  Serial.printf("睡眠模式: 禁用\n");
  Serial.printf("發射功率: 19.5dBm (最大)\n");

  if (strlen(ssid) > 0) {
    Serial.println("SSID: " + String(ssid));
    Serial.println("開始連接 WiFi...");
    connectToWiFi();
    
    // 只有在成功連接到 WiFi 時才使用智慧連接
    if (WiFi.status() == WL_CONNECTED)
    {
      smartConnect();  // 使用智慧連接取代 connectToMQTT()
    }
  } else {
    // 沒有 WiFi 設定，啟動 BLE 配對模式
    Serial.println("找不到 WiFi 設定，啟動 BLE 配對模式...");
    bleConfigMode = true;
    setupBLE();
    Serial.println("請使用 App 透過 BLE 配對設定 WiFi");
  }

}

void loop()
{
  // 讀取按鈕當前狀態（開機診斷判定卡在 LOW 的腳一律視為 HIGH，不參與重置流程）
  bool currentBootState = bootButtonUsable ? digitalRead(bootButton) : HIGH;
  bool currentResetState = resetButtonUsable ? digitalRead(resetButton) : HIGH;

  // 檢查按鈕是否被按下（從 HIGH 變成 LOW）
  if ((currentBootState == LOW && lastBootButtonState == HIGH) || 
      (currentResetState == LOW && lastResetButtonState == HIGH)) {
    // 按鈕剛被按下，開始計時
    if (buttonPressTime == 0) {
      buttonPressTime = millis();
      Serial.println("偵測到按鈕按下，開始計時...");
    }
  }

  // 如果按鈕正在被按下
  if (currentBootState == LOW || currentResetState == LOW) {
    // buttonPressTime 為 0 代表沒抓到 HIGH→LOW 邊緣（例如一支按鈕放開的同時另一支已按住）。
    // 此時 millis() - 0 會等於開機時間、瞬間超過門檻而立刻重置，必須先補上計時起點。
    if (buttonPressTime == 0) {
      buttonPressTime = millis();
      Serial.println("偵測到按鈕按下（補記計時起點）...");
    }

    unsigned long pressDuration = millis() - buttonPressTime;
    
    // 長按超過 3 秒，開始閃爍 LED
    if (!isBlinking && pressDuration >= LONG_PRESS_TIME) {
      isBlinking = true;
      ledBlinkStart = millis();
      Serial.println("長按 3 秒達成，開始 LED 閃爍確認...");
    }
    
    // 閃爍期間
    if (isBlinking) {
      unsigned long blinkDuration = millis() - ledBlinkStart;

      // 閃爍 2 秒
      if (blinkDuration < BLINK_CONFIRM_TIME) {
        // 250ms 週期閃爍
        bool shouldLedBeOn = (blinkDuration % BLINK_INTERVAL) < (BLINK_INTERVAL / 2);
        digitalWrite(ledOnFace, shouldLedBeOn ? HIGH : LOW);
        digitalWrite(ledOnBoard, shouldLedBeOn ? HIGH : LOW);
      } else {
        // 閃爍 2 秒後，如果按鈕還在按著，長亮 0.7 秒再執行重置
        Serial.println("確認重置，LED 長亮 0.7 秒後清除 WiFi 設定...");
        digitalWrite(ledOnFace, HIGH);
        digitalWrite(ledOnBoard, HIGH);
        delay(CONFIRM_SOLID_TIME);
        digitalWrite(ledOnFace, LOW);
        digitalWrite(ledOnBoard, LOW);
        clearWiFiConfig();  // 清除設定並重啟
      }
    }
  } else {
    // 按鈕被放開，重置所有狀態
    if (buttonPressTime != 0) {
      Serial.println("按鈕放開，取消重置");
    }
    buttonPressTime = 0;
    isBlinking = false;
  }

  // 更新按鈕上次狀態
  lastBootButtonState = currentBootState;
  lastResetButtonState = currentResetState;

  // 當不在按鈕長按流程時，根據連接狀態控制 LED 閃燈
  if (!isBlinking) {
    blinkLED();
  }

  // BLE 配對模式：只處理 BLE 連線
  if (bleConfigMode) {
    if (deviceConnected) {
      delay(10);
    }
    return;  // BLE 配對模式下不執行其他邏輯
  }

  // WiFi 和 MQTT 管理
  static unsigned long lastWiFiCheck = 0;
  static unsigned long lastKeepAlive = 0;
  static unsigned long nextMqttAttemptAt = 0;  // 下次允許嘗試 MQTT 連線的時刻，0 = 從未設定
  static unsigned long wifiConnectedTime = 0;  // 記錄連接成功的時間
  static unsigned long wifiDownSince = 0;      // 本次斷線的起點，0 代表目前連著
  static unsigned long lastWifiKick = 0;       // 上次補送 esp_wifi_connect() 的時間
  static unsigned long nextFullProbeAt = 0;    // 下次允許完整探測的時刻，0 = 從未設定
  unsigned long now = millis();

  // ══ WiFi 連線維持（2026-08 整段重寫，動手改之前請先讀完這段）══
  //
  // 【舊版做了什麼】
  // 每 5 秒呼叫一次阻塞式的 connectToWiFi()，而它進門第一件事就是
  //     WiFi.disconnect(true) → WiFi.mode(WIFI_OFF) → WiFi.mode(WIFI_STA)
  //
  // 【為什麼那是錯的】
  // 在 Arduino ESP32 core 3.3.7，disconnect() 的簽章是
  //     disconnect(bool wifioff = false, bool eraseap = false, unsigned long timeoutLength = 100)
  // 第一個參數是 wifioff，不是 eraseap（舊註釋「true = 清除之前的 AP 配置」寫錯了）。
  // wifioff=true 會一路走到 STAClass::onDisable()（core 的 STA.cpp），那裡做兩件致命的事：
  //     Network.removeEvent(_wifi_sta_event_handle);   // WiFi 事件處理器整個移除
  //     _esp_netif = NULL;                             // 之後 connect() 直接 return false
  //
  // 而 core 自己的 auto-reconnect 正是掛在那個事件處理器上（STA.cpp 的
  // _onStaArduinoEvent → STA_DISCONNECTED 分支 → disconnect(); connect();），它跑在
  // WiFi 事件任務裡、完全不佔 loop()，AP 一回來就會自己接上。
  //
  // 也就是說：舊版每次「重連」都先親手把正在運作的自動重連拆掉，再改用最壞
  // 50 秒以上的阻塞方式自己重試——而那段期間 mqttClient.loop() 一次都跑不到，
  // broker 在 1.5×keepAlive（設定 30 秒 → 45 秒）內沒收到封包就會把設備踢掉。
  //
  // 【新做法】
  //   1. 不再呼叫 WiFi.disconnect(true) / WiFi.mode(WIFI_OFF)，讓 core 的
  //      auto-reconnect 保持有效。AP 短暫消失再回來（最常見的情境）它自己會處理，
  //      韌體完全不介入、不阻塞。
  //   2. 久久沒接回來時，補送一次非阻塞的 esp_wifi_connect()。core 的
  //      auto-reconnect 沒有任何退避，偶爾會卡住，這是補刀而不是取代。
  //   3. 只有在 core 不會自動重連的情況，才動用阻塞式的完整多 auth 探測：
  //      原因碼 202(AUTH_FAIL) 與 15(4WAY_HANDSHAKE_TIMEOUT) 都不在 core 的重連白名單
  //      _is_staReconnectableReason() 裡，那種情況 core 一次都不會重試；
  //      或斷線已超過 WIFI_FULL_PROBE_AFTER_MS 仍沒好。兩次完整探測之間有冷卻。
  //
  // 【一併移除的東西】
  // 舊版的「三種升級策略」。盤點證實那三種走的是同一套動作——connectToWiFi() 開頭
  // 就把前置的 disconnect/OFF/STA 全部重做一遍，所以差別只有多墊 3 秒與 5 秒的
  // 「可中斷延遲」。序列埠上三行不同的「策略：…」訊息對應到完全相同的行為。
  //（那個 interruptibleDelay() 函式的呼叫點全在這一段裡，一併移除了。）
  // 還有那句「⚠ 重連失敗次數過多，暫停重試 30 秒」：它寫的是
  //     lastWiFiCheck = now + 25000;
  // 而判斷式是 now - lastWiFiCheck > 5000，兩邊都是 unsigned long，相減得
  // 4294942296 ≫ 5000，條件恆真 → 那句話印出來的下一個 loop 迭代就立刻重試，
  // 「暫停 30 秒」從來沒有發生過。
  if (now - lastWiFiCheck > WIFI_CHECK_INTERVAL_MS) {
    lastWiFiCheck = now;

    if (WiFi.status() == WL_CONNECTED) {
      // ── 已連上 ──
      if (wifiDownSince != 0) {
        Serial.printf("✓ WiFi 連接已恢復（中斷 %lu 秒）\n", (now - wifiDownSince) / 1000);
        Serial.printf("訊號強度: %d dBm\n", WiFi.RSSI());
        wifiDownSince = 0;
        wifiConnectedTime = now;
        lastWifiDisconnectReason = 0;
        // 冷卻是為了避免「連不上時反覆跑阻塞探測」，一旦連上就沒有意義了。
        // 不清掉的話，連上後 10 秒又以 reason 202 斷線時（core 明確不會重連），
        // 會被自家的殘餘冷卻鎖住最多 120 秒，期間只能空送必定失敗的 esp_wifi_connect()。
        nextFullProbeAt = 0;
      }
      if (wifiConnectedTime == 0) {
        wifiConnectedTime = now;
      }

      // 定期印出 WiFi 狀態（每分鐘）
      static unsigned long lastStatusPrint = 0;
      if (now - lastStatusPrint > WIFI_STATUS_PRINT_MS) {
        lastStatusPrint = now;
        Serial.printf("ℹ WiFi 狀態: 已連接 %lu 秒，訊號 %d dBm\n",
                     (now - wifiConnectedTime) / 1000, WiFi.RSSI());
      }
    } else if (strlen(ssid) > 0) {
      // ── 斷線中 ──
      if (wifiDownSince == 0) {
        // 0 是「目前連著」的哨兵，所以起點不能真的是 0。
        // millis() 恰為 0 的那 1 毫秒（開機瞬間、每 49.7 天一次）若不避開，
        // 下一個 tick 會重新判定成「首次斷線」，把 60 秒探測時鐘白白歸零一次。
        wifiDownSince = (now == 0) ? 1 : now;
        lastWifiKick = now;  // 剛斷線先讓 core 自己試一輪，不要立刻插手
        Serial.println("═══ WiFi 連接中斷 ═══");
        Serial.printf("斷線時間: %lu ms，原因碼: %d\n", now, lastWifiDisconnectReason);
        if (wifiConnectedTime > 0) {
          Serial.printf("已連接時長: %lu 秒\n", (now - wifiConnectedTime) / 1000);
        }
        Serial.println("→ 先交給 core 的 auto-reconnect 處理（跑在事件任務，不佔 loop）");
      }

      unsigned long downFor = now - wifiDownSince;

      // ── 哪些原因碼是「core 不會自己重試、只能韌體出手」──
      //
      // 依 core 3.3.7 的白名單 _is_staReconnectableReason()（STA.cpp:58-85）逐一核對，
      // 不在白名單裡的才需要韌體跑完整探測換一種 auth 設定：
      //   202 AUTH_FAIL                          （密碼錯／加密模式被換掉）
      //   210 NO_AP_FOUND_W_COMPATIBLE_SECURITY
      //   211 NO_AP_FOUND_IN_AUTHMODE_THRESHOLD  （被我們自己設的 threshold.authmode 擋掉）
      //   212 NO_AP_FOUND_IN_RSSI_THRESHOLD
      //
      // 【這裡曾經寫錯，不要再犯】
      // 第一版把 15 (4WAY_HANDSHAKE_TIMEOUT) 列進來，還在註釋裡宣稱「15 不在白名單」。
      // 事實相反：STA.cpp:62 明列 WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT → return true，
      // core 會自己重試。而 15 正是訊號邊緣最常見的斷線原因之一，把它算成 authFailure
      // 會讓設備每次弱訊號斷線都立刻進入阻塞式完整探測——正好是這整段重寫要消滅的行為。
      // 200 (BEACON_TIMEOUT) 與 201 (NO_AP_FOUND) 同樣都在白名單裡，也不可列入。
      uint8_t reasonNow = lastWifiDisconnectReason;
      bool authFailure = (reasonNow == 202 || reasonNow == 210 ||
                          reasonNow == 211 || reasonNow == 212);

      // nextFullProbeAt == 0 代表「從未設定過」，直接放行；
      // 之後一律用 wrap-safe 的有號數比較，49.7 天溢位時仍然正確。
      bool probeAllowed = (nextFullProbeAt == 0) || ((long)(now - nextFullProbeAt) >= 0);

      if ((authFailure || downFor >= WIFI_FULL_PROBE_AFTER_MS) && probeAllowed) {
        Serial.printf("WiFi 已斷線 %lu 秒%s，執行完整探測\n",
                      downFor / 1000,
                      authFailure ? "（認證類失敗，core 不會自動重連）" : "");
        connectToWiFi();

        // 時間戳一律在阻塞呼叫「之後」用新的 millis() 取。
        // 沿用阻塞前的 now 會讓所有節流從舊時間起算，等於節流不存在——
        // 這正是舊版「每 5 秒檢查一次」在失敗時變成背靠背連續重試的原因。
        unsigned long after = millis();
        nextFullProbeAt = after + WIFI_FULL_PROBE_COOLDOWN_MS;
        if (nextFullProbeAt == 0) nextFullProbeAt = 1;  // 0 是哨兵值，避開它
        lastWiFiCheck = after;
        lastWifiKick = after;
      } else if (now - lastWifiKick >= WIFI_KICK_INTERVAL_MS) {
        // 非阻塞補刀。esp_wifi_connect() 只是把請求丟給 WiFi driver，不等待、
        // 不佔 loop()。若 core 的 auto-reconnect 正在飛，這裡會拿到
        // ESP_ERR_WIFI_CONN，那是正常的，照實印出來即可。
        lastWifiKick = now;
        esp_err_t kickErr = esp_wifi_connect();
        Serial.printf("WiFi 補送 esp_wifi_connect() → %s（已斷線 %lu 秒）\n",
                      kickErr == ESP_OK ? "已送出" : esp_err_to_name(kickErr),
                      downFor / 1000);
      }
    }
  }

  // ══ MQTT 連線管理 ══
  //
  // 舊版每次重連呼叫 smartConnect()，一趟把五台 broker 全試完、最壞 90 秒以上，
  // 而 lastReconnectAttempt = now 又設在那個阻塞呼叫「之前」，回來時 10 秒閘門
  // 必定已過 → 「每 10 秒重連一次」實際上是背靠背連續重試，中間沒有喘息。
  // 現在改成 smartConnectStep()：每次只試一台，把成本攤平到 loop 的節奏上。
  // WiFi 區段可能剛剛跑完一次最壞 61 秒的完整探測，上面那個 now（:953 取樣）
  // 已經過期。本段所有節流都改用重新取樣的 nowMqtt——這正是這輪重寫自己立下的
  // 「時間戳一律在阻塞呼叫之後取」原則，漏在自己身上就是白立。
  unsigned long nowMqtt = millis();

  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      // nextMqttAttemptAt == 0 代表「從未設定過」，直接放行。
      // 少了這個哨兵，開機超過 24.8 天後 (long)(now - 0) 會是負數，
      // 條件永遠不成立 → MQTT 斷線後再也不重連。
      bool mqttAttemptAllowed =
          (nextMqttAttemptAt == 0) || ((long)(nowMqtt - nextMqttAttemptAt) >= 0);
      if (mqttAttemptAllowed) {
        smartConnectStep();
        // 時間戳在阻塞呼叫「之後」才取，理由同 WiFi 區段。
        unsigned long afterMqtt = millis();
        nextMqttAttemptAt = afterMqtt + MQTT_RETRY_INTERVAL_MS;
        if (nextMqttAttemptAt == 0) nextMqttAttemptAt = 1;  // 0 是哨兵值，避開它
      }
    } else {
      mqttClient.loop();

      // 每 3 秒發送一次保持連線的狀態更新（帶伺服器資訊）
      if (nowMqtt - lastKeepAlive > 3000) {
        // 用連線當下記下的實際位址，不要從 useCustomServer 反推（見其宣告處的說明）
        const char* server = activeMqttServer ? activeMqttServer
                                              : DEFAULT_SERVERS[currentServerIndex].server;
        publishStatusWithServer(server);
        // publish() 內部走 NetworkClient::write，retry 上限 10 次、每輪 select 1 秒。
        // 注意 10 秒是「連續無進度」的上界，不是「單次呼叫」的上界——
        // NetworkClient.cpp:431 在有部分寫入時會把 retry 重設回 10，
        // 所以理論上單次呼叫沒有硬上界，只是對 ~250 bytes 的狀態封包實務上不會發生。
        // 時間戳仍然要在呼叫之後取，否則下一次 publish 會立刻觸發而非 3 秒後。
        lastKeepAlive = millis();
      }
    }
  }
}

void connectToWiFi() {
  // 檢查當前狀態
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi 已連接，跳過重連");
    return;
  }

  // 徹底重置 WiFi 狀態（ESP32-C3 需要完整重置才能可靠連線）
  // 注意：第一個參數是 wifioff（關掉射頻），不是 eraseap。
  // 簽章為 disconnect(bool wifioff = false, bool eraseap = false, unsigned long timeoutLength = 100)。
  // 舊註釋寫「true = 清除之前的 AP 配置」是錯的——那是第二個參數的語義。
  // 這裡刻意要的就是完整重置射頻（本函式是「完整探測」，不是一般重連路徑）；
  // 一般重連請走 loop() 的非阻塞路徑，不要呼叫本函式。
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_OFF);
  delay(200);
  WiFi.mode(WIFI_STA);
  delay(200);

  // 先掃描確認目標 SSID 是否存在，並取得加密類型
  Serial.println("掃描附近 WiFi 網路...");
  // core 的 WiFiScanClass::_scanTimeout 預設是 60000ms，而本專案從未呼叫過
  // setScanTimeout()。掃描卡住時整個 loop() 會停擺一分鐘，遠超過 broker 的
  // 45 秒踢人門檻，必須把上限壓下來。
  WiFi.setScanTimeout(WIFI_SCAN_TIMEOUT_MS);
  int n = WiFi.scanNetworks();
  bool ssidFound = false;
  wifi_auth_mode_t authMode = WIFI_AUTH_WPA2_PSK;
  int8_t targetRSSI = -100;
  for (int i = 0; i < n; i++) {
    Serial.printf("  [%d] %s (%d dBm) 加密: %d ch: %d\n", i,
                  WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                  WiFi.encryptionType(i), WiFi.channel(i));
    if (WiFi.SSID(i) == String(ssid)) {
      ssidFound = true;
      authMode = WiFi.encryptionType(i);
      targetRSSI = WiFi.RSSI(i);
    }
  }
  if (!ssidFound) {
    Serial.printf("⚠ 掃描結果中找不到 SSID: %s\n", ssid);
    Serial.println("  → 請確認路由器已開啟且在 2.4GHz 頻段");
  } else {
    Serial.printf("✓ 找到目標 SSID: %s (RSSI: %d dBm, 加密類型: %d)\n", ssid, targetRSSI, authMode);
    // 顯示加密類型名稱
    switch(authMode) {
      case WIFI_AUTH_OPEN: Serial.println("  加密: 開放(無加密)"); break;
      case WIFI_AUTH_WEP: Serial.println("  加密: WEP"); break;
      case WIFI_AUTH_WPA_PSK: Serial.println("  加密: WPA-PSK"); break;
      case WIFI_AUTH_WPA2_PSK: Serial.println("  加密: WPA2-PSK"); break;
      case WIFI_AUTH_WPA_WPA2_PSK: Serial.println("  加密: WPA/WPA2-PSK"); break;
      case WIFI_AUTH_WPA3_PSK: Serial.println("  加密: WPA3-PSK"); break;
      case WIFI_AUTH_WPA2_WPA3_PSK: Serial.println("  加密: WPA2/WPA3-PSK"); break;
      default: Serial.printf("  加密: 未知(%d)\n", authMode); break;
    }
  }
  WiFi.scanDelete();

  // 掃描後重新設定 STA 模式
  WiFi.mode(WIFI_STA);
  delay(100);

  // 開始連接
  Serial.printf("正在連接 WiFi: %s (密碼長度: %d)\n", ssid, strlen(password));

  // 自動嘗試多種安全模式連線（處理各種路由器設定）
  struct WiFiAuthConfig {
    wifi_auth_mode_t authmode;
    bool pmf_capable;
    bool pmf_required;
    const char* desc;
  };

  // 依序嘗試不同安全設定
  WiFiAuthConfig authConfigs[] = {
    { WIFI_AUTH_WPA_WPA2_PSK, true, false, "WPA/WPA2 + PMF capable" },
    { WIFI_AUTH_WPA2_PSK, true, false, "WPA2 + PMF capable" },
    { WIFI_AUTH_WPA2_WPA3_PSK, true, false, "WPA2/WPA3 + PMF capable" },
    { WIFI_AUTH_WPA_WPA2_PSK, false, false, "WPA/WPA2 無 PMF" },
    { WIFI_AUTH_OPEN, false, false, "開放模式（最低門檻）" },
  };
  const int numConfigs = sizeof(authConfigs) / sizeof(authConfigs[0]);

  bool connected = false;
  // 收到 201（掃描不到 AP）時整輪放棄：換哪一種 auth 設定都一樣連不上。
  //
  // 【它擋不住什麼】只在原因碼恰為 201 時生效。最壞路徑——202、211、
  // 或等滿 20×500ms 都沒收到任何事件——它完全不生效，本函式的最壞值
  // 仍然是「掃描 8 秒 + 5 種模式各 10.4 秒 ≈ 61 秒」。
  // 真正把最壞值從 113 秒砍到 61 秒的是上面那行 setScanTimeout()，不是這個旗標。
  bool abortAllModes = false;

  for (int cfgIdx = 0; cfgIdx < numConfigs && !connected && !abortAllModes; cfgIdx++) {
    WiFiAuthConfig& cfg = authConfigs[cfgIdx];
    Serial.printf("\n嘗試第 %d/%d 種安全模式: %s\n", cfgIdx + 1, numConfigs, cfg.desc);

    // 重置連線狀態
    lastWifiDisconnectReason = 0;
    WiFi.disconnect(true);
    delay(200);
    WiFi.mode(WIFI_STA);
    delay(100);

    wifi_config_t wifi_cfg = {};
    memcpy(wifi_cfg.sta.ssid, ssid, min(strlen(ssid), sizeof(wifi_cfg.sta.ssid)));
    memcpy(wifi_cfg.sta.password, password, min(strlen(password), sizeof(wifi_cfg.sta.password)));
    wifi_cfg.sta.threshold.authmode = cfg.authmode;
    wifi_cfg.sta.pmf_cfg.capable = cfg.pmf_capable;
    wifi_cfg.sta.pmf_cfg.required = cfg.pmf_required;
    wifi_cfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    esp_err_t err;
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (err != ESP_OK) {
      Serial.printf("esp_wifi_set_config 失敗: %d\n", err);
      continue;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
      Serial.printf("esp_wifi_connect 失敗: %d\n", err);
      continue;
    }

    int attempts = 0;
    const int maxAttempts = 20;  // 每種模式等待 10 秒

    while (WiFi.status() != WL_CONNECTED && attempts < maxAttempts) {
      blinkLED();
      delay(500);

      // 在等待 WiFi 連線期間檢查重置按鈕
      if (anyResetButtonPressed()) {
        Serial.println("偵測到按鈕按下（WiFi 連線等待中）...");
        waitForResetConfirm();  // 確認成功會直接重啟，返回代表已取消
      }

      if (attempts % 4 == 0) {
        Serial.print(".");
      }

      // ── 早退：拿到明確的失敗原因就不必把 10 秒等滿 ──
      //
      // 舊版只認 202(AUTH_FAIL) 與 15(4WAY_HANDSHAKE_TIMEOUT)，漏掉了實際上最常
      // 出現的幾個（值取自 core 的 esp_wifi_types_generic.h）：
      //   200 BEACON_TIMEOUT
      //   201 NO_AP_FOUND
      //   210 NO_AP_FOUND_W_COMPATIBLE_SECURITY
      //   211 NO_AP_FOUND_IN_AUTHMODE_THRESHOLD  ← 我們自己設的 threshold.authmode 擋掉的
      //   212 NO_AP_FOUND_IN_RSSI_THRESHOLD
      // 漏掉的後果是每種模式都把 20×500ms 等滿，五種共 52 秒。
      // 「AP 剛回來卻要一分鐘才連上」主要就是這樣來的。
      uint8_t reason = lastWifiDisconnectReason;
      if (reason == 201) {
        // 201 NO_AP_FOUND：掃描階段就找不到這個 SSID，換哪一種 auth 設定都一樣，
        // 整輪直接放棄，交回 loop() 讓 core 的 auto-reconnect 繼續在背景試。
        //
        // 【不要把 200 加進來】200 是 BEACON_TIMEOUT，語義是「曾經關聯上的 AP
        // 連續數個 beacon interval 沒聽到」——AP 重開機、瞬間干擾、短暫離開範圍
        // 都會產生它，是 transient 的。core 自己把 200 放進重連白名單（STA.cpp:77），
        // 且只把它映到 WL_CONNECTION_LOST 而非 WL_NO_SSID_AVAIL。
        // 第一版把 200 當成「AP 不在」整輪放棄，會在 AP 其實還在的時候提早收手。
        Serial.printf("\n掃描不到 AP (原因碼: %d)，本輪不再嘗試其他模式\n", reason);
        abortAllModes = true;
        break;
      }
      // 【這份名單與 loop() 的 authFailure 用途不同，不要合併】
      //   * loop() 的 authFailure 問的是「core 會不會自己重連」——15 在 core 的
      //     白名單裡，所以**不可**列入那邊。
      //   * 這裡問的是「這一種 auth 設定還有沒有希望」——我們已經在跑完整探測了，
      //     收到 15（握手逾時）代表這組設定談不攏，換下一種是對的，不必等滿 10 秒。
      // 同一個原因碼在兩個問題下有不同答案，這不是矛盾。
      if (reason == 202 || reason == 15 || reason == 210 || reason == 211 || reason == 212) {
        Serial.printf("\n此模式被拒 (原因碼: %d)，切換下一種模式...\n", reason);
        break;
      }

      attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
      connected = true;
      Serial.printf("\n成功！使用模式: %s\n", cfg.desc);
    } else {
      Serial.printf("\n模式 [%s] 失敗，斷線原因碼: %d\n", cfg.desc, lastWifiDisconnectReason);
    }
  }

  // ══ 收尾：還原驅動層設定 ══
  //
  // 這兩件事漏掉任何一件，都會讓「之後的非阻塞重連」永遠連不上。
  //
  // (1) auth config 還原
  //     本函式用 esp_wifi_set_config() 逐一套用五種設定，全部失敗時驅動裡留下的是
  //     最後一組——WIFI_AUTH_OPEN 且 pmf_cfg.capable = false。
  //     而之後 loop() 的 esp_wifi_connect() 補刀、以及 core 的 auto-reconnect
  //     （STAClass::connect() 是 esp_wifi_get_config() 再原樣寫回，STA.cpp:337）
  //     都沿用那份設定。對 WPA3 或開了 PMF required 的 WPA2 AP，那份設定永遠連不上，
  //     序列埠會印滿「WiFi 補送 esp_wifi_connect() → 已送出」卻一直不通。
  //     舊版每 5 秒重跑本函式，等於每 5 秒把模式 1 重新套上一次（隱性還原）；
  //     新版呼叫頻率大幅降低，這個還原必須顯式做。
  //
  // (2) setSleep(false) 與 setTxPower() 重套
  //     這兩者是寫進驅動層的（esp_wifi_set_ps() / esp_wifi_set_max_tx_power()），
  //     而本函式開頭的 WiFi.disconnect(true) 一路走到 esp_wifi_deinit()，
  //     重新初始化的 wifiLowLevelInit() 通篇沒有重套它們。
  //     不補這一段，第一次完整探測之後設備就靜默回到預設 modem sleep 與預設發射功率
  //     ——而最容易觸發完整探測的正是訊號邊緣，等於保護在最需要它的場景下被拆掉。
  //     （setAutoReconnect 不必重套：它寫的是 STAClass::_autoReconnect，
  //       是全域 WiFi.STA 物件的成員，deinit 後仍然存活。）
  if (!connected) {
    wifi_config_t restoreCfg = {};
    memcpy(restoreCfg.sta.ssid, ssid, min(strlen(ssid), sizeof(restoreCfg.sta.ssid)));
    memcpy(restoreCfg.sta.password, password, min(strlen(password), sizeof(restoreCfg.sta.password)));
    restoreCfg.sta.threshold.authmode = authConfigs[0].authmode;
    restoreCfg.sta.pmf_cfg.capable = authConfigs[0].pmf_capable;
    restoreCfg.sta.pmf_cfg.required = authConfigs[0].pmf_required;
    restoreCfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    if (esp_wifi_set_config(WIFI_IF_STA, &restoreCfg) == ESP_OK) {
      Serial.printf("已還原 WiFi 設定為模式 [%s]，供後續非阻塞重連使用\n",
                    authConfigs[0].desc);
    } else {
      Serial.println("⚠ WiFi 設定還原失敗，後續重連可能沿用最低門檻的設定");
    }
  }

  WiFi.setSleep(false);                  // esp_wifi_set_ps(WIFI_PS_NONE)，deinit 後會失效
  WiFi.setTxPower(WIFI_POWER_19_5dBm);   // esp_wifi_set_max_tx_power()，同上

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi 連接成功！");
    Serial.print("IP 位址: ");
    Serial.println(WiFi.localIP());
    Serial.print("訊號強度: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
    Serial.print("MAC 位址: ");
    Serial.println(WiFi.macAddress());

    // WiFi 連接成功後發布狀態
    if (mqttClient.connected()) {
      publishStatus();
    }
  } else {
    Serial.println("\n✗ 無法連接到 WiFi");
    Serial.printf("最後狀態碼: %d\n", WiFi.status());
    Serial.printf("底層斷線原因碼: %d\n", lastWifiDisconnectReason);

    wl_status_t finalStatus = WiFi.status();
    Serial.println("診斷資訊：");

    switch(finalStatus) {
      case WL_NO_SSID_AVAIL:
        Serial.println("❌ 找不到指定的 SSID");
        Serial.println("  → 請確認 SSID 名稱正確");
        Serial.println("  → 確認路由器已開啟且在訊號範圍內");
        break;
      case WL_CONNECT_FAILED:
        Serial.println("❌ 連接失敗（可能是密碼錯誤）");
        Serial.println("  → 請檢查 WiFi 密碼");
        Serial.println("  → 確認使用 2.4GHz 頻段（不支援 5GHz）");
        break;
      case WL_CONNECTION_LOST:
        Serial.println("❌ 連接中斷");
        Serial.println("  → 訊號可能太弱");
        Serial.println("  → 路由器可能不穩定");
        break;
      default:
        Serial.println("其他可能原因：");
        Serial.println("1. SSID 或密碼錯誤");
        Serial.println("2. 訊號太弱（嘗試靠近路由器）");
        Serial.println("3. 路由器限制連接數（嘗試重啟路由器）");
        Serial.println("4. WiFi 頻段不支援（僅支援 2.4GHz）");
        Serial.println("5. MAC 過濾已啟用（請將設備加入白名單）");
        break;
    }
  }
}

void publishStatus() {
  if (!mqttClient.connected()) return;

  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";
  
  StaticJsonDocument<1024> doc;  // 將大小從 512 增加到 1024
  
  // 基本資訊
  doc["device_id"] = deviceId;
  doc["status"] = isUpdating ? "updating" : "online";
  doc["version"] = firmwareVersion;
  doc["model"] = deviceModel;
  doc["timestamp"] = millis() / 1000;
  
  // WiFi 資訊
  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["connected"] = (WiFi.status() == WL_CONNECTED);
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();
  
  // 設備狀態
  JsonObject device = doc.createNestedObject("device");
  device["relay"] = relayState ? 1 : 0;
  // device["free_heap"] = ESP.getFreeHeap();
  
  if (isUpdating) {
    device["update_progress"] = updateProgress;
  }
  
  char buffer[1024];  // 將緩衝區大小也增加到 1024
  
  // 計算序列化後的大小
  size_t jsonSize = measureJson(doc);
  Serial.print("JSON 大小: ");
  Serial.print(jsonSize);
  Serial.println(" bytes");
  
  if (jsonSize > sizeof(buffer)) {
    Serial.println("警告：JSON 太大，無法放入緩衝區");
    return;
  }
  
  serializeJson(doc, buffer);
  
  bool publishSuccess = mqttClient.publish(statusTopic.c_str(), buffer, true);
  
  Serial.print("發布狀態: ");
  Serial.println(buffer);
  Serial.print("MQTT 伺服器: ");
  Serial.println(mqttServer);
  Serial.print("發布狀態: ");
  Serial.println(publishSuccess ? "成功" : "失敗");
  
  if (!publishSuccess) {
    Serial.println("發布失敗原因可能是：");
    Serial.println("1. 網路連接不穩定");
    Serial.println("2. MQTT 伺服器無回應");
    Serial.println("3. 訊息太大 (目前大小: " + String(jsonSize) + " bytes)");
    Serial.println("4. 連接已斷開");
  }
}

// 發布帶有伺服器資訊的狀態
void publishStatusWithServer(const char* server) {
  if (!mqttClient.connected()) return;

  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";

  StaticJsonDocument<1024> doc;
  doc["device_id"] = deviceId;
  doc["status"] = isUpdating ? "updating" : "online";
  doc["version"] = firmwareVersion;
  doc["model"] = deviceModel;
  doc["server"] = server;  // 加入伺服器資訊
  doc["timestamp"] = millis() / 1000;

  // WiFi 資訊
  JsonObject wifi = doc.createNestedObject("wifi");
  wifi["connected"] = (WiFi.status() == WL_CONNECTED);
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();

  // 設備狀態
  JsonObject device = doc.createNestedObject("device");
  device["relay"] = relayState ? 1 : 0;

  if (isUpdating) {
    device["update_progress"] = updateProgress;
  }

  char buffer[1024];
  serializeJson(doc, buffer);
  // 一定要看 publish() 的回傳值：封包超過 PubSubClient 緩衝區時它只會靜默失敗，
  // 從前這裡不管成敗都印「已發布狀態」，害人以為訊息有送到 broker
  const bool publishSuccess = mqttClient.publish(statusTopic.c_str(), buffer, true);

  Serial.printf("已發布狀態 (%s, 伺服器: %s) - %s\n",
                deviceId, server, publishSuccess ? "成功" : "失敗");
  if (!publishSuccess) {
    Serial.printf("  ⚠ 發布失敗，JSON %u bytes + topic %u bytes，檢查 setBufferSize()\n",
                  (unsigned)measureJson(doc), (unsigned)statusTopic.length());
  }
}

// 發布伺服器切換事件
void publishServerChangeEvent(const char* switchType, const char* server) {
  if (!mqttClient.connected()) return;

  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";

  StaticJsonDocument<256> doc;
  doc["device_id"] = deviceId;
  doc["status"] = "online";
  doc["event"] = "server_changed";
  doc["switch_type"] = switchType;  // "auto" 或 "custom"
  doc["server"] = server;
  doc["timestamp"] = millis() / 1000;

  char buffer[256];
  serializeJson(doc, buffer);
  mqttClient.publish(statusTopic.c_str(), buffer, true);

  Serial.printf("已發布伺服器切換事件: %s (%s)\n", server, switchType);
}

// 快速連接指定索引的預設伺服器
bool quickConnectToIndex(int index) {
  if (index < 0 || index >= DEFAULT_SERVER_COUNT) return false;

  const MqttServerConfig& cfg = DEFAULT_SERVERS[index];
  Serial.printf("快速測試預設伺服器 [%d]: %s:%d ... ", index, cfg.server, cfg.port);

  mqttClient.setServer(cfg.server, cfg.port);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);
  // PubSubClient 預設封包上限只有 256 bytes（MQTT_MAX_PACKET_SIZE），而狀態 JSON
  // 約 200 bytes 加上 topic 35 bytes 就已逼近上限——publish() 會靜默回傳 false，
  // 序列埠卻照印「已發布狀態」。2026-08-16 實測：訂閱 45 秒只收到 3 則，而不是
  // 每 3 秒一則。放大到 512 才夠這份 JSON 用。
  mqttClient.setBufferSize(512);

  unsigned long startTime = millis();
  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";

  // 設定離線訊息
  StaticJsonDocument<128> offlineDoc;
  offlineDoc["device_id"] = deviceId;
  offlineDoc["status"] = "offline";
  offlineDoc["server"] = cfg.server;
  offlineDoc["timestamp"] = millis() / 1000;

  char offlineBuffer[128];
  serializeJson(offlineDoc, offlineBuffer);

  // 嘗試連接（1秒超時）
  if (mqttClient.connect(deviceId,
                        cfg.username,
                        cfg.password,
                        statusTopic.c_str(), 1, true,
                        offlineBuffer, true)) {
    unsigned long connectTime = millis() - startTime;

    // 連上就採用。**不可以**因為「太慢」把已經建立好的連線丟掉。
    //
    // 舊寫法是 `if (connectTime < 1000) {...} else { disconnect(); return false; }`，
    // 註釋自稱「1秒超時」，但它根本不是超時——DNS、TCP 握手、等 CONNACK 的成本全部
    // 已經付完、連線也真的建立起來了，才因為碼錶超過 1 秒而主動把它斷掉。
    // 台灣連海外公共 broker 光 RTT 就 150~300ms，DNS＋握手＋CONNACK 幾個往返輕易破
    // 1 秒，於是五台預設伺服器會**全部**被判「太慢」，而 smartConnect() 沒有任何
    // fallback（沒有「都太慢就挑最快的那台」），結果是：每一台都連得上、設備卻永遠
    // 離線，序列埠只留下一串「太慢 ✗」。
    //
    // 「優先用快的伺服器」這個目的由 currentServerIndex 達成（下次從上次成功的那台
    // 開始試），不需要靠丟棄連線來達成。
    Serial.printf("成功 (%lu ms) ✓\n", connectTime);
    if (connectTime >= MQTT_SLOW_CONNECT_WARN_MS) {
      Serial.printf("  ⚠ 連線耗時偏長 (%lu ms)，仍然採用\n", connectTime);
    }

    // 訂閱控制主題
    String controlTopic = String("hoban/") + deviceId + "/control";
    bool subscribeSuccess = mqttClient.subscribe(controlTopic.c_str());
    Serial.printf("訂閱控制主題: %s - %s\n",
                  controlTopic.c_str(),
                  subscribeSuccess ? "成功" : "失敗");

    // 同時訂閱舊版（MAC 反序）控制主題，讓尚未更新的 App 仍能控制設備
    String legacyControlTopic = String("hoban/") + getLegacyDeviceId() + "/control";
    bool legacySubscribeSuccess = mqttClient.subscribe(legacyControlTopic.c_str());
    Serial.printf("訂閱舊版控制主題: %s - %s\n",
                  legacyControlTopic.c_str(),
                  legacySubscribeSuccess ? "成功" : "失敗");

    // 發布上線狀態（包含伺服器資訊）
    activeMqttServer = cfg.server;
    publishStatusWithServer(cfg.server);
    currentServerIndex = index;

    return true;
  }

  Serial.println("失敗 ✗");
  return false;
}

// 快速連接自訂伺服器（使用全域變數中的設定）
bool quickConnectCustom() {
  Serial.printf("快速測試自訂伺服器: %s:%d ... ", mqttServer, mqttPort);

  mqttClient.setServer(mqttServer, mqttPort);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(30);
  // PubSubClient 預設封包上限只有 256 bytes（MQTT_MAX_PACKET_SIZE），而狀態 JSON
  // 約 200 bytes 加上 topic 35 bytes 就已逼近上限——publish() 會靜默回傳 false，
  // 序列埠卻照印「已發布狀態」。2026-08-16 實測：訂閱 45 秒只收到 3 則，而不是
  // 每 3 秒一則。放大到 512 才夠這份 JSON 用。
  mqttClient.setBufferSize(512);

  unsigned long startTime = millis();
  const char* deviceId = getDeviceId();
  String statusTopic = String("hoban/") + deviceId + "/status";

  // 設定離線訊息
  StaticJsonDocument<128> offlineDoc;
  offlineDoc["device_id"] = deviceId;
  offlineDoc["status"] = "offline";
  offlineDoc["server"] = mqttServer;
  offlineDoc["timestamp"] = millis() / 1000;

  char offlineBuffer[128];
  serializeJson(offlineDoc, offlineBuffer);

  // 使用自訂伺服器的帳密（如果為空字串則傳 NULL）
  const char* username = (strlen(mqttUsername) > 0) ? mqttUsername : NULL;
  const char* password = (strlen(mqttPassword) > 0) ? mqttPassword : NULL;

  // 嘗試連接（1秒超時）
  if (mqttClient.connect(deviceId,
                        username,
                        password,
                        statusTopic.c_str(), 1, true,
                        offlineBuffer, true)) {
    unsigned long connectTime = millis() - startTime;

    // 連上就採用，理由與 quickConnectToIndex() 相同（見該函式的長註釋）：
    // 舊寫法把「已經連上、成本已付完」的連線因為超過 1 秒而丟掉，在稍慢的網路上
    // 會讓設備連得上卻永遠離線。
    Serial.printf("成功 (%lu ms) ✓\n", connectTime);
    if (connectTime >= MQTT_SLOW_CONNECT_WARN_MS) {
      Serial.printf("  ⚠ 連線耗時偏長 (%lu ms)，仍然採用\n", connectTime);
    }

    // 訂閱控制主題
    String controlTopic = String("hoban/") + deviceId + "/control";
    bool subscribeSuccess = mqttClient.subscribe(controlTopic.c_str());
    Serial.printf("訂閱控制主題: %s - %s\n",
                  controlTopic.c_str(),
                  subscribeSuccess ? "成功" : "失敗");

    // 同時訂閱舊版（MAC 反序）控制主題，讓尚未更新的 App 仍能控制設備
    String legacyControlTopic = String("hoban/") + getLegacyDeviceId() + "/control";
    bool legacySubscribeSuccess = mqttClient.subscribe(legacyControlTopic.c_str());
    Serial.printf("訂閱舊版控制主題: %s - %s\n",
                  legacyControlTopic.c_str(),
                  legacySubscribeSuccess ? "成功" : "失敗");

    // 發布上線狀態（包含伺服器資訊）
    activeMqttServer = mqttServer;
    publishStatusWithServer(mqttServer);

    return true;
  }

  Serial.println("失敗 ✗");
  return false;
}

// 智慧連接：按優先順序嘗試所有伺服器
void smartConnect() {
  Serial.println("=== 開始智慧連接 ===");

  // 1. 如果有自訂伺服器，先試自訂
  if (useCustomServer && strlen(mqttServer) > 0) {
    Serial.println("優先嘗試自訂伺服器...");
    if (quickConnectCustom()) {
      Serial.println("✓ 已連接到自訂伺服器");
      publishServerChangeEvent("custom", mqttServer);
      return;
    }
    Serial.println("自訂伺服器失敗，嘗試預設伺服器清單");
  }

  // 2. 從上次成功的伺服器開始，輪流嘗試所有預設伺服器
  //
  // 本函式只在 setup() 被呼叫（運行期一律走 smartConnectStep()，一次只試一台）。
  // 五台全掛時最壞阻塞約 90 秒，期間 loop() 完全不跑，長按重置也就沒人理——
  // 而「WiFi 通、broker 全掛」正是長按重置作為唯一復原手段的情境之一。
  // 所以每試完一台就給按鈕一次機會。
  //
  // 【它擋不住什麼】quickConnectToIndex() 內部單台最壞約 18 秒
  //（不受管的 DNS + 3 秒 TCP + 15 秒等 CONNACK 的 busy-wait，且那個迴圈沒有 yield()），
  // 這道輪詢插不進去。使用者最久仍需按住約 18 秒才會被看見，只是不再是 90 秒。
  for (int i = 0; i < DEFAULT_SERVER_COUNT; i++) {
    if (anyResetButtonPressed()) {
      Serial.println("偵測到按鈕按下（MQTT 連線等待中）...");
      waitForResetConfirm();  // 確認成功會直接重啟，返回代表已取消
    }

    int index = (currentServerIndex + i) % DEFAULT_SERVER_COUNT;
    if (quickConnectToIndex(index)) {
      Serial.printf("✓ 已連接到預設伺服器 [%d]: %s\n", index, DEFAULT_SERVERS[index].server);
      publishServerChangeEvent("default", DEFAULT_SERVERS[index].server);
      return;
    }
    delay(500);  // 短暫等待後嘗試下一個
  }

  Serial.println("✗ 所有伺服器連接失敗");
  // 下次重試從下一個伺服器開始
  currentServerIndex = (currentServerIndex + 1) % DEFAULT_SERVER_COUNT;
}

// ══ 每次只嘗試一台 broker 的重連步進器 ══
//
// 舊的 smartConnect() 是一個 for 迴圈，一次呼叫就把自訂伺服器＋五台預設伺服器
// 全部試完。單台失敗的成本是：不受任何 caller timeout 管的 DNS 解析
//（NetworkClient::connect() 先做 Network.hostByName() 才把 timeout 傳下去）
// ＋ 3 秒 TCP connect（WIFI_CLIENT_DEF_CONN_TIMEOUT_MS）
// ＋ 15 秒等 CONNACK 的 busy-wait（MQTT_SOCKET_TIMEOUT，而且那個 while 迴圈裡
//   沒有 yield()）。五台加起來單次呼叫最壞 90 秒以上，期間 loop() 完全停擺。
//
// 改成每次呼叫只試一台，把「試完五台」攤平到 loop 的 MQTT_RETRY_INTERVAL_MS 節奏上。
// 順帶修掉：舊版 currentServerIndex 只在連線成功時才更新，所以連續重連會一直打同一台
// 剛剛才失敗的伺服器，白燒 18 秒以上才輪到下一台。
static int mqttProbeOffset = 0;       // 這一輪已經試過幾台預設伺服器
static bool mqttCustomTried = false;  // 這一輪是否已經試過自訂伺服器

void resetMqttProbe() {
  mqttProbeOffset = 0;
  mqttCustomTried = false;
}

void smartConnectStep() {
  // 1. 有自訂伺服器時，這一輪優先試它一次
  if (useCustomServer && strlen(mqttServer) > 0 && !mqttCustomTried) {
    mqttCustomTried = true;
    Serial.println("MQTT 重連：嘗試自訂伺服器");
    if (quickConnectCustom()) {
      Serial.println("✓ 已連接到自訂伺服器");
      publishServerChangeEvent("custom", mqttServer);
      resetMqttProbe();
    }
    return;  // 本次呼叫只試這一台，其餘交給下一輪
  }

  // 2. 從上次成功的伺服器開始，每次往後移一台
  int index = (currentServerIndex + mqttProbeOffset) % DEFAULT_SERVER_COUNT;
  mqttProbeOffset++;
  Serial.printf("MQTT 重連：嘗試預設伺服器 [%d] %s ... ", index, DEFAULT_SERVERS[index].server);
  if (quickConnectToIndex(index)) {
    publishServerChangeEvent("default", DEFAULT_SERVERS[index].server);
    resetMqttProbe();
    return;
  }

  // 3. 本輪五台都試過仍失敗：換一台當起點，重新開始下一輪
  if (mqttProbeOffset >= DEFAULT_SERVER_COUNT) {
    currentServerIndex = (currentServerIndex + 1) % DEFAULT_SERVER_COUNT;
    Serial.printf("本輪所有伺服器皆失敗，下一輪改從 [%d] %s 開始\n",
                  currentServerIndex, DEFAULT_SERVERS[currentServerIndex].server);
    resetMqttProbe();
  }
}

// 保留原有的 connectToMQTT 函數作為向後兼容（現在內部使用 smartConnect）
void connectToMQTT() {
  smartConnect();
}

// 韌體下載和更新函數（透過 MQTT 觸發）
void startFirmwareUpdate(const char* downloadUrl) {
  if (isUpdating) {
    Serial.println("更新已在進行中，無法開始新的更新");
    return;
  }
  
  Serial.println("=== 開始韌體下載更新 ===");
  Serial.printf("下載網址：%s\n", downloadUrl);
  Serial.printf("可用空間：%u bytes\n", ESP.getFreeSketchSpace());
  Serial.printf("當前韌體版本：%s\n", firmwareVersion);

  isUpdating = true;
  updateProgress = 0;
  // LED 開始快閃（更新模式）
  digitalWrite(ledOnFace, LOW);
  digitalWrite(ledOnBoard, LOW);
  
  // 發送更新開始狀態到 MQTT
  if (mqttClient.connected()) {
    String deviceId = getDeviceId();
    String statusTopic = "hoban/" + deviceId + "/status";
    mqttClient.publish(statusTopic.c_str(), "updating", true);
    Serial.println("已發送更新開始狀態到 MQTT");
  }
  
  // 使用 HTTPClient
  WiFiClientSecure client;
  HTTPClient http;
  
  // 設定 SSL/TLS
  client.setInsecure(); // 允許自簽名證書
  
  Serial.println("檢查網路狀態：");
  Serial.printf("WiFi SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("WiFi 訊號強度: %d dBm\n", WiFi.RSSI());
  Serial.printf("本地 IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("DNS 伺服器: %s\n", WiFi.dnsIP().toString().c_str());
  
  const int maxRetries = 3;
  const int baseDelay = 5000; // 基礎延遲 5 秒
  int retryCount = 0;
  bool downloadSuccess = false;
  String finalUrl = downloadUrl;
  
  while (retryCount < maxRetries && !downloadSuccess) {
    if (retryCount > 0) {
      int delayTime = baseDelay * (1 << retryCount); // 指數退避
      Serial.printf("重試第 %d 次，等待 %d 毫秒...\n", retryCount, delayTime);
      delay(delayTime);
    }
    
    Serial.println("正在連接到下載伺服器...");
    
    // 設定超時
    http.setTimeout(30000); // 30 秒超時
    
    if (http.begin(client, finalUrl)) {
      // 添加請求標頭
      http.addHeader("User-Agent", "ESP32-FirmwareUpdate/1.0");
      http.addHeader("Accept", "*/*");
      
      Serial.println("發送 GET 請求下載檔案...");
      int httpCode = http.GET();
      
      if (httpCode == HTTP_CODE_OK) {
        int contentLength = http.getSize();
        Serial.printf("檔案大小: %d bytes\n", contentLength);
        
        // 檢查空間是否足夠
        if (contentLength > ESP.getFreeSketchSpace()) {
          Serial.println("錯誤：空間不足");
          break;
        }
        
        if (!Update.begin(contentLength)) {
          Serial.printf("錯誤：無法開始更新，錯誤碼：%d\n", Update.getError());
          break;
        }
        
        WiFiClient* stream = http.getStreamPtr();
        size_t written = 0;
        uint8_t buff[1024] = { 0 };

        // 下載超時設定
        const unsigned long downloadTimeout = 300000; // 5 分鐘
        unsigned long startTime = millis();
        unsigned long lastProgressTime = startTime;
        unsigned long lastBlinkTime = startTime;
        bool ledBlinkState = false;

        while (http.connected() && (written < contentLength)) {
          size_t available = stream->available();
          if (available) {
            size_t bytesRead = stream->readBytes(buff, min(available, sizeof(buff)));
            size_t bytesWritten = Update.write(buff, bytesRead);
            if (bytesWritten > 0) {
              written += bytesWritten;
              updateProgress = (written * 100) / contentLength;

              if (millis() - lastProgressTime >= 1000) {
                Serial.printf("下載進度：%d%%（%u/%d bytes）\n", updateProgress, written, contentLength);
                lastProgressTime = millis();
              }
            }
          }

          // LED 快閃 (每 QUICK_BLINK 毫秒切換一次)
          if (millis() - lastBlinkTime >= QUICK_BLINK) {
            ledBlinkState = !ledBlinkState;
            digitalWrite(ledOnFace, ledBlinkState ? HIGH : LOW);
            digitalWrite(ledOnBoard, ledBlinkState ? HIGH : LOW);
            lastBlinkTime = millis();
          }

          // 檢查超時
          if (millis() - startTime > downloadTimeout) {
            Serial.println("錯誤：下載超時");
            break;
          }

          delay(1); // 避免看門狗重置
        }
        
        if (written == contentLength && Update.end(true)) {
          Serial.println("更新成功！準備重新啟動...");
          downloadSuccess = true;
          
          if (mqttClient.connected()) {
            String deviceId = getDeviceId();
            String statusTopic = "hoban/" + deviceId + "/status";
            mqttClient.publish(statusTopic.c_str(), "update_success", true);
          }
          
          delay(1000);
          ESP.restart();
          return;
        }
      } else if (httpCode == HTTP_CODE_FOUND || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        // 處理重定向
        String newUrl = http.getLocation();
        Serial.printf("收到重定向到新網址：%s\n", newUrl.c_str());
        finalUrl = newUrl;
        http.end();
        continue; // 使用新的 URL 重試
      } else {
        Serial.printf("GET 請求失敗，錯誤碼：%d\n", httpCode);
        Serial.printf("錯誤訊息：%s\n", http.errorToString(httpCode).c_str());
        
        if (httpCode == HTTPC_ERROR_CONNECTION_REFUSED) {
          Serial.println("伺服器拒絕連接");
        } else if (httpCode == HTTPC_ERROR_SEND_HEADER_FAILED) {
          Serial.println("發送請求標頭失敗");
        } else if (httpCode == HTTPC_ERROR_SEND_PAYLOAD_FAILED) {
          Serial.println("發送請求內容失敗");
        } else if (httpCode == HTTPC_ERROR_NOT_CONNECTED) {
          Serial.println("未連接到伺服器");
        } else if (httpCode == HTTPC_ERROR_CONNECTION_LOST) {
          Serial.println("連接丟失，可能原因：");
          Serial.println("1. GitHub 伺服器連接不穩定");
          Serial.println("2. SSL/TLS 握手失敗");
          Serial.println("3. 網路延遲過高");
          Serial.println("4. DNS 解析失敗");
        } else if (httpCode == HTTPC_ERROR_NO_HTTP_SERVER) {
          Serial.println("找不到 HTTP 伺服器");
        }
      }
    } else {
      Serial.println("無法初始化 HTTP 客戶端");
    }
    
    http.end();
    retryCount++;
  }
  
  // 更新失敗處理
  isUpdating = false;
  digitalWrite(ledOnFace, LOW);
  digitalWrite(ledOnBoard, LOW);
  
  if (mqttClient.connected()) {
    String deviceId = getDeviceId();
    String statusTopic = "hoban/" + deviceId + "/status";
    String errorMsg = "update_failed";
    mqttClient.publish(statusTopic.c_str(), errorMsg.c_str(), true);
    Serial.println("已發送更新失敗狀態到 MQTT");
  }
}