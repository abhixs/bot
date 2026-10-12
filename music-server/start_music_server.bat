@echo off
REM Change the folder below to where your songs are.
set MUSIC_FOLDER=%USERPROFILE%\Music
python "%~dp0robothings_music_server.py" "%MUSIC_FOLDER%"
pause
