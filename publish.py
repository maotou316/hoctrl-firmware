#!/usr/bin/env python3
"""
hoRelay2 韌體發布自動化腳本
此腳本會編譯韌體、上傳到 Firebase Storage 並更新 Firestore 記錄
"""

import os
import sys
import subprocess
import json
import re
import argparse
from pathlib import Path
from datetime import datetime
import shutil
import platform

class Colors:
    """終端顏色定義"""
    RED = '\033[0;31m'
    GREEN = '\033[0;32m'
    YELLOW = '\033[1;33m'
    CYAN = '\033[0;36m'
    WHITE = '\033[0;37m'
    GRAY = '\033[0;90m'
    NC = '\033[0m'  # No Color

def print_color(message, color=Colors.WHITE):
    """顏色輸出"""
    try:
        print(f"{color}{message}{Colors.NC}")
    except UnicodeEncodeError:
        # Windows console 編碼問題，使用 ASCII 替代字符
        safe_message = message.encode('ascii', errors='replace').decode('ascii')
        print(f"{color}{safe_message}{Colors.NC}")

def print_header(title):
    """打印標題"""
    print_color(f"\n{'='*50}", Colors.CYAN)
    print_color(f"  {title}", Colors.CYAN)
    print_color(f"{'='*50}\n", Colors.CYAN)

def get_arduino_cli_path():
    """取得 arduino-cli 執行檔路徑"""
    # Windows 常見安裝位置
    if platform.system() == 'Windows':
        common_paths = [
            r'C:\Program Files\Arduino CLI\arduino-cli.exe',
            r'C:\Program Files (x86)\Arduino CLI\arduino-cli.exe',
            os.path.expandvars(r'%LOCALAPPDATA%\Arduino15\arduino-cli.exe'),
            os.path.expandvars(r'%USERPROFILE%\AppData\Local\Programs\Arduino CLI\arduino-cli.exe'),
        ]
        for path in common_paths:
            if os.path.exists(path):
                return path

    # 其他系統或在 PATH 中
    return 'arduino-cli'

def check_command(command):
    """檢查命令是否存在"""
    if command == 'arduino-cli':
        path = get_arduino_cli_path()
        if platform.system() == 'Windows' and path.endswith('.exe'):
            return os.path.exists(path)

    # 特別處理 gh (GitHub CLI) 在 Windows 上的路徑
    if command == 'gh' and platform.system() == 'Windows':
        gh_path = r'C:\Program Files\GitHub CLI\gh.exe'
        if os.path.exists(gh_path):
            return True

    return shutil.which(command) is not None

def check_requirements():
    """檢查必要工具"""
    print_header("檢查必要工具")
    
    requirements = {
        'arduino-cli': 'https://arduino.github.io/arduino-cli/',
        'firebase': 'npm install -g firebase-tools'
    }
    
    all_installed = True
    for cmd, install_guide in requirements.items():
        if check_command(cmd):
            print_color(f"✓ {cmd} 已安裝", Colors.GREEN)
        else:
            print_color(f"❌ 未安裝 {cmd}", Colors.RED)
            print_color(f"   安裝方式: {install_guide}", Colors.YELLOW)
            all_installed = False
    
    # 檢查可選工具
    if check_command('gsutil'):
        print_color("✓ gsutil 已安裝 (推薦)", Colors.GREEN)
    else:
        print_color("⚠ 未安裝 gsutil (可選，但推薦安裝以自動上傳)", Colors.YELLOW)
    
    return all_installed

def get_firmware_info():
    """從 .ino 檔案讀取韌體資訊"""
    print_header("讀取韌體資訊")
    
    ino_file = "ho_relay2.ino"
    if not os.path.exists(ino_file):
        print_color(f"❌ 找不到 {ino_file}", Colors.RED)
        return None
    
    try:
        with open(ino_file, 'r', encoding='utf-8') as f:
            content = f.read()
        
        # 提取版本號
        version_match = re.search(r'const char\* firmwareVersion = "([^"]+)"', content)
        if not version_match:
            print_color("❌ 無法從 .ino 檔案讀取版本號", Colors.RED)
            return None
        version = version_match.group(1)
        
        # 提取設備型號
        model_match = re.search(r'const char\* deviceModel = "([^"]+)"', content)
        if not model_match:
            print_color("❌ 無法從 .ino 檔案讀取設備型號", Colors.RED)
            return None
        model = model_match.group(1)
        
        print_color(f"設備型號: {model}", Colors.WHITE)
        print_color(f"韌體版本: {version}", Colors.WHITE)
        
        return {
            'version': version,
            'model': model
        }
    except Exception as e:
        print_color(f"❌ 讀取檔案失敗: {e}", Colors.RED)
        return None

