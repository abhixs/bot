# RoboThings S3 Mini firmware

Xiaozhi AI voice assistant for the ESP32-S3 Super Mini Zero (4MB flash, 2MB PSRAM),
built from the open-source [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) project
with a new board `robothings-s3-mini`.

## What is new

- **Animated robot eyes** on the 128x64 OLED: blink, look around when idle, grow
  attentive while listening, pulse while speaking, scan while connecting, and drift
  calmly in standby. Each AI emotion has its own face: happy, laughing, sad,
  crying, angry, surprised, love (heart eyes), thinking, confused, wink, cool.
- **Voice alarms and timers** (device-side MCP tools `self.alarm.*`):
  set once / daily / weekdays / weekends / chosen days, list, cancel, snooze, optional
  lamp-on with the alarm. Saved in flash, ring without internet. Stop it by saying the
  wake word ("Alexa") or pressing any button; it auto-stops after 60 s.
- **Lamp relay** voice control (GPIO 12), as before.
- **Music from your own library**: run `tools/music-server/robothings_music_server.py`
  on a PC with your songs, then say "Alexa, Tum Hi Ho chalao". Saying "Alexa" pauses,
  the song resumes after the chat. Tools `self.music.*`.
- **Free online music (Jamendo)**: legal music by independent artists in every genre,
  streamed and decoded (MP3) on the device, no PC needed. Say "Alexa, lofi music chalao"
  or "Alexa, play some relaxing piano". Jamendo has no Bollywood songs; those come from
  your own library. Needs a free Jamendo client ID as the `JAMENDO_CLIENT_ID` repository
  secret (see below).
- **India time (IST)** by default: the clock is synced from internet time (SNTP) and
  kept at UTC+05:30. Another zone can be set by voice (tool `self.clock.set_timezone`).

## Wake word

"Alexa" (ESP-SR model `wn9_alexa`). Another one can be chosen when running the
workflow manually (Actions -> Build RoboThings firmware -> Run workflow).

## Voice shortcuts and screen

- **"Bye", "Bye bye", "Chup raho", "Chup raho ab"**: the device stops at once and goes
  to standby without a word: the speaker is muted the moment the words are recognised.
- **"So jao"**: the same, showing the sleeping eyes for a few seconds.
- **Two screen modes**, remembered after a restart:
  - **Clock mode**: "Show time", "Show clock", "Clock mode", "Clock dikhao". A big
    dot-matrix clock (hours in the left eye, minutes in the right, always two digits,
    e.g. `09 00`), also while talking.
  - **Face mode**: "Show face", "Show face animations", "Face mode". The animated eyes
    and emotions.
  - **"Change the mode"** switches to the other one.
- **"Light mode" / "Dark mode"**: light mode inverts the screen (dark is the default),
  remembered after a restart.
- **"Time format 12 hours / 24 hours pe kar do"**: switches the clock format.
- **"Pomodoro timer lagao" / "10 minute ka timer"**: while a timer runs and the device
  is idle, the screen counts down minutes : seconds in the same dot font.
- **"Roo ke dikhao", "Gussa dikhao", "Dil wali aankhein dikhao"...**: shows that
  expression (crying, angry, in love, ...) for a few seconds.
- Standby (face mode) shows the big square "surprised" eyes drifting slowly inside the
  eye circles, with slow, relaxed blinks.

## Speech and alarms

- **Words said right after "Alexa" are kept**: the microphone keeps recording while the
  connection opens and sends that speech first, so "Alexa, roo ke dikhao" in one breath
  reaches the server complete.
- **Stopping an alarm or timer by voice**: the beep plays for about 1 s every 3.5 s and
  the wake word is extra sensitive while it rings. Say "Alexa" in a gap between beeps
  (or press any button).
- Speech-to-text runs on the server. Speaking clearly within about 1 m of the mic helps.

## Change the wake word by voice

Say "Alexa, change your wake word to Jarvis". Available names: Alexa, Hi ESP, Jarvis,
Computer, Sophia, Mycroft, Hi Joy, Hi Jason, Hi Andy, Hey Willow, Hey Wanda, Hey Ivy,
Hey Kira, Hi Lily, Hi Telly, Hi Wall E, Nihao Xiaozhi. The device restarts, downloads
the new voice model from the repository's `gh-pages` branch (`wakewords/`, about
0.55 MB, via raw.githubusercontent.com so GitHub Pages does not need to be on), restarts
once more and then answers to the new name. Any other name is not possible: each
wake word is a trained model and only one fits in the 4 MB flash at a time.

## Time

The device clock is synced from the internet and kept on IST. The AI is told to use
the `self.clock.get_time` tool for the time. For best results also add this line to
the role description on xiaozhi.me: *"The user lives in India (IST, UTC+5:30). For the
current time or date always call self.clock.get_time."*

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

## Online music setup (Jamendo, one time)

1. Create a free account at https://devportal.jamendo.com and add an application
   (non-commercial use). Copy its **Client ID**.
2. In GitHub: repository **Settings -> Secrets and variables -> Actions -> New repository
   secret**. Name `JAMENDO_CLIENT_ID`, value = the Client ID. (Optional: the firmware
   has a default client ID in `music_player.cc`; the secret overrides it.)
3. **Actions -> Build RoboThings firmware -> Run workflow**, then flash the new firmware.

Jamendo's API is free for non-commercial use; selling devices with it needs a
commercial licence from Jamendo.
