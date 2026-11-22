# XiaoMiao Desktop OS — Full Functional Test Report

- Generated: 2026-11-22 09:08:46
- Port: COM6  Firmware: v71.1

## Summary

| Total | PASS | FAIL | WARN | SKIP | Pass rate |
|---:|---:|---:|---:|---:|---:|
| 43 | 39 | 0 | 2 | 2 | 90.7% |

## Details

| Section | Item | Status | Detail |
|---|---|---|---|
| boot-log | Boot header / firmware version | PASS |  |
| boot-log | Main loop running | PASS |  |
| boot-log | Display ON | PASS |  |
| boot-log | LVGL initialized | PASS |  |
| boot-log | Framebuffer ready | PASS |  |
| boot-log | Button task created | PASS |  |
| boot-log | Battery ADC initialized | PASS |  |
| boot-log | RGB LED strip initialized | PASS |  |
| boot-log | LMD2718 digital mic initialized | PASS |  |
| boot-log | I2S DAC backend ready | PASS |  |
| boot-log | Bluetooth A2DP Sink initialized | PASS |  |
| boot-log | MicroPython runtime initialized | PASS |  |
| boot-log | Desktop page loaded | PASS |  |
| boot-log | 6 builtin apps registered | PASS |  |
| boot-log | Test interface ready | PASS |  |
| boot-log | SD card not inserted | WARN | hardware absent; Chinese font falls back to English |
| boot-log | No reboot-loop | PASS | rst: count=0 (1 expected from --reset) |
| boot-log | No stack overflow | PASS | overflow count=0 |
| boot-log | No UART driver error | PASS | error count=0 |
| command | cmd0 PING | PASS | id=1017 rtt-ok |
| command | cmd1 GET_INFO | PASS | Xiaomiao Desktop v71.1 screen=160x128 btns=6 leds=3 mpy=True |
| command | cmd10 LIST_TESTS | PASS | 9 test items listed |
| command | cmd9 GET_BATTERY | PASS | 4078 mV, 97% |
| command | cmd5 SIMULATE_KEY | PASS | 6 keys + long-press injected; invalid key rejected |
| command | cmd6 SET_LED | PASS | 3 LEDs set R/G/B; invalid index rejected; LEDs off |
| command | cmd7 SET_BACKLIGHT | PASS | 100/50/10/100% applied; missing param rejected |
| command | cmd8 PLAY_TONE | PASS | 440Hz + 880Hz tones played; missing param rejected |
| command | cmd4 EXEC_PYTHON | PASS | ret=0 script executed OK; print_echo=n/a (stdout not routed to console by design) |
| command | cmd13 DIAG_WIFI_SCAN (crash guard) | PASS | scan OK, device survived; WiFi scan complete: 1 networks found[0m |
| command | cmd14 FILE_IO stat (internal FAT music) | PASS | /flash/music/renshijian.mp3 size=480487 |
| command | cmd15/16 MP3 decode + playback | PASS | MP3 decoded: src=2 44100Hz 1ch bytes=57344 file=renshijian.mp3 |
| command | cmd2 RUN_TEST + cmd3 GET_TEST_RESULT | PASS | battery item state=2 score=100 |
| hardware-item | item0 Buttons (via SIMULATE_KEY) | SKIP | covered by cmd5 SIMULATE_KEY; physical presses need human |
| hardware-item | item4 Battery | SKIP | covered in command checks |
| hardware-item | item1 Display | PASS | score=100 Display patterns shown (160x128 RGB565) |
| hardware-item | item3 LED | PASS | score=100 RGB LED strip tested (3 LEDs) |
| hardware-item | item6 MicroPython | PASS | score=100 MicroPython runtime ready; xiaomiao framebuffer available |
| hardware-item | item8 WiFi | PASS | score=90 WiFi stack initialized, debug server running |
| hardware-item | item2 Audio | PASS | score=100 Test tones played via I2S DAC |
| hardware-item | item5 Bluetooth | PASS | score=80 Bluetooth A2DP initialized, manual pairing test needed |
| hardware-item | item7 SDCard | WARN | no card inserted (hardware absent) |
| reset | cmd11 RESET_DEVICE | PASS | ack=True rebooted-and-back=True |
| reset | cmd12 ENTER_BOOTLOADER | PASS | ack=True rebooted-and-back=True |