def build_firmware(model):
    """編譯韌體"""
    print_header("開始編譯韌體")

    # FQBN 含完整配置參數 (對應 Arduino IDE 設定)
    fqbn = "esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,DebugLevel=error,EraseFlash=all,FlashFreq=80,FlashMode=dio,FlashSize=4M,JTAGAdapter=default,PartitionScheme=custom,UploadSpeed=921600,ZigbeeMode=default"
    build_path = "build"

    # 創建 build 目錄
    os.makedirs(build_path, exist_ok=True)

    print_color("正在編譯 (使用自訂 Partition Scheme)...", Colors.YELLOW)

    try:
        # 編譯 - arduino-cli 需要指向包含 .ino 檔案的目錄
        # 不使用 capture_output 以便即時顯示編譯進度
        result = subprocess.run(
            ['arduino-cli', 'compile', '--fqbn', fqbn, '--output-dir', build_path, '.'],
            encoding='utf-8',
            errors='ignore'
        )

        if result.returncode != 0:
            print_color("❌ 編譯失敗", Colors.RED)
            return None
        
        # 找到編譯後的 .bin 檔案
        bin_files = list(Path(build_path).glob('*.bin'))
        if not bin_files:
            print_color("❌ 找不到編譯後的 .bin 檔案", Colors.RED)
            return None
        
        bin_file = bin_files[0]
        file_size = bin_file.stat().st_size / 1024  # KB
        
        print_color(f"✓ 編譯成功: {bin_file.name}", Colors.GREEN)
        print_color(f"檔案大小: {file_size:.2f} KB", Colors.WHITE)
        
        return str(bin_file)
    except Exception as e:
        print_color(f"❌ 編譯過程出錯: {e}", Colors.RED)
        return None

def get_firebase_project_id():
    """從 Firebase 配置讀取專案 ID"""
    firebase_config_path = "../../hoctrl/.firebaserc"
    try:
        with open(firebase_config_path, 'r') as f:
            config = json.load(f)
            return config['projects']['default']
    except Exception as e:
        print_color(f"⚠ 無法讀取 Firebase 專案 ID: {e}", Colors.YELLOW)
        return None

