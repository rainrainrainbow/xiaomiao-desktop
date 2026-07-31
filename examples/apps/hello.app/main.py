"""
XiaoMiao Desktop — Hello World Example App

Demonstrates the xiaomiao hardware module API:
  - xiaomiao.lvgl: draw text and rectangles
  - xiaomiao.keypad: read button state
  - xiaomiao.battery: read battery level
  - xiaomiao.xm_time: sleep/delay
"""

import xiaomiao
import xiaomiao_lvgl as lvgl
import xiaomiao_xm_time as time

# Clear the screen
lvgl.clear()

# Draw a title
lvgl.label("Hello XiaoMiao!", 10, 10)

# Draw a decorative rectangle
lvgl.rect(10, 30, 140, 2, 0xF6D34A)

# Show battery level
battery = xiaomiao.battery.read()
lvgl.label("Battery: {}%".format(battery), 10, 40)

# Draw a "Press A" prompt
lvgl.label("Press A to continue...", 10, 70)

# Main loop
pressed = False
while True:
    key = xiaomiao.keypad.get(100)
    if key == xiaomiao.keypad.A:
        if not pressed:
            lvgl.rect(10, 90, 140, 20, 0x2DD466)
            lvgl.label("Button A pressed! 🎉", 20, 93)
            pressed = True
    elif key == xiaomiao.keypad.B:
        # Exit the app
        lvgl.clear()
        lvgl.label("Goodbye! 👋", 40, 50)
        time.msleep(1000)
        break
    time.msleep(50)