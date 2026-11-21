#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
install_music.py — 把测试音乐写进 ESP32 内部 5MB FAT 分区 (/flash/music)

通过固件新增的 XTC cmd=14 (FILE_IO) 命令分块传输：
  op=open / write(data_b64) / close / mkdir / stat
写操作在固件的内部 RAM 栈任务里执行，走 ESP-IDF VFS + wear_levelling，
规避 MicroPython 直接写 /flash 会挂死的问题。

用法:
    python install_music.py --port COM6 --mp3 <path.mp3>
    python install_music.py --port COM6 --verify-only
"""
import argparse
import base64
import json
import os
import sys
import time

import serial

REMOTE_DIR = "/flash/music"
REMOTE_NAME = "renshijian.mp3"       # ASCII 文件名，规避 FATFS LFN/编码差异
CHUNK = 1800                          # 每块原始字节数（base64 后 ~2400 字符）


class Dev:
    def __init__(self, port, baud=115200):
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.rx = b""
        self.nid = 5000
        try:
            self.ser.setDTR(True)
            self.ser.setRTS(False)
        except Exception:
            pass

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def _pump(self, secs):
        out = []
        end = time.monotonic() + secs
        while time.monotonic() < end:
            if self.ser.in_waiting:
                self.rx += self.ser.read(self.ser.in_waiting)
            while b"\n" in self.rx:
                raw, self.rx = self.rx.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip("\r")
                if line.startswith("XTR:"):
                    try:
                        out.append(json.loads(line[4:]))
                    except json.JSONDecodeError:
                        pass
            time.sleep(0.005)
        return out

    def cmd(self, params, timeout=35):
        cid = self.nid
        self.nid += 1
        wire = "XTC:" + json.dumps({"cmd": 14, "id": cid, "params": params},
                                   separators=(",", ":")) + "\n"
        self.ser.write(wire.encode("utf-8"))
        self.ser.flush()
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for r in self._pump(0.02):
                if r.get("id") == cid:
                    return r
        return None

    def ping(self, tries=20):
        for _ in range(tries):
            cid = self.nid
            self.nid += 1
            wire = "XTC:" + json.dumps({"cmd": 0, "id": cid}, separators=(",", ":")) + "\n"
            self.ser.write(wire.encode())
            end = time.monotonic() + 1.0
            while time.monotonic() < end:
                for r in self._pump(0.02):
                    if r.get("id") == cid and r.get("msg") == "pong":
                        return True
            time.sleep(0.3)
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM6")
    ap.add_argument("--mp3", help="本地 mp3/wav 源文件")
    ap.add_argument("--name", default=REMOTE_NAME, help="设备上的目标文件名（默认 renshijian.mp3）")
    ap.add_argument("--verify-only", action="store_true")
    args = ap.parse_args()

    d = Dev(args.port)
    if not d.ping():
        print("[ERR] 设备无 PONG，请确认 COM6 与固件已烧录")
        d.close()
        sys.exit(1)
    print("[OK] 设备在线")

    remote = REMOTE_DIR + "/" + args.name

    if args.verify_only:
        r = d.cmd({"op": "stat", "path": remote})
        print("[STAT]", json.dumps(r, ensure_ascii=False))
        d.close()
        return

    if not args.mp3 or not os.path.exists(args.mp3):
        print("[ERR] 未找到 mp3 文件:", args.mp3)
        d.close()
        sys.exit(1)

    data = open(args.mp3, "rb").read()
    print(f"[INFO] 源文件 {len(data)} bytes -> {remote}")

    # 建目录（已存在会返回 -17/EEXIST，忽略）
    r = d.cmd({"op": "mkdir", "path": REMOTE_DIR})
    print("[MKDIR]", json.dumps(r, ensure_ascii=False))

    # 打开（清空）
    r = d.cmd({"op": "open", "path": remote, "mode": "w"})
    print("[OPEN]", json.dumps(r, ensure_ascii=False))
    if not r or r.get("status") != "ok":
        print("[ERR] open 失败")
        d.close()
        sys.exit(1)

    # 分块写入
    total = 0
    nblk = (len(data) + CHUNK - 1) // CHUNK
    for i in range(nblk):
        chunk = data[i * CHUNK:(i + 1) * CHUNK]
        b64 = base64.b64encode(chunk).decode("ascii")
        r = d.cmd({"op": "write", "data_b64": b64})
        if not r or r.get("status") != "ok":
            print(f"[ERR] write 块 {i+1}/{nblk} 失败:", r)
            d.close()
            sys.exit(1)
        total += r.get("written", len(chunk))
        if (i + 1) % 10 == 0 or i + 1 == nblk:
            print(f"  写入 {i+1}/{nblk} 块, {total}/{len(data)} bytes")

    r = d.cmd({"op": "close"})
    print("[CLOSE]", json.dumps(r, ensure_ascii=False))

    # 验证大小
    r = d.cmd({"op": "stat", "path": remote})
    print("[VERIFY]", json.dumps(r, ensure_ascii=False))
    if r and r.get("status") == "ok" and r.get("size") == len(data):
        print("[PASS] 音乐已写入内部 FAT，大小一致")
    else:
        print("[WARN] 大小校验未通过，请人工确认")

    d.close()


if __name__ == "__main__":
    main()
