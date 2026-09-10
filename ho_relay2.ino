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

const char* firmwareVersion = "1.8.3"; // 當前韌體版本
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

// 設定已存、等待重啟的時間點；0 代表沒有待處理的重啟。
//
// 為什麼不能在 onWrite() 裡直接 ESP.restart()：esp32 core 3.x 的
// BLECharacteristic::handleGATTServerEvent()（ESP_GATTS_WRITE_EVT）是
// 「先呼叫 onWrite()，回來之後才 esp_ble_gatts_send_response()」。
// 在回調裡重開機，那個 ATT 寫入回應永遠送不出去，App 端的
// write(withoutResponse: false) 只會等到連線被重開機切斷 —— 設定明明已經
// 存進 NVS，App 卻顯示「與設備的藍牙連線已中斷」並放棄新增設備。
// 舊版 core 是先送回應再呼叫 onWrite，所以這個寫法以前不會出事。
//
// 擋不住什麼：這只保證「回應有機會送出」。App 若在這 2 秒內自己離開頁面、
// 或封包在空中掉了，一樣會走到斷線那條路，那一層靠 App 端的補救判定。
volatile unsigned long bleRestartAt = 0;


const char* deviceModel = "hoRelay2"; // 設備型號

// ESP32-C3 GPIO 定義
const int bootButton = 9;     // BOOT 按鈕在 GPIO 9
const int resetButton = 1;        // 

const int ledOnBoard = 3;    // 板載 LED 在 GPIO 3（舊註釋誤寫成「第二個按鈕在 GPIO 8」）
const int ledOnFace = 0;     // 面板 LED 在 GPIO 0
// 繼電器腳位：兩版板子分別接在不同腳位
//   341305A_P25_250814 → GPIO 7
//   341305A_Y176_250318 → GPIO 4
// 兩支同時驅動，未接 MOS 的那支為空接腳，輸出不會有副作用。
// 這樣可避免腳位設錯時該腳未初始化而浮空，導致 MOS 誤導通、繼電器恆閉燒毀設備。
const int relayPins[] = {4, 7};
const int relayPinCount = sizeof(relayPins) / sizeof(relayPins[0]);

// ── 電量檢測：GPIO 1 一支腳同時當「電池分壓量測」與「RESET 按鈕」──
//
// 為什麼擠在同一支腳：ESP32-C3 能接 ADC1 的只有 GPIO 0~4（ADC2 的 GPIO 5 在
// WiFi 開啟時讀不到值，這台設備全程掛著 WiFi/MQTT，等於不能用），而板子上
// 實際拉出可焊接的只有 GPIO 0（面板 LED）與 GPIO 1（RESET 按鈕）。
//
// 能共用的原理：按鈕的動作本來就是「把腳拉到 GND」，在 ADC 眼裡就是電壓掉到 0。
// 所以全程只讀 ADC，不切 pinMode，就同時得到兩件事：
//   1.20~1.68V → 電池 6.0~8.4V（2S 鋰電，模組原廠 5 倍分壓，不需改電阻）
//   < 0.5V     → RESET 按鈕被按下
// 按下時是 0V，離電池空電的 1.20V 有 2.4 倍距離，不可能誤判。
//
// 接線：分壓模組 S 腳接 GPIO 1，按鈕維持原本的 GPIO 1 ↔ GND。
// 模組的 "+" 腳不用接（純電阻分壓，那支腳沒作用）。S 腳對 GND 要加 100nF，
// 否則模組 6kΩ 的輸出阻抗擋不住 ESP32 ADC 取樣電容造成的抖動。
const int batterySensePin = 1;                    // 與 resetButton 同一支腳

// 選配的 100kΩ 上拉（GPIO 1 → 3.3V）會讓讀值線性偏移，換算係數因此有兩組。
// 加上拉的用意是防「分壓模組焊點脫落」：沒有它，模組一掉線 GPIO 1 就浮空，
// ADC 讀隨機值有機率掉進按鈕門檻而誤觸重置、把 WiFi 設定清光。
// 加了之後脫落 = 讀到接近 3.3V，會被 BATTERY_OPEN_MV 判為量測異常。
#define BATTERY_HAS_PULLUP 0                      // 焊了 100kΩ 上拉就改成 1
#if BATTERY_HAS_PULLUP
const float BATTERY_SCALE = 5.3f;                 // Vbat = (Vadc - offset) * scale
const int BATTERY_OFFSET_MV = 187;                // 100k 上拉造成的固定抬升
#else
const float BATTERY_SCALE = 5.0f;                 // 模組原廠分壓比
const int BATTERY_OFFSET_MV = 0;
#endif

const int BATTERY_BUTTON_MV = 500;    // ADC 低於此值 → 判定 RESET 按鈕按下
const int BATTERY_OPEN_MV = 2500;     // ADC 高於此值 → 判定分壓模組脫落／量測異常
const int BATTERY_SAMPLES = 8;        // 電量量測的取樣次數（按鈕偵測只取 1 次，不能拖慢 loop）
const unsigned long BATTERY_READ_INTERVAL_MS = 5000;  // 電量重新量測的間隔