def upload_to_firebase(bin_path, model, version, changelog="更新"):
    """上傳韌體 (優先使用 GitHub Releases)"""
    print_header("上傳韌體")

    file_name = f"{model}_v{version}.bin"
    storage_path = f"firmware/{model}/{file_name}"

    print_color(f"上傳檔案: {file_name}", Colors.YELLOW)

    # 方法1: 使用 GitHub Releases (優先)
    if check_command('gh'):
        try:
            print_color("使用 GitHub Releases 上傳...", Colors.YELLOW)

            # GitHub CLI 路徑
            gh_cmd = r'C:\Program Files\GitHub CLI\gh.exe' if platform.system() == 'Windows' else 'gh'

            # GitHub repository 設定
            repo = os.getenv('GITHUB_REPO', 'maotou316/hoctrl-firmware')
            tag_name = f"v{version}"

            # 重命名檔案 (如果還沒重命名)
            renamed_file = f'build/{model}_v{version}.bin'
            if bin_path != renamed_file and not os.path.exists(renamed_file):
                shutil.copy(bin_path, renamed_file)
                bin_path = renamed_file
            elif os.path.exists(renamed_file):
                # 檔案已存在,直接使用
                bin_path = renamed_file

            print_color(f"Repository: {repo}", Colors.GRAY)
            print_color(f"Tag: {tag_name}", Colors.GRAY)

            # 檢查 release 是否已存在
            check_cmd = [gh_cmd, 'release', 'view', tag_name, '--repo', repo]
            check_result = subprocess.run(check_cmd, capture_output=True, text=True)

            if check_result.returncode == 0:
                # Release 已存在，上傳檔案
                print_color(f"Release {tag_name} 已存在，上傳檔案...", Colors.GRAY)
                upload_cmd = [
                    gh_cmd, 'release', 'upload', tag_name,
                    bin_path,
                    '--clobber',
                    '--repo', repo
                ]
            else:
                # 建立新 release
                print_color(f"建立新 Release {tag_name}...", Colors.GRAY)
                upload_cmd = [
                    gh_cmd, 'release', 'create', tag_name,
                    bin_path,
                    '--title', f"{model} v{version}",
                    '--notes', f"韌體版本 {version}\n\n{changelog}",
                    '--repo', repo
                ]

            result = subprocess.run(upload_cmd, capture_output=True, text=True)

            if result.returncode == 0:
                # 生成下載 URL
                uploaded_file_name = f"{model}_v{version}.bin"
                download_url = f"https://github.com/{repo}/releases/download/{tag_name}/{uploaded_file_name}"

                print_color("✓ 上傳成功", Colors.GREEN)
                print_color(f"下載 URL: {download_url}", Colors.WHITE)

                return download_url
            else:
                print_color(f"GitHub 上傳失敗: {result.stderr}", Colors.RED)
        except Exception as e:
            print_color(f"GitHub Releases 上傳失敗: {e}", Colors.YELLOW)

    # 方法2: 使用 Python 直接上傳到 Firebase Storage
    try:
        from google.cloud import storage
        from google.oauth2 import service_account

        # 尋找 service account key
        service_account_paths = [
            "serviceAccountKey.json",
        ]

        service_account_path = None
        for path in service_account_paths:
            if os.path.exists(path):
                service_account_path = path
                break

        if service_account_path:
            print_color(f"使用 Service Account: {service_account_path}", Colors.GRAY)

            # 使用 service account 認證
            credentials = service_account.Credentials.from_service_account_file(
                service_account_path
            )
            storage_client = storage.Client(credentials=credentials, project='hoctrl')
        else:
            # 嘗試使用預設認證
            print_color("嘗試使用預設認證...", Colors.GRAY)
            storage_client = storage.Client(project='hoctrl')

        # 嘗試取得或建立 bucket
        bucket_name = 'hoctrl.firebasestorage.app'
        try:
            bucket = storage_client.get_bucket(bucket_name)
            print_color(f"使用現有 bucket: {bucket_name}", Colors.GRAY)
        except Exception:
            # Bucket 不存在，嘗試建立
            print_color(f"Bucket {bucket_name} 不存在，正在建立...", Colors.YELLOW)
            try:
                bucket = storage_client.create_bucket(bucket_name, location='asia-east1')
                print_color(f"✓ Bucket 建立成功", Colors.GREEN)
            except Exception as create_error:
                # 如果建立失敗，嘗試使用預設的 appspot bucket
                print_color(f"建立 bucket 失敗: {create_error}", Colors.YELLOW)
                bucket_name = 'hoctrl.appspot.com'
                print_color(f"嘗試使用預設 bucket: {bucket_name}", Colors.GRAY)
                try:
                    bucket = storage_client.get_bucket(bucket_name)
                except Exception:
                    # 建立 appspot bucket
                    bucket = storage_client.create_bucket(bucket_name, location='asia-east1')
                    print_color(f"✓ 預設 bucket 建立成功", Colors.GREEN)

        # 上傳檔案
        print_color("正在上傳...", Colors.YELLOW)
        blob = bucket.blob(storage_path)
        blob.upload_from_filename(bin_path)

        # 設置為公開讀取
        blob.make_public()

        # 生成下載 URL
        download_url = blob.public_url

        print_color("✓ 上傳成功", Colors.GREEN)
        print_color(f"下載 URL: {download_url}", Colors.WHITE)

        return download_url

    except ImportError:
        print_color("⚠ 未安裝 google-cloud-storage", Colors.YELLOW)
    except Exception as e:
        print_color(f"⚠ Firebase Storage 上傳失敗: {e}", Colors.YELLOW)
        if kDebugMode := os.getenv('DEBUG'):
            import traceback
            traceback.print_exc()

    # 方法3: 使用 gsutil
    if check_command('gsutil'):
        bucket = "gs://hoctrl.firebasestorage.app"

        try:
            print_color("使用 gsutil 上傳...", Colors.YELLOW)

            # 上傳檔案
            subprocess.run(
                ['gsutil', 'cp', bin_path, f"{bucket}/{storage_path}"],
                check=True
            )

            # 設置公開讀取權限
            subprocess.run(
                ['gsutil', 'acl', 'ch', '-u', 'AllUsers:R', f"{bucket}/{storage_path}"],
                check=True
            )

            # 生成下載 URL
            download_url = f"https://storage.googleapis.com/hoctrl.firebasestorage.app/{storage_path}"

            print_color("✓ 上傳成功", Colors.GREEN)
            print_color(f"下載 URL: {download_url}", Colors.WHITE)

            return download_url
        except subprocess.CalledProcessError as e:
            print_color(f"❌ 上傳失敗: {e}", Colors.RED)

    # 方法3: 手動上傳提示
    print_color("\n⚠ 自動上傳失敗，請手動上傳", Colors.YELLOW)
    print_color(f"\n檔案位置: {os.path.abspath(bin_path)}", Colors.CYAN)
    print_color(f"Firebase Storage 路徑: {storage_path}", Colors.CYAN)
    print_color("\n手動上傳步驟:", Colors.WHITE)
    print_color("1. 開啟 Firebase Console: https://console.firebase.google.com/project/hoctrl/storage", Colors.WHITE)
    print_color(f"2. 上傳檔案到: {storage_path}", Colors.WHITE)
    print_color("3. 點擊檔案，取得下載 URL", Colors.WHITE)
    print_color("4. 在下方輸入 URL\n", Colors.WHITE)

    # 預設 URL (如果手動上傳後權限設為公開)
    default_url = f"https://storage.googleapis.com/hoctrl.firebasestorage.app/{storage_path}"
    print_color(f"預期的 URL (如果已手動上傳): {default_url}", Colors.GRAY)

    download_url = input("\n請輸入下載 URL (或直接按 Enter 使用預期的 URL): ").strip()
    return download_url if download_url else default_url

