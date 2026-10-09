// Local voice shortcuts for the RoboThings S3 Mini.
//
// The server's speech-to-text result (what the user said) is checked on the device
// first. Short commands that only concern the device itself - going to standby,
// the screen mode and the screen theme - are recognised from their key words, so
// natural variations work ("show me the time", "screen par time dikhao", "time
// dikhao", ...), and are carried out at once, without waiting for the AI.
//
// Anything longer or not clearly one of these commands returns None and is left
// to the AI (which can still use the MCP tools).
#pragma once

#include <string>

enum class VoiceShortcut {
    None,
    Standby,        // "bye", "bye bye", "goodbye", "chup raho"...: silent standby
    Sleep,          // "so jao": standby showing the sleeping eyes
    TimeMode,       // "show time", "clock dikhao", "change the mode to time"
    EmotionMode,    // "show face", "emotions dikhao", "change the mode to emotions"
    ToggleMode,     // "change the mode", "switch mode"
    TimerMode,      // "show timer", "show countdown", "timer dikhao"
    TimerSet,       // "set a timer for 10 minutes", "10 minute ka timer lagao" (seconds)
    Extend,         // "extend 10 minutes", "10 minute aur" (seconds, default 10 min)
    StopAlarm,      // "stop", "alarm band karo" (ringing alarm / Pomodoro decision)
    StopTimer,      // "stop the timer": the stopwatch, else a ringing alarm
    PomodoroStart,  // "set pomodoro timer", "pomodoro start karo" (seconds = focus, optional)
    PomodoroStop,   // "stop pomodoro", "pomodoro band karo"
    PomodoroPause,  // "pause the pomodoro"
    PomodoroResume, // "resume / continue the pomodoro"
    StopwatchStart, // "start stopwatch", "start the timer" (no duration)
    StopwatchPause, // "pause stopwatch", "pause"
    StopwatchResume,// "resume stopwatch", "resume"
    StopwatchStop,  // "stop stopwatch", "end stopwatch", "reset stopwatch"
    StopwatchShow,  // "show stopwatch"
    ShowExpression, // "rone wala chehra dikhao", "excited ho ke dikhao" (expression)
    LightTheme,     // "light mode", "invert the screen", "make screen white"
    DarkTheme,      // "dark mode", "turn off light mode", "screen dark karo"
};

// Lower-case ASCII, punctuation (also the Devanagari danda) turned into spaces,
// single spaces, and a leading / trailing wake word ("alexa") removed.
std::string NormalizeSpeech(const std::string& text);

struct VoiceCommand {
    VoiceShortcut type = VoiceShortcut::None;
    int seconds = 0;  // duration for TimerSet / Extend / PomodoroStart (0 = none given)
    const char* expression = nullptr;  // ShowExpression: e.g. "crying"
};

VoiceCommand ParseVoiceCommand(const std::string& text);
VoiceShortcut ParseVoiceShortcut(const std::string& text);  // ParseVoiceCommand(text).type

// Duration spoken in the text ("10 minutes", "ten minute", "दस मिनट", "1 hour 30
// minutes", "half an hour"), in seconds; 0 when there is none.
int ParseDuration(const std::string& normalized_text);

const char* VoiceShortcutName(VoiceShortcut shortcut);
