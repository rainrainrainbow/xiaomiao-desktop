#!/usr/bin/env python3
"""
XiaoMiao Desktop OS — Hardware Test Suite (v2)
==============================================
Automated hardware verification tests driven by the device boot log
plus serial command probes.

Usage:
    # Run all tests (requires pySerial + ESP32 connected)
    python3 test_hardware.py --port COM6 --all

    # Run specific test
    python3 test_hardware.py --port COM6 --test boot

    # Dry run (no hardware)
    python3 test_hardware.py --dry-run

Notes:
    - Log markers are aligned with the current firmware build
      (v71.1 / "Xiaomiao Desktop v20 (Backlight Fix)").
    - The boot log is captured once into a cumulative buffer and every
      test searches that buffer, so fast/emitted-once messages are not lost.
"""

import sys
import time
import argparse
import serial
import serial.tools.list_ports


# =============================================================================
# Test Configuration
# =============================================================================
PASS = "✅ PASS"
FAIL = "❌ FAIL"
SKIP = "⏭️ SKIP"
WARN = "⚠️ WARN"

# Markers taken from real firmware output (see docs/HARDWARE.md).
M = {
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
}


# =============================================================================
# Test Runner
# =============================================================================
class TestRunner:
    def __init__(self, port=None, baud=115200, dry_run=False, capture_s=5, force_reset=True):
        self.port = port
        self.baud = baud
        self.dry_run = dry_run
        self.capture_s = capture_s
        self.force_reset = force_reset
        self.serial = None
        self.log = ""            # cumulative boot log buffer
        self.results = []
        self.total = 0
        self.passed = 0
        self.failed = 0
        self.warned = 0

    def connect(self):
        if self.dry_run:
            print("ℹ️  Dry run mode — no hardware connection")
            return True
        if not self.port:
            ports = list(serial.tools.list_ports.comports())
            if not ports:
                print("❌ No serial ports found. Use --port or --dry-run")
                return False
            self.port = ports[0].device
            print(f"ℹ️  Auto-detected port: {self.port}")
        try:
            self.serial = serial.Serial(self.port, self.baud, timeout=1)
            print(f"ℹ️  Connected to {self.port} @ {self.baud} baud")
            return True
        except Exception as e:
            print(f"❌ Failed to connect: {e}")
            return False

    def disconnect(self):
        if self.serial:
            self.serial.close()

    def reset_device(self):
        """Optional hard-reset (disabled by default; use --reset)."""
        if self.dry_run or not self.serial:
            return
        self.serial.setDTR(False)
        self.serial.setRTS(True)
        time.sleep(0.15)
        self.serial.setRTS(False)
        self.serial.setDTR(True)
        print("  ↻  Device reset")

    def wait_ready(self, timeout=25):
        """Wait until the device command interface answers PING."""
        if self.dry_run or not self.serial:
            return True
        deadline = time.time() + timeout
        attempt = 0
        while time.time() < deadline:
            self.serial.reset_input_buffer()
            self.serial.write(b'XTC:{"cmd":0,"id":%d}\n' % (900 + attempt))
            self.serial.flush()
            attempt += 1
            time.sleep(0.15)
            buf = b""
            end = time.time() + 0.8
            while time.time() < end:
                if self.serial.in_waiting:
                    buf += self.serial.read(self.serial.in_waiting)
                if b"XTR" in buf or b"pong" in buf:
                    print(f"  ✓ Device ready (after {attempt} probe(s))")
                    return True
                time.sleep(0.03)
        print("  ✗ Device not answering PING after wait")
        return False

    def collect_log(self):
        """Passively capture whatever the device emits (no reset by default).

        The firmware takes ~14s to boot. Unless --reset is given we do NOT
        reset it here: repeated DTR/RTS resets can leave the board half-booted
        and make the UART command task appear dead. We instead drain the log
        stream and, if needed, probe PING until the device is ready.
        """
        if self.dry_run:
            self.log = "\n".join(M.values())  # every marker "found"
            return
        # Reset may occasionally fail to boot (DTR/RTS timing). Retry until we
        # see the main-loop marker, up to 3 attempts.
        for attempt in range(3):
            if attempt > 0:
                # Let the board fully settle between resets.
                time.sleep(3)
            if self.force_reset:
                self.reset_device()
            self.serial.reset_input_buffer()
            self.log = ""
            start = time.time()
            while time.time() - start < 24:
                if self.serial.in_waiting:
                    self.log += self.serial.read(self.serial.in_waiting).decode(
                        "utf-8", errors="replace"
                    )
                time.sleep(0.05)
            if M["mainloop"] in self.log:
                break
            print(f"  ↻  Boot not detected (attempt {attempt + 1}), retrying reset ...")
        time.sleep(1.0)
        self.uart_err_count = self.log.count(M["uart_err"])
        print(f"  ℹ️  Captured {len(self.log)} bytes, "
              f"uart driver errors: {self.uart_err_count}")

    def has(self, marker):
        return marker in self.log

    def send_key(self, key):
        """Send a simulated keypress via raw serial byte (best effort)."""
        if self.dry_run or not self.serial:
            return
        key_map = {
            "UP": "w", "DOWN": "s", "LEFT": "a", "RIGHT": "d",
            "A": " ", "B": "\x1b", "ENTER": "\r",
        }
        code = key_map.get(str(key).upper())
        if code:
            self.serial.write(code.encode())
            time.sleep(0.15)

    def test(self, name, fn, soft=False):
        """Run a single test case. soft=True -> WARN instead of FAIL."""
        self.total += 1
        print(f"\n{'='*60}\n  Test #{self.total}: {name}\n{'='*60}")
        try:
            fn()
            self.passed += 1
            print(f"  {PASS}")
        except AssertionError as e:
            if soft:
                self.warned += 1
                print(f"  {WARN} — {e}")
            else:
                self.failed += 1
                print(f"  {FAIL} — {e}")
        except Exception as e:
            self.failed += 1
            print(f"  {FAIL} — Exception: {e}")

    def summary(self):
        print(f"\n{'='*60}\n  Test Summary\n{'='*60}")
        print(f"  Total:  {self.total}")
        print(f"  Passed: {self.passed}")
        print(f"  Failed: {self.failed}")
        print(f"  Warned: {self.warned}")
        rate = (self.passed / self.total * 100) if self.total else 0
        print(f"  Rate:   {rate:.0f}%")
        return self.failed == 0


