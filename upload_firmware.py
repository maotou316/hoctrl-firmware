#!/usr/bin/env python3
"""
簡單的韌體上傳腳本
"""
from google.cloud import storage
from google.oauth2 import service_account

# 使用 service account 認證
creds = service_account.Credentials.from_service_account_file('serviceAccountKey.json')
client = storage.Client(credentials=creds, project='hoctrl')

bucket_name = 'hoctrl.firebasestorage.app'

print(f'檢查 bucket: {bucket_name}')
try:
    bucket = client.get_bucket(bucket_name)
    print(f'✓ Bucket 已存在: {bucket_name}')
except Exception as e:
    print(f'Bucket 不存在，正在建立...')
    try:
        bucket = client.create_bucket(bucket_name, location='asia-east1')
        print(f'✓ Bucket 建立成功: {bucket.name}')
    except Exception as create_err:
        print(f'建立失敗: {create_err}')
        print('改用預設 bucket...')
        bucket_name = 'hoctrl.appspot.com'
        bucket = client.get_bucket(bucket_name)

# 上傳檔案
storage_path = 'firmware/hoRelay2/hoRelay2_v1.2.2.bin'
local_file = 'build/ho_relay2.ino.bin'

print(f'正在上傳 {local_file} 到 {storage_path}...')
blob = bucket.blob(storage_path)
blob.upload_from_filename(local_file)

# 設為公開
blob.make_public()

print(f'✓ 上傳成功')
print(f'URL: {blob.public_url}')
