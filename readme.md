### hoRelay2
- **開發板**: ESP32-C3 Dev Module
- **特色**: 無聲繼電器
- **韌體版本**: 1.6.1
- **GPIO 定義**:
  - BOOT 按鈕: GPIO 9
  - RESET 按鈕: GPIO 1
  - 板載 LED: GPIO 3
  - 面板 LED: GPIO 0
  - 繼電器按鈕: GPIO 4 與 GPIO 7（兩支同時驅動，單一韌體通吃兩版板子）
    - 341305A_P25_250814 → 實際接在 GPIO 7
    - 341305A_Y176_250318 → 實際接在 GPIO 4
    - 未接 MOS 的那支為空接腳，輸出無副作用；不再需要依板號手動改腳位
- **繼電器腳位注意事項**:
  - GPIO 4/7 在 ESP32-C3 上是 JTAG 腳（MTMS/MTDO），reset 後狀態不保證為低電位
  - `initRelayPins()` 必須維持在 `setup()` 第一行，越晚拉低、MOS 誤導通的時間窗越長
- **開發板設定**:
  - USB CDC On Boot: Enabled
  - CPU Frequency: 160MHz (WiFi)
  - Flash Size: 4MB (32Mb)
  - Partition Scheme: Custom (使用 partitions.csv)
  - Upload Speed: 921600
  - Flash Mode: DIO
  - Erase All Flash Before Sketch Upload：Enabled

---

## 編譯與燒錄

```powershell
Set-Location A:\project\hoctrl_arduino
.\flash.ps1 -Model 2            # 只編譯
.\flash.ps1 -Model 2 -Upload    # 編譯並燒錄（自動偵測 COM 埠）
```

`-Model` 的合法值是 `1,2,3,master,master-c3,slave,test`，**hoRelay2 對應 `2`**。
腳本預設 `EraseFlash=all`，燒完 EEPROM 一併被抹掉，**需重新透過 BLE 配網**。

### flash 用量要自己換算

用 `PartitionScheme=custom` 時，arduino-cli 印的百分比是拿**整顆晶片**當分母（顯示為 16MB），
不是實際的 app0 分區。真實分母請看 `partitions.csv` 的 `app0`：**0x1F0000 = 2,031,616 bytes**。

例：1.6.1 編譯出 1,433,347 bytes，arduino-cli 印「8%」，對 app0 實際是 **70.5%**。

---

## 已知硬體限制：開機瞬間繼電器短暫通電

> **這是硬體限制，韌體無法根治。** 不要再嘗試用軟體解決，只能靠改板子。

### 症狀

插著設備上電（或按 RESET）時，繼電器會**短暫通電再斷掉**（聽得到咔一聲），
之後才進入正常的斷電狀態。預期行為應該是全程保持斷電。

### 成因

繼電器接的 GPIO 4 與 GPIO 7 在 ESP32-C3 上是外部 JTAG 腳（MTMS / MTDO），
晶片 reset 後由 ROM 依 JTAG 功能配置，**不保證維持低電位**。
從上電 → bootloader → 跑到第一行使用者程式，這段約 **200~500ms** 的空窗期內，
腳位狀態完全由晶片自己決定，任何韌體都插不了手。
浮空或被拉高的 MOS gate 一旦超過導通門檻（約 1~2V），繼電器就會動作。

### 韌體已做的緩解

`initRelayPins()` 放在 `setup()` **最前面**，早於 `Serial.begin()` 與其後的 `delay(1000)`，
把導通窗口從原本的 1 秒以上壓到僅剩上述 ROM 空窗。
**修改 `setup()` 時務必保持它在第一行**，任何插到它前面的程式都會直接拉長繼電器誤動作時間。

同時 GPIO 4 與 7 兩支都會被初始化拉低，因此**不存在「未使用的腳浮空」的情況**。
這一併消除了舊版依板號手動設定腳位、設錯就導致繼電器恆閉燒毀設備的風險。

### 根治方式（需改硬體）