# =============================================================================
# Test Cases  (all driven by the captured boot log)
# =============================================================================
def _require(tr, marker, label):
    assert tr.has(marker), f"'{label}' not found in boot log (expected '{marker}')"
    print(f"  ✓ {label}")


def test_boot(tr):
    _require(tr, M["boot_header"], "Boot header")
    _require(tr, M["mainloop"], "Main loop running")


def test_display(tr):
    # Splash 是启动早期一次性日志，可能被 UART error 刷屏淹没，仅作信息项
    if tr.has(M["splash"]):
        print("  ✓ Splash screen")
    _require(tr, M["display_on"], "Display ON")
    _require(tr, M["lvgl"], "LVGL initialized")
    _require(tr, M["framebuffer"], "Framebuffer ready (160x128 RGB565)")


def test_buttons(tr):
    _require(tr, M["button_task"], "Button task created")


def test_battery(tr):
    _require(tr, M["battery"], "Battery ADC initialized")


def test_led(tr):
    _require(tr, M["led"], "RGB LED strip initialized (3 LEDs)")


def test_mic(tr):
    _require(tr, M["mic"], "LMD2718 digital mic initialized")


def test_audio(tr):
    _require(tr, M["audio_i2s"], "I2S DAC backend ready")
    _require(tr, M["audio_bt"], "Bluetooth A2DP Sink ready")


def test_mpy(tr):
    _require(tr, M["mpy"], "MicroPython runtime initialized")


def test_apps(tr):
    _require(tr, M["desktop"], "Desktop page loaded")
    _require(tr, M["apps"], "6 builtin apps registered")


def test_sd_card(tr):
    if tr.has(M["sd_ok"]):
        print("  ✓ SD card mounted")
    elif tr.has(M["sd_fail"]):
        assert False, "SD card not detected (no card inserted / wiring issue)"
    else:
        assert False, "SD card status not found in log"


def test_uart_error(tr):
    """The test command interface must not spam uart driver errors."""
    cnt = getattr(tr, "uart_err_count", 0)
    assert cnt == 0, (f"{cnt} 'uart driver error' events — "
                      "UART0 driver not installed (uart_driver_install missing)")


def test_uart_command(tr):
    """Probe the JSON command interface (PING).

    The device takes ~14s to finish booting (WiFi retries + BT A2DP), and the
    UART command task only becomes ready after the main loop starts. So keep
    probing until ready instead of assuming boot finished after log capture.
    """
    if tr.dry_run or not tr.serial:
        return
    sent = False
    # Send a few PINGs, then drain cumulatively for up to 20s. Do NOT clear the
    # input buffer between probes: the low-priority UART task can delay a
    # response by a second or more, and clearing would discard it.
    tr.serial.reset_input_buffer()
    for i in range(5):
        tr.serial.write(b'XTC:{"cmd":0,"id":%d}\n' % (400 + i))
        tr.serial.flush()
        time.sleep(0.15)
    buf = b""
    deadline = time.time() + 20.0
    while time.time() < deadline:
        if tr.serial.in_waiting:
            buf += tr.serial.read(tr.serial.in_waiting)
        chunk = buf.decode("utf-8", errors="replace")
        if "pong" in chunk.lower() or "XTR" in chunk:
            sent = True
            resp = [l for l in chunk.splitlines()
                    if "XTR" in l or "pong" in l.lower()][:1]
            print(f"  ✓ Command interface responded: {resp}")
            break
        time.sleep(0.1)
    assert sent, "No response to PING after 20s — UART command interface not receiving"