// 分壓模組是否真的焊上去了（開機時自動偵測，見 detectBatterySense()）。
// 沒焊的板子退回原本的 INPUT_PULLUP 按鈕模式，單一韌體通吃改裝前後兩種板子，
// 與繼電器「GPIO 4/7 兩支同時驅動」是同一套哲學。
bool batterySenseAvailable = false;
int lastBatteryMilliVolts = 0;        // 最近一次有效的電池電壓（mV），0 代表還沒量到
int lastBatteryPercent = -1;          // 最近一次有效的電量百分比，-1 代表未知
unsigned long lastBatteryReadTime = 0;

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
unsigned long wifiDisconnectStart = 0;      // WiFi 斷線起始時間（用於切換到心跳閃）
const unsigned long LED_TIMEOUT = 30000;    // 斷線超過這麼久，由快閃改為低頻心跳閃
const unsigned long HEARTBEAT_PERIOD = 3000; // 心跳閃週期
const unsigned long HEARTBEAT_ON = 100;      // 心跳閃每次亮燈時間（duty cycle ≈ 3%）

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
void detectBatterySense();
bool isResetButtonPressed();
void updateBatteryReading();
void addBatteryToStatus(JsonDocument& doc);

// ── EEPROM 佈局 ──
//
// 【1.7.3 之前這裡是壞的，不要改回去】
// 舊版 EEPROM.begin(128) 但 mqttPassword 寫在 114~129：
//   * 126 / 127 兩格同時被 mqttPort 寫入，且 mqttPort 寫在後面 → 覆蓋掉密碼第 13、14 字元
//   * 128 / 129 超出 begin(128) 宣告的範圍 → 第 15、16 字元直接寫丟
// 也就是說 MQTT 密碼實際上只有前 12 字元是可靠的，第 13 字元起是 port 的位元組殘留。
// 現在把區塊搬到 130 起、EEPROM 放大到 160。
//
// 【為什麼只搬 mqttPassword，其他一格都不動】
// 搬動任何欄位都會讓已出貨設備在 OTA 之後讀到空值。ssid / password / mqttServer /
// useCustomServer / mqttUsername / mqttPort 全部維持原位，升級後照常運作；
// 只有 mqttPassword 需要重設 —— 而它本來就是壞的（超過 12 字元必定讀錯），
// 搬移是把一份不可靠的資料換成可靠的，不是把好資料弄丟。
const int EEPROM_SIZE      = 160;
const int EE_SSID          = 0;    // 0~31    SSID (32)
const int EE_PASSWORD      = 32;   // 32~63   WiFi 密碼 (32)
const int EE_MQTT_SERVER   = 64;   // 64~95   MQTT 伺服器 (32)
const int EE_USE_CUSTOM    = 96;   // 96      是否使用自訂伺服器 (1)
                                   // 97      保留（未使用）
const int EE_MQTT_USER     = 98;   // 98~113  MQTT 帳號 (16)
const int EE_MQTT_PORT     = 126;  // 126~127 MQTT Port (2)
const int EE_MQTT_PASSWORD = 130;  // 130~145 MQTT 密碼 (16)

// WiFi 設定相關函數實作
void saveWiFiConfig() {
  EEPROM.begin(EEPROM_SIZE);
  // 儲存 WiFi 設定
  for (int i = 0; i < 32; i++) {
    EEPROM.write(EE_SSID + i, ssid[i]);
    EEPROM.write(EE_PASSWORD + i, password[i]);
  }
  // 儲存自訂 MQTT 伺服器設定
  for (int i = 0; i < 32; i++) {
    EEPROM.write(EE_MQTT_SERVER + i, mqttServer[i]);
  }
  // 儲存 MQTT 認證資訊
  for (int i = 0; i < 16; i++) {
    EEPROM.write(EE_MQTT_USER + i, mqttUsername[i]);
    EEPROM.write(EE_MQTT_PASSWORD + i, mqttPassword[i]);
  }
  // 儲存 MQTT Port (2 bytes)
  EEPROM.write(EE_MQTT_PORT, mqttPort & 0xFF);            // 低位元組
  EEPROM.write(EE_MQTT_PORT + 1, (mqttPort >> 8) & 0xFF); // 高位元組

  // 儲存 useCustomServer 標誌
  EEPROM.write(EE_USE_CUSTOM, useCustomServer ? 1 : 0);

  EEPROM.commit();
}

