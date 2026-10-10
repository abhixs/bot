# RoboThings music server

Plays songs from **your own music folder** on the RoboThings S3 Mini assistant.
Say "Alexa, Tum Hi Ho chalao" and the device streams that song from your computer
over your home Wi-Fi. Nothing is uploaded anywhere.

## Setup (Windows)

1. Install **Python 3** from https://www.python.org (tick "Add Python to PATH").
2. Install **ffmpeg**: open PowerShell and run `winget install ffmpeg`
   (or download from https://ffmpeg.org and add its `bin` folder to PATH).
3. Put your songs (MP3, M4A, FLAC, WAV...) in one folder, e.g. `D:\Music`.
   Sub-folders are fine; a folder name like the film name helps search.
4. Double-click `start_music_server.bat`, or run:

   ```
   python robothings_music_server.py "D:\Music"
   ```

5. If Windows Firewall asks, click **Allow** (Private networks).

The computer and the device must be on the same Wi-Fi. Keep the window open
while you listen. Linux/macOS: `python3 robothings_music_server.py ~/Music`.

## Voice commands

- "Alexa, Kesariya chalao" / "play Tum Hi Ho"
- "Alexa, Arijit Singh ke gaane chalao" (best match)
- "Alexa, koi bhi gaana chalao" / "shuffle my songs" (random, non-stop)
- "Alexa, agla gaana" / "next song"
- "Alexa, gaana band karo" / "stop the music"
- "Alexa, gaana wapas chalao" / "resume"

Saying "Alexa" pauses the song while you talk; it continues afterwards unless you
asked to stop. Volume: "Alexa, volume 60 karo" or the volume buttons.

## Tips

- Song names come from file names, so `Tum Hi Ho - Arijit Singh.mp3` works better
  than `track01.mp3`.
- New files are picked up automatically every 2 minutes.
- The device finds the server by itself (UDP port 47123); the server listens on
  TCP port 8765 (`--port` to change).
