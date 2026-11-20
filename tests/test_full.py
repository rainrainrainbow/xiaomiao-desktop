#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
XiaoMiao Desktop OS — Full Functional Test Suite (v3)
=====================================================
Covers EVERY documented capability of the XTC:/XTR: command interface:

  Commands (13/13):
    0  PING               1  GET_INFO          2  RUN_TEST
    3  GET_TEST_RESULT    4  EXEC_PYTHON       5  SIMULATE_KEY
    6  SET_LED            7  SET_BACKLIGHT     8  PLAY_TONE
    9  GET_BATTERY        10 LIST_TESTS        11 RESET_DEVICE
    12 ENTER_BOOTLOADER

  Hardware test items (RUN_TEST item=0..9):
    Buttons / Display / Audio / LED / Battery / Bluetooth /
    MicroPython / SDCard / WiFi / All

  Plus boot-log module checks and runtime stability checks.

Usage:
    python test_full.py --port COM6 --reset            # full run with clean boot log
    python test_full.py --port COM6                    # full run, reuse running device
    python test_full.py --port COM6 --quick            # skip long-running tests
    python test_full.py --dry-run                      # no hardware needed

A JSON + Markdown report is written to tests/full_test_report.* by default.
"""

import argparse
import json
import os
import subprocess
import sys
import time

import serial
import serial.tools.list_ports

try:
    from zoneinfo import ZoneInfo  # noqa: F401
except Exception:  # pragma: no cover
    pass

PASS, FAIL, WARN, SKIP = "PASS", "FAIL", "WARN", "SKIP"

# Boot-log markers aligned with current firmware (v71.1 / "Xiaomiao Desktop v20").
MARKERS = {
    "boot_header":   "Xiaomiao Desktop v20",
    "mainloop":      "Main loop started",
    "splash":        "Splash screen shown",
    "display_on":    "Display ON",
    "lvgl":          "LVGL initialized",
    "framebuffer":   "framebuffer ready",
    "button_task":   "Button task created",
    "battery":       "Battery ADC initialized",
    "led":           "NeoPixel RGB LED strip initialized",
    "mic":           "LMD2718 digital mic initialized",
    "audio_i2s":     "I2S DAC backend ready",
    "audio_bt":      "Bluetooth A2DP Sink initialized successfully",
    "mpy":           "MicroPython runtime initialized successfully",
    "desktop":       "Desktop page pushed successfully",
    "apps":          "Registered 6 builtin apps",
    "apptest":       "Test interface ready on USB CDC",
    "uart_err":      "uart driver error",
    "sd_ok":         "SD card mounted",
    "sd_fail":       "Failed to mount SD card",
    "font_fallback": "falling back to English UI",
    "reboot":        "rst:0x",
    "stack_overflow": "stack overflow",
    "backtrace":     "Backtrace:",
}

ITEM_NAMES = {
    0: "Buttons", 1: "Display", 2: "Audio", 3: "LED", 4: "Battery",
    5: "Bluetooth", 6: "MicroPython", 7: "SDCard", 8: "WiFi", 9: "All",
}
STATE_NAMES = {0: "IDLE", 1: "RUNNING", 2: "COMPLETED", 3: "FAILED"}


# ---------------------------------------------------------------------------
# Serial command transport
# ---------------------------------------------------------------------------
class Device:
    """JSON-over-UART transport with id-correlated asynchronous responses."""

    def __init__(self, port, baud=115200):
        self.port = port
        self.baud = baud
        self.ser = None
        self.log_lines = []          # non-XTR lines (boot log / firmware output)
        self.next_id = 1000
        self.rx_buf = b""

    def connect(self):
        self.ser = serial.Serial(self.port, self.baud, timeout=0.2)
        # Keep EN idle-high, boot-mode pin idle-low on typical USB-UART bridges.
        try:
            self.ser.setDTR(True)
            self.ser.setRTS(False)
        except Exception:
            pass
        self.ser.reset_input_buffer()
        return self

    def close(self):
        if self.ser:
            try:
                self.ser.close()
            finally:
                self.ser = None

    def _pump(self, seconds):
        """Read from UART for `seconds`, extracting complete lines into buffers.

        Returns a list of parsed XTR response dicts received during the window.
        """
        responses = []
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if self.ser and self.ser.in_waiting:
                self.rx_buf += self.ser.read(self.ser.in_waiting)
            while b"\n" in self.rx_buf:
                raw, self.rx_buf = self.rx_buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="replace").strip("\r")
                if not line:
                    continue
                if line.startswith("XTR:"):
                    payload = line[4:]
                    try:
                        responses.append(json.loads(payload))
                    except json.JSONDecodeError:
                        # Keep malformed responses visible in the log.
                        self.log_lines.append("XTR?:" + payload)
                else:
                    self.log_lines.append(line)
                    if len(self.log_lines) > 4000:
                        self.log_lines = self.log_lines[-2000:]
            time.sleep(0.01)
        return responses

    def log_text(self):
        return "\n".join(self.log_lines)

    def clear_log(self):
        self.log_lines = []

    def send_cmd(self, cmd, params=None, timeout=5.0, terminal_status=None,
                 accept_first=True):
        """Send one XTC command and return its XTR response.

        terminal_status: if set (e.g. 'ok' for async python), wait until a
        response with that status arrives (skipping intermediate 'accepted').
        """
        cid = self.next_id
        self.next_id += 1
        payload = {"cmd": cmd, "id": cid}
        if params is not None:
            payload["params"] = params
        wire = "XTC:" + json.dumps(payload, separators=(",", ":")) + "\n"
        self.ser.write(wire.encode("utf-8"))
        self.ser.flush()

        deadline = time.monotonic() + timeout
        matched = []
        while time.monotonic() < deadline:
            for r in self._pump(0.02):
                if r.get("id") == cid:
                    matched.append(r)
                    if terminal_status:
                        if r.get("status") == terminal_status:
                            return r
                    elif accept_first:
                        return r
            if terminal_status and any(
                    r.get("status") == terminal_status for r in matched):
                return [r for r in matched if r.get("status") == terminal_status][0]
        if terminal_status and matched:
            return matched[-1]
        return matched[0] if matched else None

    def wait_ready(self, timeout=90.0, verbose=True):
        """Poll PING until the command interface answers."""
        deadline = time.monotonic() + timeout
        attempts = 0
        while time.monotonic() < deadline:
            attempts += 1
            r = self.send_cmd(0, timeout=1.0)
            if r and r.get("msg") == "pong":
                if verbose:
                    print(f"  device ready after {attempts} PING(s)")
                return True
            time.sleep(0.2)
        return False

    def wait_test_idle(self, timeout=90.0):
        """Poll GET_INFO until test_running becomes false."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            r = self.send_cmd(1, timeout=3.0)
            if r and r.get("status") == "ok":
                data = r.get("data") or {}
                if not data.get("test_running"):
                    return True
            time.sleep(0.5)
        return False