在 MOS gate 對地加 **10kΩ 下拉電阻**。
上電瞬間即把 gate 釘在 0V，ROM 空窗完全消失，這也是 MOS 驅動電路的標準做法。
目前 341305A_P25_250814 與 341305A_Y176_250318 兩版板子皆未配置，
**下一版 layout 應補上**。已出貨的板子只能維持現狀的緩解程度。

---

## LED 狀態指示

| 狀態 | LED 行為 |
|------|----------|
| BLE 配對模式（含長按清除設定後）| 快閃 200ms（`PAIRING_BLINK`），持續不熄燈 |
| WiFi 未連接 | 快閃 300ms（`QUICK_BLINK`），30 秒後熄燈省電 |
| WiFi 已連、MQTT 未連 | 一長二短 |
| WiFi 與 MQTT 都已連上 | 熄燈 |
| 長按重置確認中 | 閃爍 250ms（`BLINK_INTERVAL`），確認後長亮 0.7 秒 |

長按清除設定後，EEPROM 全 0 會被 `loadWiFiConfig()` 判定為「有效但空白」，
覆蓋掉編譯期預設的 `HBTech`，因此設備會進入 BLE 配對模式持續快閃，**不會自己去連預設 WiFi**。
硬編碼的預設 WiFi 只在晶片出廠、EEPROM 從未寫過時生效一次。

---

## 長按重置設定

BOOT(GPIO 9) 或 RESET(GPIO 1) **任一顆**，總共按住 5 秒：

1. 按住滿 3 秒（`LONG_PRESS_TIME`）→ LED 開始以 250ms 週期閃爍
2. 閃爍期間**持續按住**再 2 秒（`BLINK_CONFIRM_TIME`）→ LED 長亮 0.7 秒（`CONFIRM_SOLID_TIME`）
3. 清除 EEPROM 全部 128 bytes 並重啟 → 進入 BLE 配對模式（200ms 持續快閃）

中途放開即取消、計時歸零。WiFi 連線等待期間共用同一支 `waitForResetConfirm()`，
行為與正常運作時一致。

（1.7.0 之前另有一支 `interruptibleDelay()` 也共用它，該函式的呼叫點全在
1.7.0 重寫掉的 WiFi 重連區段裡，已一併移除。）

### 開機按鈕自檢

`checkStuckButtons()` 在 `setup()` 中 `pinMode(..., INPUT_PULLUP)` 之後、任何重置流程之前，
取樣兩支按鈕腳 500ms。整段都是 LOW 的腳判定為短路／未接，**本次開機停用其重置功能**：

```
⚠ 按鈕自檢: RESET(GPIO 1) 恆為 LOW，本次開機停用其重置功能
```

這是為了擋掉「開機即清除設定 → 重啟 → 再清除」的無限迴圈（2026-08-14 實際發生過，
RESET 按鈕 GPIO 1 內部短路）。副作用：「按住按鈕再上電」會被擋掉，放開後重新上電即恢復。
詳見 `.claude/rules/button-pin-stuck-low.md`。

---

## 設備 ID 與 MQTT 主題

設備 ID 格式 `hoban-{MAC}`，MAC 為**網路正序**（與 `WiFi.macAddress()` 一致）。

```
狀態發布: hoban/{device_id}/status
控制訂閱: hoban/{device_id}/control
```

1.6.0 修正了 `getDeviceId()` 把 `ESP.getEfuseMac()` 的位元組**反序**輸出的缺陷。
為避免 OTA 後尚未更新的 App 失聯，韌體會**同時訂閱舊版反序主題**
（`getLegacyDeviceId()`），收到時序列埠會印：

```
⚠ 收到舊版主題指令（App 尚未更新設備 ID）
```

狀態一律只發布到新的正序主題。待所有 App 完成遷移後，可移除 `getLegacyDeviceId()`
與兩處 `legacyControlTopic` 訂閱。

---

## 版本記錄

### 1.7.0

**WiFi／MQTT 重連邏輯整段重寫。** 起因是「斷線後不會自動連線」，盤點後在重連路徑上
找出 13 項缺陷，詳見 `.claude/rules/wifi-mqtt-reconnect-antipatterns.md`。

