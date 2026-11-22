# XiaoMiao Desktop OS — Full Functional Test Report

- Generated: 2026-11-22 11:20:11
- Port: COM6  Firmware: v71.1

## Summary

| Total | PASS | FAIL | WARN | SKIP | Pass rate |
|---:|---:|---:|---:|---:|---:|
| 38 | 18 | 0 | 1 | 19 | 47.4% |

## Details

| Section | Item | Status | Detail |
|---|---|---|---|
| boot-log | Boot header / firmware version | SKIP | no boot log captured; use --reset to collect |
| boot-log | Main loop running | SKIP | no boot log captured; use --reset to collect |
| boot-log | Display ON | SKIP | no boot log captured; use --reset to collect |
| boot-log | LVGL initialized | SKIP | no boot log captured; use --reset to collect |
| boot-log | Framebuffer ready | SKIP | no boot log captured; use --reset to collect |
| boot-log | Button task created | SKIP | no boot log captured; use --reset to collect |
| boot-log | Battery ADC initialized | SKIP | no boot log captured; use --reset to collect |
| boot-log | RGB LED strip initialized | SKIP | no boot log captured; use --reset to collect |
| boot-log | LMD2718 digital mic initialized | SKIP | no boot log captured; use --reset to collect |
| boot-log | I2S DAC backend ready | SKIP | no boot log captured; use --reset to collect |
| boot-log | Bluetooth A2DP Sink initialized | SKIP | no boot log captured; use --reset to collect |
| boot-log | MicroPython runtime initialized | SKIP | no boot log captured; use --reset to collect |
| boot-log | Desktop page loaded | SKIP | no boot log captured; use --reset to collect |
| boot-log | 6 builtin apps registered | SKIP | no boot log captured; use --reset to collect |
| boot-log | Test interface ready | SKIP | no boot log captured; use --reset to collect |
| command | cmd0 PING | PASS | id=1010 rtt-ok |
| command | cmd1 GET_INFO | PASS | Xiaomiao Desktop v71.1 screen=160x128 btns=6 leds=3 mpy=True |
| command | cmd10 LIST_TESTS | PASS | 9 test items listed |
| command | cmd9 GET_BATTERY | PASS | 4113 mV, 100% |
| command | cmd5 SIMULATE_KEY | PASS | 6 keys + long-press injected; invalid key rejected |
| command | cmd6 SET_LED | PASS | 3 LEDs set R/G/B; invalid index rejected; LEDs off |
| command | cmd7 SET_BACKLIGHT | PASS | 100/50/10/100% applied; missing param rejected |
| command | cmd8 PLAY_TONE | PASS | 440Hz + 880Hz tones played; missing param rejected |
| command | cmd4 EXEC_PYTHON | PASS | ret=0 script executed OK; print_echo=n/a (stdout not routed to console by design) |
| command | cmd13 DIAG_WIFI_SCAN (crash guard) | PASS | scan OK, device survived; WiFi scan complete: 1 networks found[0m |
| command | cmd14 FILE_IO stat (internal FAT music) | PASS | /flash/music/renshijian.mp3 size=480487 |
| command | cmd15/16 MP3 decode + playback | PASS | MP3 decoded: src=2 44100Hz 1ch bytes=56320 file=renshijian.mp3 |
| command | cmd2 RUN_TEST + cmd3 GET_TEST_RESULT | PASS | battery item state=2 score=100 |
| hardware-item | item0 Buttons (via SIMULATE_KEY) | SKIP | covered by cmd5 SIMULATE_KEY; physical presses need human |
| hardware-item | item4 Battery | SKIP | covered in command checks |
| hardware-item | item1 Display | PASS | score=100 Display patterns shown (160x128 RGB565) |
| hardware-item | item3 LED | PASS | score=100 RGB LED strip tested (3 LEDs) |
| hardware-item | item6 MicroPython | PASS | score=100 MicroPython runtime ready; xiaomiao framebuffer available |
| hardware-item | item8 WiFi | PASS | score=90 WiFi stack initialized, debug server running |
| hardware-item | item2 Audio | PASS | score=100 Test tones played via I2S DAC |
| hardware-item | item7 SDCard | WARN | no card inserted (hardware absent) |
| reset | cmd11 RESET_DEVICE | SKIP | quick mode |
| reset | cmd12 ENTER_BOOTLOADER | SKIP | quick mode |
