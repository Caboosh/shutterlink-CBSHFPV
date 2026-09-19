#ifndef SERIAL_CONFIG_H
#define SERIAL_CONFIG_H

#include <Arduino.h>

/// Initialise the Web Serial bench config protocol. Call from setup(),
/// any time after Serial.begin().
void serialConfigInit();

/// Call from loop(). Non-blocking: consumes whatever bytes are currently
/// available, assembles them into lines, and dispatches any complete line
/// that looks like a JSON command (starts with '{'). Anything else on the
/// input (a human typing into a plain serial terminal, stray bytes) is
/// silently ignored.
void serialConfigUpdate();

#endif // SERIAL_CONFIG_H