#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
XiaoMiao Desktop OS — 实机内存压测 (mem_stress.py)

目标：在 COM6 上定位「哪类操作会让堆持续增长/下降」的具体路径，输出诊断证据。

=== 实机探测结论（2026-11-21，固件 v71.1 / commit 24bdd6db）===
  * GET_INFO (cmd1) 响应 data 仅含静态设备信息，**不含任何堆字段**。
  * 无隐藏诊断命令：cmd14/15/16/20/99 均返回 {"status":"error","msg":"Unknown command"}。
  * EXEC_PYTHON (cmd4) 在本固件为 **no-op**：脚本根本不执行
    （实测 time.sleep(3) 仅耗时 0.14s；print/stdout/stderr/raise 均无输出，
     ret 恒为 0、output 恒为空）。因此无法用 MicroPython 的 gc.mem_free() 读堆。
  => get_heap() 按任务要求记录 None 并说明原因；改用以下代理证据定位泄漏：
       - 设备重启/崩溃日志（rst:0x / SW_CPU_RESET / Guru Meditation / Backtrace / stack overflow）
       - 内存分配失败日志中的 "mem free=NNN" 快照（固件各模块在分配失败时打印）
       - 每轮后 PING 是否存活
       - 每条路径能坚持的轮数

用法:
    python tests/mem_stress.py --port COM6 --rounds 12 --budget 420

输出:
    tests/mem_stress_report.txt （诊断报告）+ 终端关键摘要
