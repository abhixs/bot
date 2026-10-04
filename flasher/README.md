# RoboThings S3 Mini firmware

Xiaozhi AI voice assistant for the ESP32-S3 Super Mini Zero (4MB flash, 2MB PSRAM),
built from the open-source [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) project
with a new board `robothings-s3-mini`.

## What is new

- **Animated robot eyes** on the 128x64 OLED: blink, look around when idle, grow
  attentive while listening, bounce while speaking, scan while connecting, fall asleep
  (Zzz) after 2 minutes idle. Each AI emotion has its own face: happy, laughing, sad,
  crying, angry, surprised, love (heart eyes), thinking, confused, wink, cool.
- **Voice alarms and timers** (device-side MCP tools `self.alarm.*`):
  set once / daily / weekdays / weekends / chosen days, list, cancel, snooze, optional
  lamp-on with the alarm. Saved in flash, ring without internet, stop with any button,
  auto-stop after 60 s.
- **Lamp relay** voice control (GPIO 12), as before.

## Wiring (unchanged from the original firmware)

| Part | Pin | ESP32-S3 |
|---|---|---|
| INMP441 mic | WS / SCK / SD | GPIO 4 / 5 / 6 (L/R to GND) |
| MAX98357A amp | DIN / BCLK / LRC | GPIO 1 / 2 / 3 (VIN 5V, SD to 3.3V) |
| SSD1306 OLED | SDA / SCL | GPIO 7 / 8 |
| Boot / talk button | | GPIO 0 |
| Hold-to-talk button | | GPIO 11 |
| Volume up / down | | GPIO 9 / 10 |
| Lamp relay IN | | GPIO 12 |
| Status LED | | GPIO 21 |

## Flashing

Open `index.html` from GitHub Pages (or any https host) in Chrome / Edge, or flash
manually with ESP Flash Download Tool (ESP32-S3, 4MB, DIO, 80MHz):

| File | Address |
|---|---|
| bootloader.bin | 0x0 |
| partition-table.bin | 0x8000 |
| ota_data_initial.bin | 0xd000 |
| xiaozhi.bin | 0x10000 |
| generated_assets.bin | 0x300000 |

`merged-binary.bin` contains all of them at 0x0.

Two builds are published: `firmware/` for 0.96" SSD1306 OLEDs and
`firmware-sh1106/` for 1.3" SH1106 OLEDs (most 1.3" I2C modules). The wrong one
shows a screen full of random dots.

The partition layout is the same as the original firmware, so the existing
GitHub Pages flasher works by replacing the five `.bin` files.

## First setup

1. Power on: the device opens a Wi-Fi hotspot `Xiaozhi-XXXX`.
2. Join it, open `http://192.168.4.1`, choose your Wi-Fi.
3. The OLED status line shows an activation code. Add the device on
   https://xiaozhi.me with that code. Set language, voice and personality there.
