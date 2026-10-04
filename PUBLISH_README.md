# hoRelay2 韌體發布指南

本文檔說明如何使用自動化腳本發布 hoRelay2 韌體更新。

## 前置準備

### 必要工具

1. **Arduino CLI**
   ```bash
   # 下載並安裝
   https://arduino.github.io/arduino-cli/
   ```

2. **Firebase CLI**
   ```bash
   npm install -g firebase-tools
   ```

3. **Python 3.7+** (使用 Python 腳本時)
   ```bash
   python --version  # 確認版本
   ```

### 可選工具（推薦）

1. **Google Cloud SDK (gsutil)**
   ```bash
   # 用於自動上傳到 Firebase Storage
   https://cloud.google.com/sdk/docs/install
   ```

   安裝後需要認證：
   ```bash
   gcloud auth login
   gcloud config set project YOUR_PROJECT_ID
   ```

### Firebase 設定

1. 確保 `hoctrl` 專案目錄下有 `serviceAccountKey.json`
2. 確保已登入 Firebase CLI：
   ```bash
   firebase login
   ```

## 使用方式

### Python 腳本（推薦）

#### 基本用法

```bash
# 在 ho_relay2 目錄下執行
python publish.py
```

#### 帶參數用法

```bash
# 指定更新說明和最低版本
python publish.py -c "修正 WiFi 連接問題" -m "1.2.0"

# 跳過確認直接發布
python publish.py -y -c "緊急修復" -m "1.2.0"
```

#### 完整參數說明

- `-c, --changelog`: 更新說明（可選，不指定會提示輸入）
- `-m, --min-version`: 最低版本要求（預設 1.0.0）
- `-y, --yes`: 跳過確認直接發布
- `-h, --help`: 顯示幫助資訊

### PowerShell 腳本（Windows）

```powershell
# 基本用法
.\publish.ps1

# 帶參數用法
.\publish.ps1 -changeLog "修正 WiFi 連接問題" -minVersion "1.2.0"
```

### Bash 腳本（Linux/Mac）

```bash
# 給予執行權限
chmod +x publish.sh

# 基本用法
./publish.sh

# 帶參數用法
./publish.sh -c "修正 WiFi 連接問題" -m "1.2.0"
```

## 發布流程

腳本會自動執行以下步驟：

### 1. 檢查必要工具
- Arduino CLI
- Firebase CLI
- Python (Python 腳本)
- gsutil (可選)

### 2. 讀取韌體資訊
從 `ho_relay2.ino` 自動讀取：
- 設備型號 (`deviceModel`)
- 韌體版本 (`firmwareVersion`)

### 3. 確認更新資訊
顯示並確認：
- 設備型號
- 韌體版本
- 最低版本要求
- 更新說明

### 4. 編譯韌體
使用 Arduino CLI 編譯：
```
arduino-cli compile --fqbn esp32:esp32:esp32c3 --output-dir build ho_relay2
```

### 5. 上傳到 Firebase Storage
- 使用 gsutil（如果已安裝）自動上傳
- 或提示手動上傳並輸入 URL

檔案路徑格式：
```
firmware/{model}/{model}_v{version}.bin
例如：firmware/hoRelay2/hoRelay2_v1.2.2.bin
```

### 6. 更新 Firestore
更新 `firmware_updates` 集合中的文件：

```json
{
  "version": "1.2.2",
  "download_url": "https://storage.googleapis.com/...",
  "changelog": "更新說明",
  "min_version": "1.0.0",
  "publish_time": Timestamp
}
```

會寫進兩個 Firebase 專案：`hoctrl`（齁控 App）與 `holucam-be6c6`（**只為舊版 HoLuCam App 保留**）。

### 7. 登記 HoLuCam 後台
HoLuCam App 與網頁後台現在讀後台 MySQL 的 `FirmwareRelease`，所以每個型號（hoRelay2 與 hoRelay2-1 各一次）還會：

```
PUT {HOLUCAM_API_BASE}/api/firmware-publish/releases/{model}
Authorization: Bearer {HOLUCAM_FIRMWARE_PUBLISH_TOKEN}
{ "version", "downloadUrl", "md5", "minVersion", "changelog" }
```

| 環境變數 | 說明 |
|----------|------|
| `HOLUCAM_FIRMWARE_PUBLISH_TOKEN` | 後台發版 token（機密）。沒設 → 黃色警告「略過 HoLuCam 後台登記」，發版照常完成 |
| `HOLUCAM_API_BASE` | 後台網址，預設 `https://holucam.neuter.online` |