def update_firestore(model, version, download_url, changelog, min_version):
    """更新 Firestore"""
    print_header("更新 Firestore 記錄")

    # 方法1: 使用 Python Firebase Admin SDK
    try:
        from google.cloud import firestore
        from google.oauth2 import service_account
        from datetime import datetime

        # 尋找 service account key
        service_account_paths = [
            "serviceAccountKey.json",
            "../../hoctrl/serviceAccountKey.json",
        ]

        service_account_path = None
        for path in service_account_paths:
            if os.path.exists(path):
                service_account_path = path
                break

        if service_account_path:
            print_color(f"使用 Service Account: {service_account_path}", Colors.GRAY)
            credentials = service_account.Credentials.from_service_account_file(
                service_account_path
            )
            db = firestore.Client(credentials=credentials, project='hoctrl')
        else:
            print_color("嘗試使用預設認證...", Colors.GRAY)
            db = firestore.Client(project='hoctrl')

        # 更新 Firestore
        print_color("正在更新 Firestore...", Colors.YELLOW)

        update_data = {
            'version': version,
            'download_url': download_url,
            'changelog': changelog,
            'min_version': min_version,
            'publish_time': firestore.SERVER_TIMESTAMP
        }

        db.collection('firmware_updates').document(model).set(update_data, merge=True)

        print_color("✓ Firestore 更新成功", Colors.GREEN)
        print_color(f"文件路徑: firmware_updates/{model}", Colors.WHITE)
        return True

    except ImportError:
        print_color("⚠ 未安裝 google-cloud-firestore", Colors.YELLOW)
    except Exception as e:
        print_color(f"⚠ Python 更新 Firestore 失敗: {e}", Colors.YELLOW)

    # 方法2: 使用 Node.js 腳本
    flutter_dir = Path("../../hoctrl")
    node_script = f"""
const admin = require('firebase-admin');
const serviceAccount = require('./serviceAccountKey.json');

admin.initializeApp({{
  credential: admin.credential.cert(serviceAccount)
}});

const db = admin.firestore();
const updateData = {{
  version: '{version}',
  download_url: '{download_url}',
  changelog: `{changelog}`,
  min_version: '{min_version}',
  publish_time: admin.firestore.Timestamp.now()
}};

db.collection('firmware_updates')
  .doc('{model}')
  .set(updateData, {{ merge: true }})
  .then(() => {{
    console.log('✓ Firestore 更新成功');
    process.exit(0);
  }})
  .catch((error) => {{
    console.error('❌ Firestore 更新失敗:', error);
    process.exit(1);
  }});
"""

    script_path = flutter_dir / "temp_firestore_update.js"

    try:
        # 檢查 Node.js 是否可用
        if not check_command('node'):
            raise FileNotFoundError("未安裝 Node.js")

        # 寫入 Node.js 腳本
        with open(script_path, 'w', encoding='utf-8') as f:
            f.write(node_script)

        print_color("使用 Node.js 更新 Firestore...", Colors.YELLOW)

        # 執行 Node.js 腳本
        result = subprocess.run(
            ['node', 'temp_firestore_update.js'],
            cwd=str(flutter_dir),
            capture_output=True,
            text=True
        )

        if result.returncode == 0:
            print_color("✓ Firestore 更新成功", Colors.GREEN)
            return True
        else:
            print_color("❌ Node.js 更新失敗", Colors.RED)
            print_color(result.stderr, Colors.RED)
    except Exception as e:
        print_color(f"❌ Node.js 更新失敗: {e}", Colors.RED)
    finally:
        # 清理臨時檔案
        if script_path.exists():
            script_path.unlink()

    # 方法3: 手動更新提示
    print_color("\n⚠ 自動更新 Firestore 失敗，請手動更新", Colors.YELLOW)
    print_color("\n手動更新步驟:", Colors.WHITE)
    print_color("1. 開啟 Firebase Console: https://console.firebase.google.com/project/hoctrl/firestore", Colors.WHITE)
    print_color(f"2. 進入集合: firmware_updates", Colors.WHITE)
    print_color(f"3. 編輯或新增文件 ID: {model}", Colors.WHITE)
    print_color("4. 設定以下欄位:", Colors.WHITE)
    print_color(f"   - version: {version}", Colors.GRAY)
    print_color(f"   - download_url: {download_url}", Colors.GRAY)
    print_color(f"   - changelog: {changelog}", Colors.GRAY)
    print_color(f"   - min_version: {min_version}", Colors.GRAY)
    print_color(f"   - publish_time: (使用 Timestamp.now())", Colors.GRAY)

    return False