void loadWiFiConfig() {
  EEPROM.begin(EEPROM_SIZE);

  // 檢查 EEPROM 是否已初始化（檢查第一個字元是否為可列印字元或 NULL）
  char firstChar = EEPROM.read(EE_SSID);
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
    ssid[i] = EEPROM.read(EE_SSID + i);
    password[i] = EEPROM.read(EE_PASSWORD + i);
  }
  // 讀取自訂 MQTT 伺服器設定
  for (int i = 0; i < 32; i++) {
    mqttServer[i] = EEPROM.read(EE_MQTT_SERVER + i);
  }
  // 讀取 MQTT 認證資訊
  for (int i = 0; i < 16; i++) {
    mqttUsername[i] = EEPROM.read(EE_MQTT_USER + i);
    mqttPassword[i] = EEPROM.read(EE_MQTT_PASSWORD + i);
  }
  // 讀取 MQTT Port (2 bytes)
  mqttPort = EEPROM.read(EE_MQTT_PORT) | (EEPROM.read(EE_MQTT_PORT + 1) << 8);
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

  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < EEPROM_SIZE; i++) {  // 清除所有設定包括 MQTT
    EEPROM.write(i, 0);
  }
  EEPROM.commit();

  // ── 一併清除 WiFi driver 自己存在 NVS 的舊 AP ──
  //
  // 【為什麼一定要做這件事】
  // 清 EEPROM 只清掉「韌體自己記的那份」。WiFi driver 在 NVS 裡還留著上一次連上的
  // AP（SSID／密碼／PMK 快取），而 setup() 的 WiFi.setAutoReconnect(true) 會讓它
  // 開機就拿那份去連。結果是：
  //   1. 使用者長按重置、用 App 綁定新的 SSID、設備重開
  //   2. driver 在背景連「舊 AP」，connectToWiFi() 在前景連「新 AP」，兩邊互搶
  //   3. 4-way handshake 每次做到一半被對方的 connect() 打斷 → 五種模式全部 reason 15
  //      （序列埠同時會出現 `E wifi:sta is connecting, cannot set config`，
  //        以及每個模式開頭的 reason 8 ASSOC_LEAVE，那是被自己人斷開的痕跡）
  //   4. 第一次探測把 NVS 覆寫成新 AP，所以「再清除一次、再綁定一次」就會成功
  // 這正是 2026-09 回報的「第一次綁定必定失敗、第二次才成功」。
  //
  // esp_wifi_restore() 把 driver 的持久化設定整份還原成預設值。反正下一行就要重啟，
  // 還原後 WiFi stack 的狀態不需要考慮。
  WiFi.disconnect(false, true);  // 第二個參數才是 eraseap，先清掉當前的 AP 記錄
  delay(100);
  esp_err_t wifiRestoreErr = esp_wifi_restore();
  if (wifiRestoreErr == ESP_OK) {
    Serial.println("WiFi driver 的 NVS 設定已還原（舊 AP 不會再被自動重連）");
  } else {
    Serial.printf("⚠ esp_wifi_restore() 失敗: %s，舊 AP 可能仍留在 NVS\n",
                  esp_err_to_name(wifiRestoreErr));
  }

  Serial.println("WiFi 設定已清除。重新啟動中...");
  delay(2000);
  ESP.restart();
}

// ── 電量檢測相關函式 ──

// 開機時偵測分壓模組是否存在，決定 GPIO 1 走 ADC 模式還是原本的 INPUT_PULLUP 按鈕模式。
//
// 原理：開內部下拉，用 ADC 讀電壓。
//   有模組 → 分壓源阻抗僅 6kΩ，對抗內部 45kΩ 下拉後仍有約 1.0~1.5V
//   沒模組 → 腳被內部下拉直接扯到 GND → 接近 0mV
// 【1.8.3 起改用 ADC 判斷，不能用 digitalRead】1.8.0 的寫法是 INPUT_PULLDOWN 後
// digitalRead 看 HIGH/LOW，但有模組時腳上那 1.48V 對 ESP32-C3 的數位輸入是灰色地帶
// （判 HIGH 要 ≥2.5V、判 LOW 要 ≤0.8V），實機讀成 LOW → 判成沒模組 → 退回
// INPUT_PULLUP 按鈕模式 → 分壓輸出約 1.7V 又落在灰色地帶被讀成 LOW → 一開機就
// 「偵測到按鈕按下，開始計時」（2026-09-10 第一次接模組實測）。ADC 讀 mV 兩邊差十倍以上，
// 沒有灰色地帶。
//
// analogRead 附掛腳位時會把腳改成 ANALOG 模式、拿掉上下拉，所以下拉要在附掛之後
// 用 gpio_pulldown_en() 補回來；偵測完成後再拿掉，否則 45kΩ 下拉會並聯到分壓下臂，
// 讀值會偏低約 14%。
//
// 沒有這道偵測的話，未改裝的板子刷上這版韌體會很慘：GPIO 1 沒了 INPUT_PULLUP
// 又只接一顆對地按鈕，等於浮空，ADC 讀隨機值隨時可能掉進按鈕門檻而誤觸重置。
//
// 誤判情境：開機瞬間按住 RESET 按鈕會把腳短路到地，被判成「沒有模組」。
// 可接受 —— 按住按鈕開機本來就不是合法流程（見 checkStuckButtons() 的註釋），
// 放開後重新上電即恢復。
const int BATTERY_DETECT_MV = 600;    // 開下拉後 ADC 高於此值 → 判定有分壓模組