有設 token 卻登記失敗時會印出後台回的錯誤碼（如 `firmware-publish-disabled`、`invalid-publish-token`、`invalid-body`、`invalid-model`、`invalid-md5`、`url-too-long`）與訊息，結尾列出失敗項並以結束代碼 1 結束。

**部署順序**：先部署 HoLuCam 後台並設定 `FIRMWARE_PUBLISH_TOKEN`，之後才在發版機設定 `HOLUCAM_FIRMWARE_PUBLISH_TOKEN`。反過來的話，發版機有 token、後台卻還沒啟用，登記會回 503 並以結束代碼 1 結束。後台的 `FIRMWARE_PUBLISH_TOKEN` 少於 32 字元會視同未設定（同樣回 503 `firmware-publish-disabled`）。

`HOLUCAM_API_BASE` 必須是 `https://`（只有 `http://localhost`、`http://127.0.0.1` 可用 http）；token 含換行等控制字元也會直接判定失敗，兩者都不會送出 token。

登記失敗時韌體已上傳、Firestore 已寫、`.ino` 版號也已 +1：**不要重跑 publish.py（會再跳一個版號），請到 HoLuCam 後台「韌體管理」頁手動登記。**

`.ino` 的 `deviceModel` 與 `MODEL_CONFIGS` 的 `expected_model` 不符（或與其他型號撞名）時，會在改版號與編譯前就中止，不上傳、不寫 Firestore、不登記後台。

## 手動上傳步驟（無 gsutil）

如果未安裝 gsutil，需要手動上傳：

1. 腳本會編譯韌體並顯示 `.bin` 檔案路徑
2. 前往 [Firebase Console](https://console.firebase.google.com/)
3. 選擇專案 → Storage
4. 上傳檔案到 `firmware/{model}/` 目錄
5. 點擊檔案 → 取得下載 URL
6. 將 URL 貼回腳本

## 版本號規則

版本號格式：`主版本.次版本.修訂版本`

例如：`1.2.2`
- 主版本 (1)：重大架構變更
- 次版本 (2)：新功能或改進
- 修訂版本 (2)：錯誤修正

### 更新版本號

修改 `ho_relay2.ino` 中的版本號：

```cpp
const char* firmwareVersion = "1.2.3"; // 更新這裡
```

## 最低版本要求

`min_version` 用於限制哪些舊版本可以更新：

- 設定為 `1.0.0`：所有版本都可以更新
- 設定為 `1.2.0`：只有 1.2.0 及以上版本可以更新

使用情境：
- 重大架構變更，舊版本無法直接升級
- 需要先升級到中間版本

## 測試更新

### 1. 檢查 Firestore

前往 Firebase Console → Firestore：
```
集合: firmware_updates
文件: hoRelay2
```

確認資料是否正確。

### 2. 測試設備更新

1. 打開 Flutter App
2. 進入設備詳情頁
3. 點選「韌體更新」
4. 確認顯示新版本資訊
5. 執行更新並觀察過程

## 常見問題

### Q: Arduino CLI 找不到開發板

```bash
# 安裝 ESP32 開發板
arduino-cli core install esp32:esp32
```

### Q: 編譯失敗，缺少函式庫

```bash
# 安裝必要函式庫
arduino-cli lib install "PubSubClient"
arduino-cli lib install "ArduinoJson"
```

### Q: gsutil 上傳失敗

確認已經認證並設定專案：
```bash
gcloud auth login
gcloud config set project YOUR_PROJECT_ID
```

### Q: Firestore 更新失敗

檢查：
1. `serviceAccountKey.json` 是否存在
2. Service Account 是否有 Firestore 寫入權限
3. Node.js 是否已安裝 `firebase-admin`：
   ```bash
   cd ../../hoctrl
   npm install firebase-admin
   ```

## 回滾版本

如果更新出現問題，可以：

1. 修改 `firmwareVersion` 回到穩定版本
2. 重新執行發布腳本
3. 或在 Firestore 中直接修改 `version` 和 `download_url`

## 安全注意事項

- 不要將 `serviceAccountKey.json` 提交到版本控制
- 確保 Storage 的檔案權限正確設定
- 定期檢查並清理舊版本韌體檔案
- 測試新版本後再發布給所有使用者

## 自動化建議

可以將發布流程整合到 CI/CD：

```yaml
# GitHub Actions 範例
- name: Publish Firmware
  run: |
    cd hoctrl_arduino/ho_relay2
    python publish.py -y -c "${{ github.event.head_commit.message }}"
```

## 聯絡資訊

如有問題，請查看：
- [Arduino 韌體文檔](./readme.md)
- [Flutter App 文檔](../../hoctrl/README.md)

