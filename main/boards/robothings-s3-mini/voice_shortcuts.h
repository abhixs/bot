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
    LightTheme,     // "light mode", "invert the screen", "make screen white"
    DarkTheme,      // "dark mode", "turn off light mode", "screen dark karo"
};

// Lower-case ASCII, punctuation (also the Devanagari danda) turned into spaces,
// single spaces, and a leading / trailing wake word ("alexa") removed.
std::string NormalizeSpeech(const std::string& text);

VoiceShortcut ParseVoiceShortcut(const std::string& text);

const char* VoiceShortcutName(VoiceShortcut shortcut);