void detectBatterySense() {
  analogReadResolution(12);
  analogReadMilliVolts(batterySensePin);               // 先附掛成 ANALOG，之後才能設衰減、補下拉
  analogSetPinAttenuation(batterySensePin, ADC_11db);  // 量程約 0~2.5V 線性，涵蓋 1.20~1.68V
                                                       // （放在附掛前會印 "Pin is not configured as analog channel"）
  gpio_pulldown_en((gpio_num_t)batterySensePin);
  delay(20);  // 等內部下拉把腳位拉穩
  int detectMv = readSenseMilliVolts(8);
  gpio_pulldown_dis((gpio_num_t)batterySensePin);

  // 診斷指紋：再開上拉量一次，分辨「沒接模組」（≈3300）與「模組接了但沒供電」
  // （S 經 7.5kΩ 到地，≈470，會被按鈕模式當成按下）。只印出來，不參與判定。
  gpio_pullup_en((gpio_num_t)batterySensePin);
  delay(20);
  int pullupMv = readSenseMilliVolts(8);
  gpio_pullup_dis((gpio_num_t)batterySensePin);
  Serial.printf("電量檢測: 指紋 開下拉 %d mV / 開上拉 %d mV（沒模組≈0/3300、模組沒電≈0/470、模組有電≈1300/1700）\n",
                detectMv, pullupMv);

  batterySenseAvailable = (detectMv >= BATTERY_DETECT_MV);

  if (batterySenseAvailable) {
    delay(20);  // 下拉拿掉後讓分壓回到真實電位
    int senseMv = readSenseMilliVolts(BATTERY_SAMPLES);
    Serial.printf("電量檢測: 已啟用（GPIO %d 走 ADC，兼作 RESET 按鈕；偵測 %d mV，分壓 %d mV → 電池約 %d mV）\n",
                  batterySensePin, detectMv, senseMv, senseToBatteryMilliVolts(senseMv));
  } else {
    pinMode(batterySensePin, INPUT_PULLUP);
    Serial.printf("電量檢測: 未偵測到分壓模組（開下拉後偵測 %d mV，門檻 %d mV），GPIO %d 退回按鈕模式\n",
                  detectMv, BATTERY_DETECT_MV, batterySensePin);
    // 「模組接了但沒供電」保護：S 經模組下臂 7.5kΩ 通到地，開上拉只剩約 500mV，
    // 按鈕模式會把它當成一直按著，3 秒後觸發長按重置、清光 WiFi 設定
    // （2026-09-10 第一次接模組、VCC 沒接上，實測指紋 0/527）。
    // 沒接模組時開上拉是 ≈3300，正常按鈕放開也是 ≈3300，所以低於 2000 一定是線上
    // 掛了外部東西。這種狀態下停用重置按鈕，寧可少一個功能也不能自毀設定。
    if (pullupMv < 2000) {
      resetButtonUsable = false;
      Serial.printf("⚠ 電量檢測: 開上拉僅 %d mV，GPIO %d 被外部拉低（分壓模組接了但沒供電？），"
                    "本次開機停用 RESET 按鈕以免誤觸重置。請檢查模組 VCC/GND 是否接到電池\n",
                    pullupMv, batterySensePin);
    }
  }
}

// 讀 GPIO 1 的電壓（mV）。用 analogReadMilliVolts() 而非 analogRead()：
// 前者會套用 eFuse 裡的出廠校準，ESP32 的 ADC 非線性很嚴重，自己乘係數會差到 5% 以上。
int readSenseMilliVolts(int samples) {
  long total = 0;
  for (int i = 0; i < samples; i++) {
    total += analogReadMilliVolts(batterySensePin);
  }
  return (int)(total / samples);
}

// 把分壓後的讀值換算回電池電壓（mV）
int senseToBatteryMilliVolts(int senseMv) {
  int corrected = senseMv - BATTERY_OFFSET_MV;
  if (corrected < 0) corrected = 0;
  return (int)(corrected * BATTERY_SCALE);
}

// 2S 鋰電的放電曲線查表（單顆 SOC 曲線 ×2）。
// 不用線性換算是因為鋰電中段極為平坦：7.74V 到 7.58V 之間就跨掉 20% 電量，
// 而線性法會把這段算成 6%，App 上會看到「電量卡在 60% 很久然後瞬間掉光」。
int batteryPercentFromMilliVolts(int mv) {
  static const int curve[][2] = {
    {8400, 100}, {8120, 90}, {7960, 80}, {7840, 70}, {7740, 60}, {7640, 50},
    {7580, 40}, {7540, 30}, {7480, 20}, {7360, 10}, {6900, 5}, {6000, 0}
  };
  const int points = sizeof(curve) / sizeof(curve[0]);

  if (mv >= curve[0][0]) return 100;
  if (mv <= curve[points - 1][0]) return 0;

  for (int i = 0; i < points - 1; i++) {
    if (mv <= curve[i][0] && mv > curve[i + 1][0]) {
      int mvSpan = curve[i][0] - curve[i + 1][0];
      int pctSpan = curve[i][1] - curve[i + 1][1];
      return curve[i + 1][1] + (mv - curve[i + 1][0]) * pctSpan / mvSpan;
    }
  }
  return 0;
}

