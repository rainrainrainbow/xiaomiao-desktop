#!/usr/bin/env python3
"""
应用签名生成工具 — 为 MicroPython 应用生成 app.json 中的 signature 字段。

用法：
    python3 sign_app.py <app_dir>
    
示例：
    python3 sign_app.py /sdcard/apps/my_app/
    
这将在 app.json 中添加或更新 signature 字段。

签名算法：FNV-1a 哈希
- 输入：name|icon|color|version|author
- 输出：8位大写十六进制字符串
"""

import os
import sys
import json
import struct


def fnv1a_hash(data: str) -> str:
    """计算 FNV-1a 哈希，返回8位大写十六进制字符串"""
    hash_val = 0x811C9DC5
    for c in data.encode('utf-8'):
        hash_val ^= c
        hash_val = (hash_val * 0x01000193) & 0xFFFFFFFF
    return format(hash_val, '08X')


def sign_app(app_dir: str) -> bool:
    """为指定目录下的 app.json 生成签名"""
    json_path = os.path.join(app_dir, 'app.json')
    
    if not os.path.isfile(json_path):
        print(f"❌ 未找到 {json_path}")
        return False
    
    with open(json_path, 'r', encoding='utf-8') as f:
        app_data = json.load(f)
    
    # 提取用于签名的字段
    name = app_data.get('name', '')
    icon = app_data.get('icon', '')
    color = app_data.get('color', '')
    version = app_data.get('version', '')
    author = app_data.get('author', '')
    
    # 构建签名字符串
    sig_data = f"{name}|{icon}|{color}|{version}|{author}"
    
    # 计算签名
    signature = fnv1a_hash(sig_data)
    
    # 更新 app.json
    app_data['signature'] = signature
    
    with open(json_path, 'w', encoding='utf-8') as f:
        json.dump(app_data, f, ensure_ascii=False, indent=2)
    
    print(f"✅ 签名已生成: {signature}")
    print(f"   数据: \"{sig_data}\"")
    print(f"   文件: {json_path}")
    return True


def main():
    if len(sys.argv) < 2:
        print("用法: python3 sign_app.py <app_dir>")
        print("示例: python3 sign_app.py ./examples/apps/clock.app/")
        sys.exit(1)
    
    app_dir = sys.argv[1]
    if not os.path.isdir(app_dir):
        print(f"❌ 目录不存在: {app_dir}")
        sys.exit(1)
    
    if sign_app(app_dir):
        print("\n💡 提示：将签名后的 app.json 和 main.py 一起放入 SD 卡的 /sdcard/apps/<app_name>/ 目录")
    else:
        sys.exit(1)


if __name__ == '__main__':
    main()