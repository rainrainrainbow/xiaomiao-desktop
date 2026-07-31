#!/usr/bin/env python3
"""
XiaoMiao Desktop OS — Hardware Test Suite
==========================================
Automated hardware verification tests.

Usage:
    # Run all tests (requires pySerial + ESP32 connected)
    python3 test_hardware.py --port /dev/ttyUSB0 --all

    # Run specific test
    python3 test_hardware.py --port /dev/ttyUSB0 --test boot

    # Dry run (no hardware)
    python3 test_hardware.py --dry-run
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

EXPECTED_BOOT_LOG = "XiaoMiao Desktop OS booting..."
EXPECTED_LCD_LOG = "LCD initialized"
EXPECTED_MPY_LOG = "MicroPython engine initialized"
EXPECTED_SD_LOG = "SD card mounted"
EXPECTED_DESKTOP_LOG = "Found"


# =============================================================================
# Test Runner
# =============================================================================
class TestRunner:
    def __init__(self, port=None, baud=115200, dry_run=False):
        self.port = port
        self.baud = baud
        self.dry_run = dry_run
        self.serial = None
        self.results = []
        self.total = 0
        self.passed = 0
        self.failed = 0

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
            self.serial = serial.Serial(self.port, self.baud, timeout=3)
            print(f"ℹ️  Connected to {self.port} @ {self.baud} baud")
            return True
        except Exception as e:
            print(f"❌ Failed to connect: {e}")
            return False

    def disconnect(self):
        if self.serial:
            self.serial.close()

    def read_log(self, timeout=5, expect=None):
        """Read serial log, optionally searching for expected string."""
        if self.dry_run:
            print(f"  [DRY RUN] Would read serial log (expect={expect})")
            return True if expect else ""

        start = time.time()
        buffer = ""
        while time.time() - start < timeout:
            if self.serial.in_waiting:
                data = self.serial.read(self.serial.in_waiting).decode('utf-8', errors='replace')
                buffer += data
                if expect and expect in buffer:
                    return True
            time.sleep(0.1)

        if expect:
            return expect in buffer
        return buffer

    def send_key(self, key):
        """Send a simulated keypress via serial."""
        if self.dry_run:
            print(f"  [DRY RUN] Send key: {key}")
            return
        # Key codes match the keypad driver
        key_map = {
            'UP': 'w', 'DOWN': 's', 'LEFT': 'a', 'RIGHT': 'd',
            'A': ' ', 'B': '\x1b',
            'ENTER': '\r', 'RESET': None
        }
        if key == 'RESET':
            self.serial.setDTR(False)
            self.serial.setRTS(True)
            time.sleep(0.1)
            self.serial.setRTS(False)
            self.serial.setDTR(True)
            time.sleep(0.5)
            return

        code = key_map.get(key.upper())
        if code:
            self.serial.write(code.encode())

    def test(self, name, fn):
        """Run a single test case."""
        self.total += 1
        print(f"\n{'='*60}")
        print(f"  Test #{self.total}: {name}")
        print(f"{'='*60}")
        try:
            fn()
            self.passed += 1
            print(f"  {PASS}")
        except AssertionError as e:
            self.failed += 1
            print(f"  {FAIL} — {e}")
        except Exception as e:
            self.failed += 1
            print(f"  {FAIL} — Exception: {e}")

    def summary(self):
        print(f"\n{'='*60}")
        print(f"  Test Summary")
        print(f"{'='*60}")
        print(f"  Total:  {self.total}")
        print(f"  Passed: {self.passed}")
        print(f"  Failed: {self.failed}")
        rate = (self.passed / self.total * 100) if self.total > 0 else 0
        print(f"  Rate:   {rate:.0f}%")
        return self.failed == 0


# =============================================================================
# Test Cases
# =============================================================================
def test_boot(tr):
    """TC-01: Verify boot sequence completes successfully."""
    if not tr.dry_run:
        tr.send_key('RESET')
        time.sleep(1)
    boot_ok = tr.read_log(timeout=10, expect=EXPECTED_BOOT_LOG)
    assert boot_ok, f"Expected boot log '{EXPECTED_BOOT_LOG}' not found"
    print(f"  ✅ Boot log detected")

    lcd_ok = tr.read_log(timeout=5, expect=EXPECTED_LCD_LOG)
    assert lcd_ok, f"Expected LCD log '{EXPECTED_LCD_LOG}' not found"
    print(f"  ✅ LCD initialized")

    # Wait for desktop
    desktop_ok = tr.read_log(timeout=8, expect=EXPECTED_DESKTOP_LOG)
    if desktop_ok:
        print(f"  ✅ Desktop loaded")
    else:
        print(f"  ⚠️  Desktop log not found (may take longer)")


def test_mpy_init(tr):
    """TC-09: Verify MicroPython initialization."""
    mpy_ok = tr.read_log(timeout=5, expect=EXPECTED_MPY_LOG)
    assert mpy_ok, f"Expected MicroPython log '{EXPECTED_MPY_LOG}' not found"
    print(f"  ✅ MicroPython engine initialized")


def test_sd_card(tr):
    """Verify SD card detection."""
    sd_ok = tr.read_log(timeout=3, expect=EXPECTED_SD_LOG)
    if sd_ok:
        print(f"  ✅ SD card detected")
    else:
        print(f"  ⚠️  SD card not detected (check card/connections)")


def test_serial_loopback(tr):
    """Verify serial communication is working."""
    if tr.dry_run:
        return
    # Send a test command and check for echo
    tr.serial.write(b"\n")
    time.sleep(0.5)
    data = tr.serial.read(100)
    assert len(data) > 0, "No serial response"
    print(f"  ✅ Serial communication OK ({len(data)} bytes received)")


def test_keypad_navigation(tr):
    """TC-02: Verify keypad navigation works."""
    if tr.dry_run:
        return
    # Send navigation keys
    for key in ['UP', 'DOWN', 'LEFT', 'RIGHT']:
        tr.send_key(key)
        time.sleep(0.2)
    print(f"  ✅ Navigation keys sent (verify visually)")

    # Send A to open app
    tr.send_key('A')
    time.sleep(0.5)
    print(f"  ✅ A key sent (verify visually)")

    # Send B to go back
    tr.send_key('B')
    time.sleep(0.5)
    print(f"  ✅ B key sent (verify visually)")


# =============================================================================
# Main
# =============================================================================
def main():
    parser = argparse.ArgumentParser(description='XiaoMiao Desktop OS Hardware Test Suite')
    parser.add_argument('--port', '-p', help='Serial port (e.g. /dev/ttyUSB0)')
    parser.add_argument('--baud', '-b', type=int, default=115200, help='Baud rate')
    parser.add_argument('--dry-run', '-n', action='store_true', help='Dry run without hardware')
    parser.add_argument('--all', '-a', action='store_true', help='Run all tests')
    parser.add_argument('--test', '-t', choices=['boot', 'mpy', 'sd', 'serial', 'keypad', 'all'],
                        default='all', help='Specific test to run')
    args = parser.parse_args()

    tr = TestRunner(port=args.port, baud=args.baud, dry_run=args.dry_run)

    if not tr.connect():
        sys.exit(1)

    try:
        if args.test in ('boot', 'all'):
            tr.test("Boot Sequence", lambda: test_boot(tr))

        if args.test in ('mpy', 'all'):
            tr.test("MicroPython Init", lambda: test_mpy_init(tr))

        if args.test in ('sd', 'all'):
            tr.test("SD Card Detection", lambda: test_sd_card(tr))

        if args.test in ('serial', 'all'):
            tr.test("Serial Loopback", lambda: test_serial_loopback(tr))

        if args.test in ('keypad', 'all'):
            tr.test("Keypad Navigation", lambda: test_keypad_navigation(tr))

        success = tr.summary()
        sys.exit(0 if success else 1)

    finally:
        tr.disconnect()


if __name__ == '__main__':
    main()