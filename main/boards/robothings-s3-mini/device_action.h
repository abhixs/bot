// Command mode vs conversation mode for the RoboThings S3 Mini.
//
// A question ("what is AI?", "tell me a joke") keeps the conversation open for a
// follow-up, as before. A device action (alarm, timer, Pomodoro, stopwatch, lamp,
// screen, clock...) ends the conversation once the reply has been spoken, so the
// next request starts with the wake word again.
//
// MCP tools that change the device call NoteDeviceAction(); the board closes the
// conversation when the reply after it is over. Implemented in the board file.
#pragma once

void NoteDeviceAction();
