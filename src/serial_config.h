#ifndef SERIAL_CONFIG_H
#define SERIAL_CONFIG_H

#include <Arduino.h>

/// Initialise the Web Serial bench config protocol. Call from setup(),
/// any time after Serial.begin().
void serialConfigInit();

/// Call from loop(). Non-blocking: consumes whatever bytes are currently
/// available on the direct USB bench port (Serial), assembles them into
/// lines, and dispatches any complete line that looks like a JSON command
/// (starts with '{'). Anything else on the input (a human typing into a
/// plain serial terminal, stray bytes) is silently ignored.
void serialConfigUpdate();

/// Feed one byte that arrived on Serial1 (the FC UART) into the same
/// JSON-line bench-config protocol, on top of (never instead of) whatever
/// else already consumes that byte (the MSP parser in main.cpp's
/// mspReadIncoming()). This is what lets the Configurator be reached
/// through a Betaflight serial passthrough session targeting the UART
/// wired to this board — no direct USB cable to the C3 required, just the
/// FC's own USB port with passthrough engaged on that UART.
///
/// Serial1 can only be read once per byte (HardwareSerial has no "peek and
/// leave it" for a stream two independent consumers can each drain), so
/// this must be called from main.cpp's existing Serial1.read() loop —
/// never call Serial1.available()/read() independently in here.
void serialConfigFeedFcUartByte(uint8_t b);

/// True while a bench command has been received over Serial1 (the FC UART)
/// within the last FC_UART_BENCH_IDLE_MS — i.e. a Betaflight serial
/// passthrough session is actively bridging that UART to a browser rather
/// than a live FC. main.cpp uses this to pause its own periodic MSP polling
/// (mspPollRC(), fcStatusUpdate()) during that window: those requests would
/// otherwise still go out over Serial1 as always, get faithfully forwarded
/// by the passthrough bridge straight back to the browser, and corrupt the
/// JSON line framing there (raw MSP frames contain no '\n', so they glue
/// onto the front of the next real JSON reply). There's no live FC to
/// poll during passthrough anyway, so pausing costs nothing.
bool serialConfigFcUartActive();

#endif // SERIAL_CONFIG_H