def test_serial_loopback(tr):
    if tr.dry_run:
        return
    # A PING command is a reliable round-trip probe: write then expect bytes.
    tr.serial.reset_input_buffer()
    tr.serial.write(b'XTC:{"cmd":0,"id":7}\n')
    tr.serial.flush()
    data = b""
    deadline = time.time() + 20.0
    while time.time() < deadline:
        if tr.serial.in_waiting:
            data += tr.serial.read(tr.serial.in_waiting)
        if data:
            break
        time.sleep(0.1)
    assert len(data) > 0, "No serial bytes received after write"
    print(f"  ✓ Serial TX/RX active ({len(data)} bytes received)")


def test_keypad(tr):
    if tr.dry_run:
        return
    for key in ("UP", "DOWN", "LEFT", "RIGHT", "A", "B"):
        tr.send_key(key)
    print("  ✓ Navigation + A/B keys sent (visual confirmation required)")


# =============================================================================
# Main
# =============================================================================
def main():
    parser = argparse.ArgumentParser(description="XiaoMiao Desktop OS Hardware Test Suite")
    parser.add_argument("--port", "-p", help="Serial port (e.g. COM6)")
    parser.add_argument("--baud", "-b", type=int, default=115200, help="Baud rate")
    parser.add_argument("--dry-run", "-n", action="store_true", help="Dry run without hardware")
    parser.add_argument("--all", "-a", action="store_true", help="Run all tests")
    parser.add_argument("--capture", type=int, default=5, help="Log drain seconds (default 5)")
    parser.add_argument("--reset", action="store_true",
                        help="Hard-reset device first to capture full boot log")
    parser.add_argument(
        "--test", "-t",
        choices=["boot", "display", "buttons", "battery", "led", "mic", "audio",
                 "mpy", "apps", "sd", "uart", "cmd", "serial", "keypad", "all"],
        default="all", help="Specific test to run",
    )
    args = parser.parse_args()

    tr = TestRunner(port=args.port, baud=args.baud,
                    dry_run=args.dry_run, capture_s=args.capture,
                    force_reset=args.reset)

    if not tr.connect():
        sys.exit(1)

    try:
        # Capture the boot log once; all log-driven tests share it.
        print("\n>>> Capturing boot log ...")
        tr.collect_log()

        all_tests = args.test == "all" or args.all
        # Run live serial probes first, right after boot (device is freshest).
        if all_tests or args.test == "cmd":
            tr.test("JSON Command Interface", lambda: test_uart_command(tr))
        if all_tests or args.test == "serial":
            tr.test("Serial Loopback", lambda: test_serial_loopback(tr))
        if all_tests or args.test == "boot":
            tr.test("Boot Sequence", lambda: test_boot(tr))
        if all_tests or args.test == "display":
            tr.test("Display / LVGL", lambda: test_display(tr))
        if all_tests or args.test == "buttons":
            tr.test("Buttons", lambda: test_buttons(tr))
        if all_tests or args.test == "battery":
            tr.test("Battery ADC", lambda: test_battery(tr))
        if all_tests or args.test == "led":
            tr.test("RGB LED Strip", lambda: test_led(tr))
        if all_tests or args.test == "mic":
            tr.test("Microphone", lambda: test_mic(tr))
        if all_tests or args.test == "audio":
            tr.test("Audio (I2S DAC + BT A2DP)", lambda: test_audio(tr))
        if all_tests or args.test == "mpy":
            tr.test("MicroPython Runtime", lambda: test_mpy(tr))
        if all_tests or args.test == "apps":
            tr.test("Desktop / Builtin Apps", lambda: test_apps(tr))
        if all_tests or args.test == "sd":
            tr.test("SD Card", lambda: test_sd_card(tr), soft=True)
        if all_tests or args.test == "uart":
            tr.test("UART Driver Health", lambda: test_uart_error(tr))
        if all_tests or args.test == "keypad":
            tr.test("Keypad Navigation", lambda: test_keypad(tr))

        success = tr.summary()
        sys.exit(0 if success else 1)
    finally:
        tr.disconnect()


if __name__ == "__main__":
    main()