// RESET 按鈕是否被按下。
// ADC 模式只取樣一次：這個函式在 loop 每一圈都會跑，8 次取樣會拖慢整個迴圈。
bool isResetButtonPressed() {
  if (!batterySenseAvailable) {
    return digitalRead(batterySensePin) == LOW;
  }
  return analogReadMilliVolts(batterySensePin) < BATTERY_BUTTON_MV;
}

// 定期更新電池讀值。按鈕按住時腳被拉到地、模組脫落時讀值飄高，
// 這兩種情況都不覆寫 lastBatteryMilliVolts —— 否則 App 會在使用者長按重置的那幾秒
// 看到電量瞬間掉到 0，跳出「沒電」警告。
void updateBatteryReading() {
  if (!batterySenseAvailable) return;
  if (millis() - lastBatteryReadTime < BATTERY_READ_INTERVAL_MS) return;
  lastBatteryReadTime = millis();

  int senseMv = readSenseMilliVolts(BATTERY_SAMPLES);
  if (senseMv < BATTERY_BUTTON_MV || senseMv > BATTERY_OPEN_MV) return;  // 按鈕按住／量測異常，沿用舊值

  lastBatteryMilliVolts = senseToBatteryMilliVolts(senseMv);
  lastBatteryPercent = batteryPercentFromMilliVolts(lastBatteryMilliVolts);
}

// 把電量資訊掛進 status JSON。publishStatus() 與 publishStatusWithServer() 共用。
void addBatteryToStatus(JsonDocument& doc) {
  if (!batterySenseAvailable) return;
  JsonObject battery = doc.createNestedObject("battery");
  battery["mv"] = lastBatteryMilliVolts;
  battery["percent"] = lastBatteryPercent;
  battery["valid"] = (lastBatteryPercent >= 0);
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
    // ADC 模式下「恆低」多了一種可能：電池電壓低到分壓後不足 0.5V（即電池 2.5V）。
    // 2S 鋰電到那個電壓保護板早就斷電、板子也不會通電，實務上仍等同按鈕短路。
    if (isResetButtonPressed()) resetLowCount++;
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
  if (resetButtonUsable && isResetButtonPressed()) return true;
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
    // WiFi 未連接模式：前 LED_TIMEOUT 快閃提示，之後轉為低頻心跳閃
    //
    // 【為什麼不再永久熄燈】
    // 舊版超過 LED_TIMEOUT 就 digitalWrite(LOW) 熄到底，而 wifiDisconnectStart
    // 只有「WiFi 連上」才會歸零（本函式僅在另外兩個分支重置它）。開機後的
    // connectToWiFi() 每種 auth 模式要等 10 秒、還要輪好幾種，等它跑完進 loop，
    // 30 秒早就用光了 —— 結果是「從來就連不上的設備一次也不閃」，正好把最需要
    // 指示燈的情境變成沒有指示燈（現場實測：reason 15 連續重試，面板燈全暗）。
    // 現在改成心跳閃：每 HEARTBEAT_PERIOD 亮 HEARTBEAT_ON，duty cycle 約 3%，
    // 比原本 50% 的快閃更省電，同時永遠看得出「我還沒連上」。
    if (wifiDisconnectStart == 0) {
      wifiDisconnectStart = currentTime;
    }
    if (currentTime - wifiDisconnectStart < LED_TIMEOUT) {
      // 剛斷線：快速閃爍
      if (currentTime - lastBlinkTime >= QUICK_BLINK) {
        ledState = !ledState;
        digitalWrite(ledOnFace, ledState);
        digitalWrite(ledOnBoard, ledState);
        lastBlinkTime = currentTime;
      }
    } else {
      // 長時間斷線：低頻心跳閃省電。直接由 millis() 決定亮滅，不動 ledState，
      // 之後若重連成功再斷線，快閃分支會從它自己的 lastBlinkTime 重新起算。
      bool heartbeatOn = (currentTime % HEARTBEAT_PERIOD) < HEARTBEAT_ON;
      digitalWrite(ledOnFace, heartbeatOn ? HIGH : LOW);
      digitalWrite(ledOnBoard, heartbeatOn ? HIGH : LOW);
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
                        // 排程重啟而非就地重啟：先讓 onWrite() 返回，
                        // BLE stack 才送得出 ATT 寫入回應（見 bleRestartAt 宣告）
                        bleRestartAt = millis() + 2000;
                        if (bleRestartAt == 0) bleRestartAt = 1;
                        return;
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
    // 上一輪 hold 可能還鎖著（軟體重啟、OTA 重啟後 hold 不會自己清），先解鎖才能設定
    gpio_hold_dis((gpio_num_t)relayPins[i]);
    pinMode(relayPins[i], OUTPUT);
    digitalWrite(relayPins[i], LOW);
    holdRelayPin(relayPins[i]);
  }
  relayState = false;
}