四個根因：

- **連上了卻因為「太慢」被丟掉**（最致命）。`quickConnectToIndex()` / `quickConnectCustom()`
  舊碼在 `mqttClient.connect()` **成功之後**才判斷耗時，超過 1 秒就 `disconnect()` 並回 false。
  台灣連海外公共 broker 光 RTT 就 150~300ms，五台會**全部**被判太慢且沒有 fallback
  → 每台都連得上、設備卻永遠離線。**已改為連上就採用**，耗時只印警告
- **跟 core 的自動重連打架**。`WiFi.disconnect(true)` 的第一個參數是 `wifioff` 不是 `eraseap`，
  它會走到 `STAClass::onDisable()` 移除 WiFi 事件處理器、把 `_esp_netif` 設 NULL，
  而 core 的 auto-reconnect 正是掛在那個處理器上。**運行期不再拆 WiFi 子系統**，
  改為讓 core 自己重連、韌體只補送非阻塞的 `esp_wifi_connect()`
- **時間戳設在阻塞呼叫之前**。`connectToWiFi()` 可阻塞 50 秒以上，回來時 5 秒閘門早已過期
  → 「每 5 秒檢查」在失敗時等於背靠背連續重試。**所有時間戳改到阻塞呼叫之後才取**
- **`lastWiFiCheck = now + 25000` 的無號數比較**。差值恆為 4294942296，
  「暫停重試 30 秒」條件恆真、從未生效。**改用獨立的 `nextXxxAt` 變數與 wrap-safe 比較**

其他：

- `smartConnectStep()` 每次只試一台 broker，單次阻塞從最壞 90 秒壓到單台成本；
  `FIND_BEST_SERVER` 指令也改走這條路（原本在 callback 裡直呼 `smartConnect()`，阻塞 111 秒）
- `currentServerIndex` 改為失敗也輪替，不再連續重試同一台死 broker
- 早退原因碼補上 210/211/212；`setScanTimeout(8000)`（core 預設 60 秒）
- `connectToWiFi()` 收尾會**還原 auth config 並重套 `setSleep(false)` / `setTxPower()`**
  —— 這兩者是驅動層設定，`esp_wifi_deinit()` 後不會自動恢復
- 移除死變數 `failedAttempts`（三處寫入、零處讀取）與死函式 `interruptibleDelay()`

**實測狀態**：僅通過編譯與兩輪對抗性複審，**尚未實機驗證**。
`mqttClient.loop()` 的最大沉默窗口由靜態推演從 113 秒降至約 13 秒（broker 踢人門檻 45 秒），
AP 斷電 30 秒的恢復時間由 60~190 秒降至約 31~39 秒——**兩者都是靜態推演，無上界保證**。

### 1.6.1

- BLE 配對模式（含長按清除設定後）的 LED 由 1000ms 慢閃改為 **200ms 持續快閃**，
  與「WiFi 連不上」的 300ms 快閃刻意錯開以便肉眼分辨
- 序列埠的「已發布狀態」日誌加上 device_id，多台設備同時看 log 時可分辨來源

### 1.6.0

- **繼電器改為 GPIO 4 與 GPIO 7 同時驅動**，單一韌體通吃兩版板子，
  並以 `initRelayPins()` 在 `setup()` 第一行拉低兩支腳，消除「未使用的腳浮空導致 MOS 誤導通」的風險
- **修正 `getDeviceId()` 的 MAC 反序缺陷**，改為網路正序；同時訂閱舊版反序控制主題以相容尚未更新的 App
- **新增開機按鈕自檢 `checkStuckButtons()`**，擋掉按鈕短路造成的無限清除迴圈
- 長按重置改為「3 秒 → 250ms 閃爍 → 再 2 秒 → 長亮 0.7 秒」，並修掉 `buttonPressTime == 0`
  時 `millis() - 0` 會瞬間超過門檻而立刻重置的缺陷
- `device["relay"]` 改讀 `relayState` 變數，不再 `digitalRead()` 單一腳位

### 1.5.1 以前

見 git log。