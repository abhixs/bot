// Change the wake word by voice ("Alexa, change your wake word to Jarvis").
//
// Wake words are WakeNet models that live in the assets partition, which on a 4MB
// flash only has room for one model. CI therefore publishes one assets image per
// supported wake word next to the web flasher (<pages>/wakewords/<model>.bin).
// Switching stores that URL for the firmware's built-in assets updater and reboots;
// during the next start the device downloads the new assets (about 0.6 MB), then
// restarts once more to load the new voice model and answers to the new name.
#pragma once

void RegisterWakeWordTools();