def main():
    """主函數"""
    parser = argparse.ArgumentParser(description='hoRelay2 韌體發布自動化腳本')
    parser.add_argument('-c', '--changelog', help='更新說明')
    parser.add_argument('-m', '--min-version', default='1.0.0', help='最低版本要求')
    parser.add_argument('-y', '--yes', action='store_true', help='跳過確認直接發布')
    args = parser.parse_args()
    
    print_color("\n╔════════════════════════════════════════╗", Colors.CYAN)
    print_color("║   hoRelay2 韌體發布自動化腳本        ║", Colors.CYAN)
    print_color("╚════════════════════════════════════════╝\n", Colors.CYAN)
    
    # 檢查必要工具
    if not check_requirements():
        print_color("\n❌ 缺少必要工具，無法繼續", Colors.RED)
        sys.exit(1)
    
    # 讀取韌體資訊
    firmware_info = get_firmware_info()
    if not firmware_info:
        print_color("\n❌ 無法讀取韌體資訊", Colors.RED)
        sys.exit(1)
    
    version = firmware_info['version']
    model = firmware_info['model']
    
    # 確認更新資訊
    print_header("確認更新資訊")
    print_color(f"設備型號: {model}", Colors.WHITE)
    print_color(f"韌體版本: {version}", Colors.WHITE)
    print_color(f"最低版本: {args.min_version}", Colors.WHITE)
    
    # 取得更新說明
    changelog = args.changelog
    if not changelog:
        changelog = "修正錯誤，優化效能"
        print_color(f"使用預設更新說明: {changelog}", Colors.GRAY)

    print_color(f"\n更新說明:\n{changelog}", Colors.WHITE)
    
    # 編譯韌體
    bin_path = build_firmware(model)
    if not bin_path:
        print_color("\n❌ 編譯失敗，無法繼續", Colors.RED)
        sys.exit(1)
    
    # 上傳到 Firebase 或 GitHub
    download_url = upload_to_firebase(bin_path, model, version, changelog)
    if not download_url:
        print_color("\n❌ 上傳失敗，無法繼續", Colors.RED)
        sys.exit(1)
    
    # 更新 Firestore
    update_firestore(model, version, download_url, changelog, args.min_version)
    
    # 完成
    print_color("\n╔════════════════════════════════════════╗", Colors.GREEN)
    print_color("║        ✓ 韌體發布完成！              ║", Colors.GREEN)
    print_color("╚════════════════════════════════════════╝", Colors.GREEN)
    print_color(f"\n版本: {version}", Colors.WHITE)
    print_color(f"下載 URL: {download_url}", Colors.WHITE)
    print_color("\n設備將在下次連線時收到更新通知\n", Colors.YELLOW)

if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print_color("\n\n已取消", Colors.YELLOW)
        sys.exit(0)
    except Exception as e:
        print_color(f"\n❌ 發生錯誤: {e}", Colors.RED)
        import traceback
        traceback.print_exc()
        sys.exit(1)