"""

import argparse
import json
import os
import re
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, SCRIPT_DIR)

# 复用 test_full.py 中已验证的 JSON-over-UART 传输层，保持协议行为完全一致。
from test_full import Device  # noqa: E402

# GET_INFO 中可能承载堆数值的字段名（按优先级）。当前固件一个都不存在。
HEAP_KEYS = (
    # 新版固件 (commit 8996dec1+) GET_INFO 真实字段名，优先采样内部 DRAM（最易耗尽）
    "heap_internal_free",
    "heap_internal_min_free_bytes",
    "heap_free_bytes",
    "psram_free_bytes",
    "heap_min_free_bytes",
    "heap_largest_block",
    # 兼容其它命名约定
    "heap", "heap_free", "free_heap", "free", "free_bytes",
    "internal", "heap_internal", "free_internal",
    "psram", "heap_psram", "free_psram",
)

# 日志中「内存分配失败」的标志；用于抓取 mem free=NNN 及判定异常。
CRASH_PAT = re.compile(
    r"rst:0x|SW_CPU_RESET|Guru Meditation|Backtrace:|"
    r"stack overflow|Stack overflow|abort\(\)", re.I)
MEMWARN_PAT = re.compile(
    r"mem free\s*=\s*\d+|malloc failed|heap_caps_malloc failed|"
    r"Failed to allocate|MemoryError|out of memory", re.I)
MEMFREE_PAT = re.compile(r"mem free\s*=\s*(\d+)", re.I)


class MemStress:
    def __init__(self, port="COM6", baud=115200, rounds=12, budget=420,
                 min_heap=20 * 1024):
        self.dev = Device(port, baud)
        self.rounds = rounds
        self.budget = budget
        self.min_heap = min_heap
        self.start = time.monotonic()
        self.report = []          # 报告文本行
        self.results = []         # 每条路径的汇总 dict
        self.total_reboots = 0
        self.total_memwarn = 0
        self.mem_free_samples = []
        self.crash_evidence = []      # 每次崩溃的完整栈证据
        self.aborted = False      # 全局预算耗尽/设备失联

    # ------------------------------------------------------------------
    # 基础设施
    # ------------------------------------------------------------------
    def emit(self, line=""):
        print(line)
        self.report.append(str(line))

    def budget_left(self):
        return self.budget - (time.monotonic() - self.start)

    def connect(self):
        self.dev.connect()
        ok = False
        for _ in range(8):
            try:
                r = self.dev.send_cmd(0, timeout=3)
            except Exception:
                r = None
            if r and r.get("msg") == "pong":
                ok = True
                break
            time.sleep(0.5)
        if not ok:
            raise RuntimeError("PING 无响应：设备未就绪或串口被占用")
        self.dev._pump(0.5)
        self.emit("[连接] %s @ 115200，PING 就绪" % self.dev.port)
        # 记录 GET_INFO 原始结构（交付项 1）
        info = self.dev.send_cmd(1, timeout=5)
        self.emit("[GET_INFO 原始响应] " + json.dumps(info, ensure_ascii=False))
        heap_found = []
        if info and isinstance(info.get("data"), dict):
            d = info["data"]
            self.emit("[GET_INFO data 字段] " + ",".join(d.keys()))
            heap_found = [("%s=%s" % (k, d[k])) for k in HEAP_KEYS if k in d]
        if heap_found:
            self.emit("[GET_INFO 堆字段] " + ", ".join(heap_found))
        else:
            self.emit("[GET_INFO 堆字段] 未发现（data 中无任何 heap/free/mem 字段）")
        return self

    def get_heap(self, log_from=0):
        """采样堆。无堆字段时返回 (None, 说明)，并按任务要求记录原因。"""
        val = None
        note = ""
        try:
            r = self.dev.send_cmd(1, timeout=5)
        except Exception as e:
            r = None
            note = "GET_INFO 异常:%s" % e
        if r and isinstance(r.get("data"), dict):
            data = r["data"]
            for k in HEAP_KEYS:
                if isinstance(data.get(k), (int, float)):
                    val = int(data[k])
                    note = "GET_INFO.%s" % k
                    break
            if val is None:
                note = "GET_INFO 无堆字段(字段:%s)" % ",".join(data.keys())
        elif not note:
            note = "GET_INFO 无有效响应"

        # 从本轮新增日志里抓 mem free=NNN 快照（固件分配失败时打印）
        new_log = "\n".join(self.dev.log_lines[log_from:])
        for m in MEMFREE_PAT.findall(new_log):
            try:
                self.mem_free_samples.append(int(m))
            except ValueError:
                pass
        return val, note

    def recover(self, max_wait=35):
        """设备重启后等待恢复。

        崩溃会触发 USB CDC 重新枚举，Windows 下旧串口句柄随之失效
        （WriteFile failed / PermissionError 13 '设备不识别此命令'）。
        因此先 close+connect 重建串口，再等待连续 2 次 PONG 且 GET_INFO 有效。
        """
        deadline = time.monotonic() + max_wait
        pongs = 0
        try:
            self.dev.close()
        except Exception:
            pass
        reopened = False
        while time.monotonic() < deadline:
            if not reopened:
                try:
                    self.dev.connect()
                    reopened = True
                    time.sleep(0.3)
                except Exception:
                    time.sleep(0.5)
                    continue
            try:
                r = self.dev.send_cmd(0, timeout=2)
            except Exception:
                r = None
            if r and r.get("msg") == "pong":
                pongs += 1
                if pongs >= 2:
                    info = self.dev.send_cmd(1, timeout=2)
                    if info and info.get("status") == "ok":
                        self.dev._pump(0.3)
                        return True
            else:
                pongs = 0
            time.sleep(0.5)
        return False

    def health(self, log_from):
        """检查本轮是否发生重启/内存告警/PING 失联。返回 (alive, events, crash_lines)。

        crash_lines 为完整崩溃栈证据（Guru Meditation / Backtrace / rst），供报告引用。
        """
        new_lines = self.dev.log_lines[log_from:]
        new_log = "\n".join(new_lines)
        events = []
        crash_lines = [l for l in new_lines if CRASH_PAT.search(l)]
        if crash_lines:
            events.append("REBOOT/CRASH")
            self.total_reboots += 1
            # 保存完整崩溃证据（最多 12 行：panic 头 + Backtrace + rst）
            self.crash_evidence.append(crash_lines[:12])
        if MEMWARN_PAT.search(new_log):
            events.append("MEM-WARN")
            self.total_memwarn += 1
        alive = True
        try:
            pong = self.dev.send_cmd(0, timeout=3)
            if not (pong and pong.get("msg") == "pong"):
                events.append("PING-FAIL")
                alive = False
        except Exception:
            events.append("PING-FAIL")
            alive = False
        return alive, events, crash_lines

    # ------------------------------------------------------------------
    # 各操作路径的单轮动作
    # ------------------------------------------------------------------
    def op_simulate_key(self, i):
        sent = 0
        # 遍历 6 个键（左右/确认/返回等组合），穿插长按，模拟进入/退出 app
        seq = [0, 4, 5, 3, 1, 2]
        if i % 3 == 0:
            seq = [5, 3, 4, 0, 5, 3]   # 确认->返回->右 的菜单进出组合
        for k in seq:
            r = self.dev.send_cmd(5, {"key": k, "long": False}, timeout=3)
            sent += 1
        if i % 4 == 0:
            self.dev.send_cmd(5, {"key": 0, "long": True}, timeout=3)
            sent += 1
        return "%d keys" % sent

    def op_play_tone(self, i):
        freqs = [220, 330, 440, 660, 880, 1175, 1568, 1760]
        f = freqs[i % len(freqs)]
        r1 = self.dev.send_cmd(8, {"freq": f, "duration": 60}, timeout=5)
        r2 = self.dev.send_cmd(8, {"freq": f // 2, "duration": 40}, timeout=5)
        return "%dHz" % f

    def op_led_backlight(self, i):
        r = i % 255
        g = (i * 37) % 255
        b = (i * 91) % 255
        self.dev.send_cmd(6, {"index": i % 3, "r": r, "g": g, "b": b}, timeout=3)
        self.dev.send_cmd(7, {"brightness": 20 + (i * 17) % 80}, timeout=3)
        return "rgb(%d,%d,%d)" % (r, g, b)

    def op_exec_python(self, i):
        # 已实测为 no-op；仍按任务要求覆盖该路径，记录其无效性。
        self.dev.send_cmd(4, {"script": "print('stress %d')\n" % i}, timeout=5)
        return "script#%d" % i

    def op_run_test(self, i):
        # 三项：display=1 / audio=2 / micropython=6，等待每项进入终态
        done = []
        for item, name in ((1, "display"), (2, "audio"), (6, "mpy")):
            r = self.dev.send_cmd(2, {"item": item}, timeout=8)
            if not (r and r.get("status") in ("started", "ok")):
                done.append(name + ":NOSTART")
                continue
            time.sleep(0.4)
            deadline = time.monotonic() + 25
            state = None
            while time.monotonic() < deadline:
                rr = self.dev.send_cmd(3, {"item": item}, timeout=5)
                d = rr.get("data") if rr else None
                if isinstance(d, list) and d:
                    state = d[0].get("state")
                    if state in (2, 3):
                        break
                time.sleep(0.3)
            done.append("%s:%s" % (name, state))
        return " ".join(done)

    def op_wifi_scan(self, i):
        self.dev.clear_log()
        r = self.dev.send_cmd(13, timeout=8, terminal_status="ok")
        # 扫描在 system event 任务异步完成（实测约 2-4s，日志刷屏需 pump 更久）
        self.dev._pump(6.0)
        log = self.dev.log_text()
        scan_done = "WiFi scan done" in log or "WiFi scan complete" in log
        return "scan_ack=%s done=%s" % (
            r.get("status") if r else "?", scan_done)

    # ------------------------------------------------------------------
    # 压测主循环
    # ------------------------------------------------------------------
    def run_path(self, key, title, op, rounds=None):
        self.emit("\n" + "=" * 68)
        self.emit("路径 %s: %s" % (key, title))
        self.emit("=" * 68)
        rounds = rounds or self.rounds
        heaps = []
        notes = []
        completed = 0
        reason = "完成全部 %d 轮" % rounds
        for i in range(1, rounds + 1):
            if self.budget_left() < 20:
                reason = "全局时间预算耗尽"
                self.aborted = True
                break
            log_from = len(self.dev.log_lines)
            try:
                action = op(i)
            except Exception as e:
                self.emit("轮次 %d: 操作异常 %s" % (i, e))
                reason = "操作异常:%s" % e
                break
            heap, note = self.get_heap(log_from)
            alive, events, crash_lines = self.health(log_from)
            heaps.append(heap)
            notes.append(note)
            ev = (" | 事件:" + ",".join(events)) if events else ""
            self.emit("轮次 %d: heap=%s (%s) %s%s" %
                      (i, heap if heap is not None else "None", note, action, ev))
            completed = i
            # 终止条件：堆跌破阈值 / 设备重启或失联
            if heap is not None and heap < self.min_heap:
                reason = "堆跌破 %d 字节" % self.min_heap
                break
            if not alive or "REBOOT/CRASH" in events:
                reason = "设备重启/失联(第%d轮)" % i
                if crash_lines:
                    self.emit("   崩溃证据: " + " / ".join(l[:120] for l in crash_lines[:3]))
                # 等待设备恢复，保证后续路径可继续采样
                self.recover()
                break

        valid = [h for h in heaps if h is not None]
        start_h = valid[0] if valid else None
        end_h = valid[-1] if valid else None
        min_h = min(valid) if valid else None
        drop = (start_h - end_h) if (start_h is not None and end_h is not None) else None
        suspicious = (drop is not None and drop > 2048)
        row = {
            "key": key, "title": title, "completed": completed, "planned": rounds,
            "start": start_h, "end": end_h, "min": min_h, "drop": drop,
            "suspicious": suspicious, "reason": reason,
            "heap_available": bool(valid), "note": notes[-1] if notes else "",
        }
        self.results.append(row)
        self.emit("小结: 完成 %d/%d 轮, 堆 %s->%s (最小 %s, 净降 %s), 原因:%s" %
                  (completed, rounds,
                   start_h if start_h is not None else "N/A",
                   end_h if end_h is not None else "N/A",
                   min_h if min_h is not None else "N/A",
                   drop if drop is not None else "N/A", reason))
        return row

    def summary(self):
        self.emit("\n" + "#" * 68)
        self.emit("# 汇总表")
        self.emit("#" * 68)
        hdr = "%-4s %-26s %6s %8s %8s %8s %8s %-10s %s" % (
            "路径", "描述", "轮次", "起始堆", "结束堆", "最小堆", "净下降", "可疑", "终止原因")
        self.emit(hdr)
        for r in self.results:
            fmt = lambda v: (str(v) if v is not None else "N/A")
            self.emit("%-4s %-26s %6s %8s %8s %8s %8s %-10s %s" % (
                r["key"], r["title"][:26],
                "%d/%d" % (r["completed"], r["planned"]),
                fmt(r["start"]), fmt(r["end"]), fmt(r["min"]), fmt(r["drop"]),
                "是" if r["suspicious"] else "否", r["reason"]))
        self.emit("")
        self.emit("[堆可读性] GET_INFO 无堆字段 / EXEC_PYTHON 为 no-op -> 所有路径 heap=None；")
        self.emit("           净下降无法直接测量，改以重启次数、mem free 告警、存活轮数作为泄漏代理证据。")
        self.emit("[重启/崩溃总次数] %d" % self.total_reboots)
        self.emit("[内存告警总次数] %d" % self.total_memwarn)
        if self.mem_free_samples:
            self.emit("[mem free 快照] 样本数=%d 最小=%d 最大=%d" %
                      (len(self.mem_free_samples), min(self.mem_free_samples),
                       max(self.mem_free_samples)))
            self.emit("             （注：这些是固件在分配失败时打印的堆余量，非逐轮采样）")
        else:
            self.emit("[mem free 快照] 无（未触发任何分配失败日志）")
        # 最可疑路径：重启优先，其次内存告警
        suspects = sorted(
            self.results,
            key=lambda r: (r["completed"] < r["planned"], r["reason"]),
            reverse=True)
        self.emit("\n[重点观察]")
        for r in suspects[:3]:
            self.emit("  - %s %s: 完成 %d/%d, 原因=%s" %
                      (r["key"], r["title"], r["completed"], r["planned"], r["reason"]))
        self.emit("\n[压测期间设备状态] %s" %
                  ("发生 %d 次重启/崩溃" % self.total_reboots if self.total_reboots
                   else "全程未观察到重启/崩溃；设备保持响应"))
        if self.crash_evidence:
            self.emit("\n[崩溃栈证据] 共 %d 次：" % len(self.crash_evidence))
            for n, ev in enumerate(self.crash_evidence[:6], 1):
                self.emit("  --- 第 %d 次崩溃 ---" % n)
                for l in ev:
                    self.emit("    " + l[:160])

    def save_report(self, path):
        with open(path, "w", encoding="utf-8") as f:
            f.write("XiaoMiao Desktop OS — 实机内存压测报告\n")
            f.write("生成时间: %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
            f.write("=" * 68 + "\n\n")
            f.write("\n".join(self.report) + "\n")
        self.emit("\n[报告已写入] %s" % path)


def main():
    ap = argparse.ArgumentParser(description="XiaoMiao OS 内存压测")
    ap.add_argument("--port", "-p", default="COM6")
    ap.add_argument("--baud", "-b", type=int, default=115200)
    ap.add_argument("--rounds", "-n", type=int, default=12,
                    help="每条路径轮数（WiFi 固定 3 次）")
    ap.add_argument("--budget", type=int, default=420,
                    help="总时长预算（秒），默认 7 分钟")
    args = ap.parse_args()

    stress = MemStress(args.port, args.baud, args.rounds, args.budget)
    try:
        stress.connect()
        # a) 反复 SIMULATE_KEY 进出 app
        stress.run_path("a", "SIMULATE_KEY 进出app", stress.op_simulate_key)
        # b) 反复 PLAY_TONE 不同频率
        stress.run_path("b", "PLAY_TONE 多频率", stress.op_play_tone)
        # c) 反复 SET_LED / SET_BACKLIGHT
        stress.run_path("c", "SET_LED/BACKLIGHT", stress.op_led_backlight)
        # d) 反复 EXEC_PYTHON
        stress.run_path("d", "EXEC_PYTHON", stress.op_exec_python)
        # e) 反复 RUN_TEST（audio/display/micropython）
        stress.run_path("e", "RUN_TEST 三项", stress.op_run_test,
                        rounds=min(8, args.rounds))
        # f) DIAG_WIFI_SCAN 3 次（历史崩溃路径，重点观察）
        stress.run_path("f", "DIAG_WIFI_SCAN x3", stress.op_wifi_scan, rounds=3)
        stress.summary()
    except KeyboardInterrupt:
        stress.emit("\n[中断] 用户 Ctrl-C")
    except Exception as e:
        stress.emit("\n[异常] %s" % e)
    finally:
        stress.save_report(os.path.join(SCRIPT_DIR, "mem_stress_report.txt"))
        try:
            stress.dev.close()
        except Exception:
            pass


if __name__ == "__main__":
    main()