// 【用 pad hold 代替板子上缺的 gate 下拉電阻】兩版板子的 MOS gate 都沒有下拉，
// 晶片沒在驅動腳位時（reset 空窗、卡在 ROM 下載模式、IDE 監視開關把 C3 重置）
// gate 浮空或被內部上拉推高，P25 版繼電器會穩定常開（2026-09-09／10 兩塊板實測）。
// 板子是定制的無法補焊，改用 gpio_hold_en()：把 pad 鎖在目前輸出電位，之後
// 軟體重啟、EN 重置、下載模式都維持鎖住的電位，只有斷電重上電會清掉。
// 代價是 hold 期間 digitalWrite 無效，所以 setRelayPins() 要先解鎖、切換、再鎖回去。
void holdRelayPin(int pin) {
  // 【鎖之前一定要等 pad 真的變成新電位】hold 鎖住的是「鎖定當下 pad 的實際電位」，
  // 不是 GPIO 暫存器的值。digitalWrite 寫進暫存器到 pad 翻轉有數十 ns 延遲，
  // 緊接著就 gpio_hold_en 會鎖到舊電位——1.8.2 第一版就是這樣：韌體回報 relay=1、
  // 序列印「繼電器 ON」，pad 卻一直是 LOW，負載完全不動（2026-09-10 實測）。
  delayMicroseconds(20);
  gpio_hold_en((gpio_num_t)pin);
}