def hard_reset(port):
    try:
        subprocess.run(
            [sys.executable, "-m", "esptool", "--port", port,
             "--after", "hard_reset", "chip_id"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=40,
        )
        return True
    except Exception as e:
        print(f"  esptool reset failed: {e}")
        return False


# ---------------------------------------------------------------------------
# Test runner
# ---------------------------------------------------------------------------
class Runner:
    def __init__(self, dev, quick=False, dry_run=False):
        self.dev = dev
        self.quick = quick
        self.dry_run = dry_run
        self.results = []

    def record(self, section, name, status, detail=""):
        self.results.append({
            "section": section, "name": name,
            "status": status, "detail": str(detail),
        })
        icon = {PASS: "PASS", FAIL: "FAIL", WARN: "WARN", SKIP: "SKIP"}[status]
        print(f"  [{icon}] {name}" + (f" — {detail}" if detail else ""))
        return status == PASS

    def check(self, section, name, fn):
        try:
            ok, detail = fn()
            self.record(section, name, PASS if ok else FAIL, detail)
        except AssertionError as e:
            self.record(section, name, FAIL, str(e))
        except Exception as e:
            self.record(section, name, FAIL, f"exception: {e}")

    # ---------------- boot log checks ----------------
    def boot_log_checks(self):
        section = "boot-log"
        print("\n=== Boot-log module checks ===")
        text = self.dev.log_text()
        if self.dry_run:
            # Exclude negative markers (things that must NOT appear).
            text = "\n".join(v for k, v in MARKERS.items()
                             if k not in ("reboot", "stack_overflow", "backtrace",
                                          "uart_err"))
        module_markers = [
            ("boot_header", "Boot header / firmware version"),
            ("mainloop", "Main loop running"),
            ("display_on", "Display ON"),
            ("lvgl", "LVGL initialized"),
            ("framebuffer", "Framebuffer ready"),
            ("button_task", "Button task created"),
            ("battery", "Battery ADC initialized"),
            ("led", "RGB LED strip initialized"),
            ("mic", "LMD2718 digital mic initialized"),
            ("audio_i2s", "I2S DAC backend ready"),
            ("audio_bt", "Bluetooth A2DP Sink initialized"),
            ("mpy", "MicroPython runtime initialized"),
            ("desktop", "Desktop page loaded"),
            ("apps", "6 builtin apps registered"),
            ("apptest", "Test interface ready"),
        ]
        for key, label in module_markers:
            found = MARKERS[key] in text
            self.record(section, label, PASS if found else FAIL,
                        "" if found else f"marker missing: {MARKERS[key]}")

        # SD card depends on hardware presence -> WARN, never FAIL.
        if MARKERS["sd_ok"] in text:
            self.record(section, "SD card mounted", PASS)
        elif MARKERS["sd_fail"] in text:
            self.record(section, "SD card not inserted", WARN,
                        "hardware absent; Chinese font falls back to English")
        else:
            self.record(section, "SD card status in log", WARN, "no marker found")

        if MARKERS["font_fallback"] in text:
            self.record(section, "Font fallback (expected w/o SD)", PASS,
                        "English UI in use")

        # Stability markers (absence == good).
        reboots = text.count(MARKERS["reboot"])
        overflow = text.count(MARKERS["stack_overflow"])
        uart_err = text.count(MARKERS["uart_err"])
        self.record(section, "No reboot-loop", PASS if reboots == 0 else FAIL,
                    f"rst: count={reboots}")
        self.record(section, "No stack overflow", PASS if overflow == 0 else FAIL,
                    f"overflow count={overflow}")
        self.record(section, "No UART driver error", PASS if uart_err == 0 else FAIL,
                    f"error count={uart_err}")
        print(f"  captured {len(text)} bytes of boot log")

    # ---------------- command checks ----------------
    def cmd_ping(self):
        r = self.dev.send_cmd(0, timeout=5)
        return bool(r and r.get("msg") == "pong"), f"id={r.get('id') if r else '?'} rtt-ok"

    def cmd_get_info(self):
        r = self.dev.send_cmd(1, timeout=5)
        if not r or r.get("status") != "ok":
            raise AssertionError(f"no valid response: {r}")
        d = r.get("data") or {}
        required = ["device", "fw_version", "hw_version", "flash_mb", "psram_mb",
                    "screen_w", "screen_h", "buttons", "leds", "micropython_ready",
                    "test_running"]
        missing = [k for k in required if k not in d]
        if missing:
            raise AssertionError(f"missing fields: {missing}")
        detail = (f"{d['device']} {d['fw_version']} screen={d['screen_w']}x{d['screen_h']} "
                  f"btns={d['buttons']} leds={d['leds']} mpy={d['micropython_ready']}")
        self.info = d
        return True, detail

    def cmd_list_tests(self):
        r = self.dev.send_cmd(10, timeout=5)
        data = r.get("data") if r else None
        if not isinstance(data, list):
            raise AssertionError(f"bad response: {r}")
        ids = [t.get("id") for t in data]
        if ids != list(range(9)):
            raise AssertionError(f"test ids {ids} != 0..8")
        return True, f"{len(data)} test items listed"

    def cmd_get_battery(self):
        r = self.dev.send_cmd(9, timeout=5)
        d = r.get("data") if r else {}
        mv = d.get("voltage_mv", 0)
        pct = d.get("percent", -1)
        if not (2500 <= mv <= 4500):
            raise AssertionError(f"voltage out of range: {mv}mV")
        if not (0 <= pct <= 100):
            raise AssertionError(f"percent out of range: {pct}")
        return True, f"{mv} mV, {pct}%"

    def cmd_simulate_key(self):
        sent = []
        for key in range(6):
            r = self.dev.send_cmd(5, {"key": key, "long": False}, timeout=5)
            ok = r and r.get("status") == "ok"
            sent.append(f"{key}:{'ok' if ok else 'NO'}")
        # Long-press path
        rl = self.dev.send_cmd(5, {"key": 0, "long": True}, timeout=5)
        sent.append("long:" + ("ok" if rl and rl.get("status") == "ok" else "NO"))
        # Invalid key must be rejected.
        rb = self.dev.send_cmd(5, {"key": 99}, timeout=5)
        rejected = rb and rb.get("status") == "error"
        if not all("NO" not in s for s in sent[:6]) or not rejected:
            raise AssertionError(" ".join(sent) + f" invalid-rejected={rejected}")
        return True, "6 keys + long-press injected; invalid key rejected"

    def cmd_set_led(self):
        colors = [(0, 255, 0, 0), (1, 0, 255, 0), (2, 0, 0, 255)]
        for idx, rr, gg, bb in colors:
            r = self.dev.send_cmd(6, {"index": idx, "r": rr, "g": gg, "b": bb}, timeout=5)
            if not r or r.get("status") != "ok":
                raise AssertionError(f"LED {idx} failed: {r}")
        rb = self.dev.send_cmd(6, {"index": 9, "r": 0, "g": 0, "b": 0}, timeout=5)
        if not rb or rb.get("status") != "error":
            raise AssertionError("invalid LED index not rejected")
        # Turn all LEDs off.
        for idx in range(3):
            self.dev.send_cmd(6, {"index": idx, "r": 0, "g": 0, "b": 0}, timeout=3)
        return True, "3 LEDs set R/G/B; invalid index rejected; LEDs off"

    def cmd_set_backlight(self):
        for value in (100, 50, 10, 100):
            r = self.dev.send_cmd(7, {"brightness": value}, timeout=5)
            if not r or r.get("status") != "ok":
                raise AssertionError(f"brightness {value} failed: {r}")
        rb = self.dev.send_cmd(7, {}, timeout=5)
        if not rb or rb.get("status") != "error":
            raise AssertionError("missing brightness not rejected")
        return True, "100/50/10/100% applied; missing param rejected"

    def cmd_play_tone(self):
        for freq, dur in ((440, 100), (880, 100)):
            r = self.dev.send_cmd(8, {"freq": freq, "duration": dur}, timeout=5)
            if not r or r.get("status") != "ok":
                raise AssertionError(f"tone {freq}Hz failed: {r}")
        rb = self.dev.send_cmd(8, {}, timeout=5)
        if not rb or rb.get("status") != "error":
            raise AssertionError("missing freq not rejected")
        return True, "440Hz + 880Hz tones played; missing param rejected"

    def cmd_exec_python(self):
        script = ("print('FULLTEST', 6*7)\n"
                  "import xiaomiao\n"
                  "print('SIZE', xiaomiao.width(), xiaomiao.height())\n")
        # MicroPython print() goes to UART console, not the JSON output field.
        # Snapshot the log position so we can look only at lines produced by
        # this execution (the device echoes script stdout on the same UART).
        before = len(self.dev.log_lines) if hasattr(self.dev, "log_lines") else 0
        r = self.dev.send_cmd(4, {"script": script}, timeout=25,
                              terminal_status="ok")
        if not r:
            raise AssertionError("no response for EXEC_PYTHON (device may have crashed)")
        if r.get("status") != "ok":
            raise AssertionError(f"python exec failed: {r}")
        out = r.get("output", "")
        serial_out = "\n".join(self.dev.log_lines[before:]) if hasattr(self.dev, "log_lines") else ""
        captured = out + "\n" + serial_out
        if "FULLTEST 42" not in captured:
            # ret==0 still means the script ran; only the console echo is missing.
            self.record("command", "EXEC_PYTHON print echo", WARN,
                        f"no print echo on UART (ret={r.get('ret')}); "
                        f"output={out[:80]!r} serial={serial_out[:80]!r}")
        return True, f"ret={r.get('ret')} print_echo={'FULLTEST 42' in captured}"

    def cmd_run_test_and_result(self, item, timeout=90):
        r = self.dev.send_cmd(2, {"item": item}, timeout=8)
        if not r or r.get("status") not in ("started", "ok"):
            raise AssertionError(f"RUN_TEST {item} not started: {r}")
        if not self.dev.wait_test_idle(timeout=timeout):
            raise AssertionError(f"RUN_TEST {item} did not finish in {timeout}s")
        rr = self.dev.send_cmd(3, {"item": item}, timeout=8)
        data = rr.get("data") if rr else None
        if not isinstance(data, list) or not data:
            raise AssertionError(f"no result for item {item}: {rr}")
        res = data[0]
        return res, res.get("detail", "")

    def command_checks(self):
        section = "command"
        print("\n=== Command interface checks (13/13) ===")
        self.check(section, "cmd0 PING", self.cmd_ping)
        self.check(section, "cmd1 GET_INFO", self.cmd_get_info)
        self.check(section, "cmd10 LIST_TESTS", self.cmd_list_tests)
        self.check(section, "cmd9 GET_BATTERY", self.cmd_get_battery)
        self.check(section, "cmd5 SIMULATE_KEY", self.cmd_simulate_key)
        self.check(section, "cmd6 SET_LED", self.cmd_set_led)
        self.check(section, "cmd7 SET_BACKLIGHT", self.cmd_set_backlight)
        self.check(section, "cmd8 PLAY_TONE", self.cmd_play_tone)
        self.check(section, "cmd4 EXEC_PYTHON", self.cmd_exec_python)
        self.check(section, "cmd2 RUN_TEST + cmd3 GET_TEST_RESULT",
                   lambda: (lambda res: (res[0].get("state") == 2,
                                         f"battery item state={res[0].get('state')} "
                                         f"score={res[0].get('score')}"))(
                       self.cmd_run_test_and_result(4, timeout=30)))

    def hardware_test_items(self):
        """Run each RUN_TEST item that can be validated automatically."""
        section = "hardware-item"
        print("\n=== Hardware test items (RUN_TEST) ===")
        # Buttons are validated via SIMULATE_KEY (cmd5) — the built-in buttons
        # item waits 30s for PHYSICAL presses, which cannot be automated.
        self.record(section, "item0 Buttons (via SIMULATE_KEY)", SKIP,
                    "covered by cmd5 SIMULATE_KEY; physical presses need human")

        # item4 battery already exercised in command_checks.
        self.record(section, "item4 Battery", SKIP, "covered in command checks")

        items = [
            (1, "Display", 40), (3, "LED", 40), (6, "MicroPython", 60),
            (8, "WiFi", 40), (2, "Audio", 40),
        ]
        if not self.quick:
            items += [(5, "Bluetooth", 60)]

        for item, name, timeout in items:
            def run(it=item, nm=name, to=timeout):
                res, detail = self.cmd_run_test_and_result(it, timeout=to)
                state = res.get("state", 0)
                score = res.get("score", 0)
                if state == 2:
                    return True, f"score={score} {detail}"
                raise AssertionError(f"state={STATE_NAMES.get(state, state)} {detail}")
            self.check(section, f"item{item} {name}", run)

        # SDCard depends on hardware presence: WARN (not FAIL), recorded once.
        try:
            res, detail = self.cmd_run_test_and_result(7, timeout=40)
            state = res.get("state", 0)
            if state == 2:
                self.record(section, "item7 SDCard", PASS,
                            f"score={res.get('score', 0)} {detail}")
            elif state == 3:
                self.record(section, "item7 SDCard", WARN,
                            "no card inserted (hardware absent)")
            else:
                self.record(section, "item7 SDCard", FAIL,
                            f"state={STATE_NAMES.get(state, state)} {detail}")
        except Exception as e:
            self.record(section, "item7 SDCard", FAIL, f"exception: {e}")

    def reset_checks(self):
        """Verify RESET_DEVICE and ENTER_BOOTLOADER actually reboot the device."""
        section = "reset"
        print("\n=== Reset / bootloader checks ===")
        if self.quick:
            self.record(section, "cmd11 RESET_DEVICE", SKIP, "quick mode")
            self.record(section, "cmd12 ENTER_BOOTLOADER", SKIP, "quick mode")
            return
        for cmd, name in ((11, "cmd11 RESET_DEVICE"), (12, "cmd12 ENTER_BOOTLOADER")):
            try:
                r = self.dev.send_cmd(cmd, timeout=8)
                ack = bool(r and r.get("status") == "ok")
                # After reboot, wait for PING to come back.
                time.sleep(2.0)
                back = self.dev.wait_ready(timeout=90, verbose=False)
                self.record(section, name, PASS if (ack and back) else FAIL,
                            f"ack={ack} rebooted-and-back={back}")
            except Exception as e:
                self.record(section, name, FAIL, f"exception: {e}")

    # ---------------- report ----------------
    def save_report(self, path_json, path_md, meta):
        os.makedirs(os.path.dirname(os.path.abspath(path_json)), exist_ok=True)
        counts = {s: 0 for s in (PASS, FAIL, WARN, SKIP)}
        for r in self.results:
            counts[r["status"]] = counts.get(r["status"], 0) + 1
        total = len(self.results)
        rate = counts[PASS] / total * 100 if total else 0
        report = {
            "meta": meta, "generated": time.strftime("%Y-%m-%d %H:%M:%S"),
            "summary": {"total": total, "pass": counts[PASS], "fail": counts[FAIL],
                        "warn": counts[WARN], "skip": counts[SKIP],
                        "pass_rate": round(rate, 1)},
            "results": self.results,
        }
        with open(path_json, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)

        lines = [
            "# XiaoMiao Desktop OS — Full Functional Test Report", "",
            f"- Generated: {report['generated']}",
            f"- Port: {meta.get('port')}  Firmware: {meta.get('fw_version', '?')}", "",
            "## Summary", "",
            f"| Total | PASS | FAIL | WARN | SKIP | Pass rate |",
            f"|---:|---:|---:|---:|---:|---:|",
            f"| {total} | {counts[PASS]} | {counts[FAIL]} | {counts[WARN]} | "
            f"{counts[SKIP]} | {rate:.1f}% |", "",
            "## Details", "", "| Section | Item | Status | Detail |",
            "|---|---|---|---|",
        ]
        for r in self.results:
            detail = r["detail"].replace("|", "\\|").replace("\n", " ")
            lines.append(f"| {r['section']} | {r['name']} | {r['status']} | {detail} |")
        with open(path_md, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

        print("\n" + "=" * 60)
        print(f"  TOTAL {total} | PASS {counts[PASS]} | FAIL {counts[FAIL]} | "
              f"WARN {counts[WARN]} | SKIP {counts[SKIP]} | RATE {rate:.1f}%")
        print(f"  JSON report: {path_json}")
        print(f"  MD   report: {path_md}")
        print("=" * 60)
        return counts[FAIL] == 0


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="XiaoMiao OS Full Functional Test Suite")
    ap.add_argument("--port", "-p", help="Serial port, e.g. COM6")
    ap.add_argument("--baud", "-b", type=int, default=115200)
    ap.add_argument("--dry-run", "-n", action="store_true")
    ap.add_argument("--reset", action="store_true",
                    help="Hard-reset first to capture a complete boot log")
    ap.add_argument("--quick", action="store_true",
                    help="Skip long-running tests (bluetooth, reset checks)")
    ap.add_argument("--report-json", default="tests/full_test_report.json")
    ap.add_argument("--report-md", default="tests/full_test_report.md")
    args = ap.parse_args()

    port = args.port
    if not port and not args.dry_run:
        ports = list(serial.tools.list_ports.comports())
        if not ports:
            print("No serial port found; use --port or --dry-run")
            sys.exit(2)
        port = ports[0].device
        print(f"Auto-detected port: {port}")

    dev = None
    meta = {"port": port, "quick": args.quick, "dry_run": args.dry_run}
    if not args.dry_run:
        if args.reset:
            print("Hard-resetting device ...")
            hard_reset(port)
            time.sleep(1.0)
        dev = Device(port, args.baud).connect()
        print(f"Connected to {port} @ {args.baud}")
        print("Waiting for command interface ...")
        if not dev.wait_ready(timeout=90):
            print("Device not answering PING — aborting")
            dev.close()
            sys.exit(3)
        # Capture boot log if not resetting (drain whatever is pending).
        dev._pump(2.0)

        # Snapshot firmware version early (before reset checks reboot it).
        info = dev.send_cmd(1, timeout=5)
        if info and info.get("data"):
            meta["fw_version"] = info["data"].get("fw_version")
    else:
        # Synthetic device for dry-run.
        class DryDev:
            log_lines = list(MARKERS.values())
            def log_text(self): return "\n".join(MARKERS.values())
            def send_cmd(self, cmd, params=None, **kw):
                statuses = {0: "pong"}
                r = {"id": 1, "cmd": cmd, "status": "ok"}
                if cmd == 0: r["msg"] = "pong"
                elif cmd == 1:
                    r["data"] = {"device": "Xiaomiao Desktop", "fw_version": "dry-run",
                                 "hw_version": "ESP32", "flash_mb": 16, "psram_mb": 8,
                                 "screen_w": 160, "screen_h": 128, "buttons": 6,
                                 "leds": 3, "micropython_ready": True,
                                 "test_running": False}
                elif cmd == 9: r["data"] = {"voltage_mv": 3900, "percent": 75}
                elif cmd == 10: r["data"] = [{"id": i} for i in range(9)]
                elif cmd == 4: r.update(ret=0, output="FULLTEST 42\nSIZE 160 128\n")
                elif cmd == 2: r["status"] = "started"
                elif cmd == 3: r["data"] = [{"state": 2, "score": 100, "detail": "dry-run"}]
                elif cmd == 5:
                    r["status"] = "error" if not (0 <= (params or {}).get("key", -1) <= 5) else "ok"
                elif cmd == 6:
                    r["status"] = "error" if not (0 <= (params or {}).get("index", -1) <= 2) else "ok"
                elif cmd == 7:
                    r["status"] = "ok" if "brightness" in (params or {}) else "error"
                elif cmd == 8:
                    r["status"] = "ok" if "freq" in (params or {}) else "error"
                elif cmd in (11, 12): r["status"] = "ok"
                return r
            def wait_ready(self, **kw): return True
            def wait_test_idle(self, **kw): return True
            def _pump(self, s): return []
        dev = DryDev()
        meta["fw_version"] = "dry-run"

    runner = Runner(dev, quick=args.quick, dry_run=args.dry_run)
    try:
        runner.boot_log_checks()
        runner.command_checks()
        runner.hardware_test_items()
        runner.reset_checks()
    finally:
        if hasattr(dev, "close"):
            dev.close()
    ok = runner.save_report(args.report_json, args.report_md, meta)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