// 同時設定所有繼電器腳位的輸出
void setRelayPins(bool on) {
  for (int i = 0; i < relayPinCount; i++) {
    gpio_hold_dis((gpio_num_t)relayPins[i]);
    digitalWrite(relayPins[i], on ? HIGH : LOW);
    holdRelayPin(relayPins[i]);
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
      // 容量要放得下 version + url（GitHub Release 網址近百字元）+ md5(32)，
      // 200 bytes 裝不下三個欄位，溢位時 deserializeJson 會回 NoMemory 而整個指令被忽略。
      StaticJsonDocument<384> doc;
      DeserializationError error = deserializeJson(doc, message.substring(7));

      if (error) {
        Serial.printf("更新指令解析失敗：%s\n", error.c_str());
      } else {
        const char* newVersion = doc["version"];
        const char* downloadUrl = doc["url"];
        const char* expectedMd5 = doc["md5"];  // 必填，缺了會被 startFirmwareUpdate() 擋下

        if (newVersion && downloadUrl) {
          Serial.println("收到韌體更新請求");
          Serial.print("新版本：");
          Serial.println(newVersion);
          Serial.print("下載網址：");
          Serial.println(downloadUrl);
          Serial.printf("MD5：%s\n", expectedMd5 ? expectedMd5 : "(未提供)");

          // 開始更新程序
          startFirmwareUpdate(downloadUrl, expectedMd5);
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
  // 【沒人讀 USB CDC 時絕不能讓 Serial 卡住】CDCOnBoot=cdc 下 Serial 是 HWCDC。
  // 電腦曾開過序列埠（監視視窗、esptool）之後，core 3.3.x 的 HWCDC::write() 就把
  // 鏈路視為 connected；之後關掉監視但 USB 線還插著，isPlugged() 仍為 true，旗標
  // 永遠翻不回去，每一次 write() 都要等 20 × tx_timeout_ms(100ms) = 2 秒才放棄。
  // mqttCallback → publishStatus 一路印十幾行，一個指令就卡 35～60 秒，MQTT loop
  // 跑不到、keepalive 過期、broker 45 秒後用 LWT 踢掉——外觀是「收到指令就斷線、
  // 一分鐘後自己回來」，開機秒數卻連續（2026-09-10 實測，開著監視就完全正常）。
  // 現場設備沒接電腦、從未列舉，走的是非阻塞 FIFO 路徑，本來就不受影響；這行是
  // 讓桌面測試時的行為跟現場一致。逾時設 0：緩衝滿了就丟，不等。
  Serial.setTxTimeoutMs(0);
  delay(1000); // 等待序列埠穩定

  Serial.println("齁控－動物管制遠端控制系統 v" + String(firmwareVersion));
  Serial.println("================");

  printRelayPins();

  // 設定並關閉兩顆 LED
  //
  // 【這裡曾經是 INPUT，不要再改回去】
  // 舊版寫 pinMode(ledOnBoard, INPUT)，註釋還誤標成「初始化第二個按鈕」。
  // GPIO 3 是板載 LED（見 readme 的 GPIO 定義），設成輸入模式後全檔案 14 處
  // digitalWrite(ledOnBoard, ...) 全都推不動它 —— 板載燈從頭到尾一次也沒亮過，
  // 所有靠板載燈判讀的狀態指示（WiFi 未連接快閃、MQTT 未連接一長二短、
  // 長按重置確認閃爍）在硬體上都是無效的。
  // GPIO 3 在 ESP32-C3 不是 strapping pin，也沒有被按鈕（GPIO 9 / GPIO 1）
  // 或繼電器（GPIO 4 / GPIO 7）佔用，設成 OUTPUT 沒有副作用。
  pinMode(ledOnBoard, OUTPUT);
  digitalWrite(ledOnBoard, LOW);  // 關閉 LED

  pinMode(ledOnFace, OUTPUT);
  digitalWrite(ledOnFace, LOW);  // 關閉 LED

  pinMode(bootButton, INPUT_PULLUP);  // 改用 INPUT_PULLUP

  // GPIO 1 的 pinMode 由此決定（有分壓模組走 ADC、沒有則退回 INPUT_PULLUP），
  // 必須早於 checkStuckButtons() —— 自檢是靠 isResetButtonPressed() 取樣的。
  detectBatterySense();
  delay(50);  // 等內部提升電阻／分壓網路把腳位拉穩再取樣

  checkStuckButtons();  // 必須早於任何重置流程，卡住的腳會在此被排除
  updateBatteryReading();  // 先量一次，避免上線後的第一筆 status 電量是空的

  loadWiFiConfig();

  // 讀取使用自訂伺服器標誌
  EEPROM.begin(EEPROM_SIZE);
  useCustomServer = (EEPROM.read(EE_USE_CUSTOM) == 1);
  Serial.printf("使用自訂伺服器: %s\n", useCustomServer ? "是" : "否");

  const char* deviceId = getDeviceId();  // 獲取設備 ID

  // 配置 WiFi 設定以提高穩定性
  Serial.println("=== 初始化 WiFi 設定 ===");
  WiFi.onEvent(onWiFiEvent);     // 註冊 WiFi 事件回調（取得斷線原因碼）

  // ── persistent(false) 必須排在 mode() 之前，順序不可對調 ──
  // core 的 WiFiGenericClass::persistent() 只是設一個 _persistent 旗標，真正生效的
  // 地方是 wifiLowLevelInit()：`if (!_persistent) esp_wifi_set_storage(WIFI_STORAGE_RAM)`。
  // 而 wifiLowLevelInit() 是被 mode() 觸發的 —— 舊版把 persistent(false) 寫在
  // mode(WIFI_STA) 後面，driver 早就用預設的 WIFI_STORAGE_FLASH 起來了，
  // 這行完全沒發揮作用：esp_wifi_set_config() 照樣把 AP 寫進 NVS，
  // 開機時 driver 也照樣從 NVS 撈舊 AP 出來自動重連（見 clearWiFiConfig() 的說明）。
  WiFi.persistent(false);        // 不將 WiFi 配置寫入 Flash（減少寫入次數，延長壽命）
  WiFi.mode(WIFI_STA);           // ESP32-C3 必須先設定模式再做其餘配置
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

  // ── BLE 配網完成後的排程重啟 ──
  // onWrite() 不能就地重啟，否則 ATT 寫入回應送不出去（見 bleRestartAt 宣告）。
  if (bleRestartAt != 0 && (long)(millis() - bleRestartAt) >= 0) {
    Serial.println("[BLE] 重新啟動");
    ESP.restart();
  }
  // 讀取按鈕當前狀態（開機診斷判定卡在 LOW 的腳一律視為 HIGH，不參與重置流程）
  bool currentBootState = bootButtonUsable ? digitalRead(bootButton) : HIGH;
  bool currentResetState = (resetButtonUsable && isResetButtonPressed()) ? LOW : HIGH;

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

  // 電量量測（內部自帶 5 秒間隔限頻，按鈕按住期間會沿用舊值不覆寫）
  updateBatteryReading();

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

  // ── 探測期間必須關掉 core 的 auto-reconnect ──
  //
  // 開著的話，本函式每一次 WiFi.disconnect() 都會觸發 STA_DISCONNECTED 事件，
  // core 的 _onStaArduinoEvent 在那個分支會自己 disconnect(); connect(); ——
  // 跑在 WiFi 事件任務裡，用的是 driver 當下的 config，與本函式前景的
  // esp_wifi_set_config() + esp_wifi_connect() 直接對撞。
  // 症狀是每一種 auth 模式都在 4-way handshake 中途被打斷 → 全部收到 reason 15，
  // 而收尾還原 config 時會撞上 `E wifi:sta is connecting, cannot set config`。
  //
  // 這與 1.7.0「不要拆掉 core 的 auto-reconnect」的原則不衝突：那條原則講的是
  // loop() 的非阻塞重連路徑，不該由韌體接管；而本函式是阻塞式完整探測，
  // 前景已經在逐一嘗試，背景再插手就只剩互搶。收尾一定要打開回來。
  WiFi.setAutoReconnect(false);

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
  //
  // (3) setAutoReconnect(true) 打開回來
  //     本函式進門時把它關掉了（理由見函式開頭）。它寫的是 STAClass::_autoReconnect，
  //     是全域 WiFi.STA 物件的成員，deinit 後仍然存活 —— 也就是說**不會自己恢復**，
  //     漏掉這一行，探測失敗後 loop() 就只剩自家每 10 秒一次的 esp_wifi_connect()
  //     補刀，AP 回來時的自動重連整個消失。
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
  WiFi.setAutoReconnect(true);           // 探測結束，把背景自動重連交還給 core

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

  // 電量（沒焊分壓模組的板子不會有這個欄位，App 端要容忍它缺席）。
  // 約多吃 46 bytes —— PubSubClient 的緩衝區設在 512（見 quickConnectToIndex()
  // 的說明），加上去之後這份 JSON 約 250 bytes，還有餘裕，但再加欄位前要重算。
  addBatteryToStatus(doc);

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

  addBatteryToStatus(doc);

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

// MD5 必須是 32 個十六進位字元，格式不合一律當成沒有
static bool isValidMd5(const char* s) {
  if (!s) return false;
  int n = 0;
  for (; s[n]; n++) {
    if (n >= 32) return false;
    char c = s[n];
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  return n == 32;
}

// 韌體下載和更新函數（透過 MQTT 觸發）
//
// ── 為什麼 expectedMd5 是必填 ──
//
// 下載走的是 client.setInsecure()，**不驗證 TLS 憑證**，HTTPS 在這條路上只提供加密、
// 不提供來源鑑別。而舊版收尾寫的是 Update.end(true)：`evenIfRemaining = true` 會跳過
// 「寫滿了沒」的檢查、直接把 _size 改成已寫入量收工，_verifyEnd() 在沒有設定 MD5 時
// 只認映像檔開頭那個 0xE9 magic byte。也就是說**只要前幾個位元組像個映像檔，
// 後面全錯也會被接受**，otadata 照樣切過去 —— 重開機直接開不起來。
//
// 而這塊板子的 MOS gate 沒有下拉電阻（見 ho_relay2/readme.md 與
// .claude/rules/relay-stuck-on-diagnosis.md）：晶片一沒跑使用者程式，繼電器就恆閉合。
// 也就是說 OTA 失敗的表現是「捕捉籠的門恆開」，這是最危險的失效方向。
// 2026-09-09 實測：同一份原始碼 USB 燒進去一切正常，OTA 傳下去就開不起來且繼電器恆開。
//
// 設了 MD5 之後，Update.end() 會逐位元組比對，不符就整個作廢、**otadata 不切換**，
// 設備維持在原本跑得好好的韌體上。失效方向從「變磚 + 門恆開」變成「更新沒成功，繼續運作」。
void startFirmwareUpdate(const char* downloadUrl, const char* expectedMd5) {
  if (isUpdating) {
    Serial.println("更新已在進行中，無法開始新的更新");
    return;
  }

  if (!isValidMd5(expectedMd5)) {
    Serial.println("✗ 拒絕更新：缺少合法的 MD5（需 32 個十六進位字元）");
    Serial.println("  下載不驗證 TLS 憑證，MD5 是唯一能確認映像檔沒壞、沒被掉包的依據。");
    Serial.println("  請在 Firestore 的 firmware_updates/{model} 補上 md5 欄位。");
    if (mqttClient.connected()) {
      String deviceId = getDeviceId();
      String statusTopic = "hoban/" + deviceId + "/status";
      mqttClient.publish(statusTopic.c_str(), "update_rejected_no_md5", true);
    }
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

        // 交給 Update 在 end() 時逐位元組比對。必須排在 begin() 之後：
        // begin() 會重置內部的 MD5 狀態，順序反了設定會被清掉。
        if (!Update.setMD5(expectedMd5)) {
          Serial.println("錯誤：Update.setMD5() 被拒絕，放棄本次更新");
          Update.abort();
          break;
        }
        Serial.printf("已設定預期 MD5：%s\n", expectedMd5);
        
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
        
        // ── 收尾一律走 Update.end()，不要傳 true ──
        //
        // end(evenIfRemaining = true) 會跳過「寫滿了沒」的檢查、把 _size 改成已寫入量
        // 直接收工。配合「沒設 MD5 時 _verifyEnd() 只檢查開頭的 0xE9」，等於截斷或
        // 內容損毀的映像檔照樣會被接受並切換 otadata。現在 MD5 是必填、長度也先檢查過，
        // 沒有任何理由再放寬。
        if (written == contentLength && Update.end()) {
          Serial.println("更新成功（MD5 驗證通過）！準備重新啟動...");
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

        // 走到這裡代表下載不完整或 MD5 不符。一定要 abort()：
        // 不 abort 的話 Update 內部仍持有分區狀態，下一輪 retry 的 begin() 會失敗，
        // 而且已寫入的半套映像檔留在 OTA 分區裡。abort() 後 otadata **不會**切換，
        // 設備維持在現有韌體上繼續運作。
        Serial.printf("✗ 更新失敗：已寫入 %u/%d bytes，Update 錯誤碼 %d\n",
                      written, contentLength, Update.getError());
        if (Update.getError() == UPDATE_ERROR_MD5) {
          Serial.println("  MD5 不符 —— 下載到的映像檔與發佈的不是同一份，已整份作廢");
        }
        Update.abort();
        if (mqttClient.connected()) {
          String deviceId = getDeviceId();
          String statusTopic = "hoban/" + deviceId + "/status";
          mqttClient.publish(statusTopic.c_str(), "update_failed", true);
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