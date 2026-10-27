# Arduino Wristband Controller

This Arduino program drives the CrowdSync test contact so commands can be sent without an RF transmitter. It includes repeated and single-packet commands, adjustable timing, raw packet transmission, and test-contact presses for selecting the local demonstrations.

The sketch targets a classic **5 V, 16 MHz ATmega328P Arduino Uno or Nano**. Its output only pulls LOW or becomes HI-Z; the wristband's pull-up provides the HIGH level.

## Wiring

| Arduino Pin | Wristband Connection | Purpose |
|---|---|---|
| D9 | MCU pin 4 / test contact | Sends the encoded packet or grounds the contact for a local press. |
| D8 | MCU pin 2 / receiver enable | Optional input that monitors when the receiver is powered. Required for `QUIET ON`. |
| GND | MCU pin 8 / board ground | Common ground. |

Power the wristband through its normal supply and the Arduino through USB. Do not connect the Arduino's 5 V or 3.3 V output to wristband power. No divider is needed on D9 because the program never drives it HIGH or enables its pull-up. Keep both devices powered while connected and keep the signal wire short.

D8 only observes the receiver-enable signal; the Arduino does not drive or switch receiver power itself.

## Sending a Color

1. Open [CrowdSync_Arduino_Wired_Controller.ino](CrowdSync_Arduino_Wired_Controller.ino), select an Uno or classic Nano, and upload the sketch.
2. Open Serial Monitor at **115200 baud**, with newline endings.
3. If D8 is connected, send `QUIET ON` to wait for receiver-off windows.
4. Send `6 0 15 0 0` to start repeating steady-red packets for group 6.
5. Try `6 0 0 15 0` for green, `6 0 0 0 15` for blue, or `6 4 15 15 15` for a white blink.
6. To finish, send `6 0 0 0 0`, allow it to be received, then send `STOP`.

The numeric format is:

```text
GROUP EFFECT RED GREEN BLUE
```

All five values are decimal. Groups 1-10 update one cached group assignment. Group 0 fills all ten assignments with the same effect and color. Effect and RGB values are 0-15; see [the effect table](../OPERATION.md#led-effects).

Updating one group preserves the other cached assignments. On startup, those assignments contain black commands rather than an ignore value.

## Serial Commands

| Command | Description |
|---|---|
| `GROUP EFFECT R G B` | Updates the selected group and starts repeating complete packets. |
| `STOP` | Stops repeated or queued one-shot transmission after the current frame, then releases D9. It does not send a black command. |
| `STATUS` | Prints streaming state, quiet mode, and generated/aborted frame counts without changing a queued transmission. These counts are not wristband acknowledgements. |
| `QUIET ON` / `QUIET OFF` | Enables or disables receiver-enable monitoring. The default is OFF. |
| `PERIOD ms` | Sets the minimum interval between frame starts, from 0 to 60000 ms. Zero sends continuously, approximately one frame every 89 ms. |
| `ONCE GROUP EFFECT R G B` | Updates the cached assignment and sends one complete packet, then stops. |
| `RAW <22 hexadecimal bytes>` | Sends exactly the supplied bytes once. It does not calculate or repair the checksum or trailer. |
| `TIMING short long sync gap` | Changes the four delay settings in microseconds. Defaults are `120 320 1600 2400`. |
| `PREFIX ON` / `PREFIX OFF` | Enables or disables the extra 600 µs synchronization guard. The default is ON. |
| `TAP ms` | Stops sending, pulls D9 LOW for 1-2500 ms, then releases it. Used to exercise the local test-contact input. |

For example, to repeat a random blue fade-in command with a minimum 500 ms interval:

```text
PERIOD 500
6 10 0 0 15
```

To send one steady-green packet:

```text
STOP
ONCE 6 0 0 15 0
```

A single packet can be missed during the wristband's ordinary receiver-off interval. Repeating a command is preferable when first acquiring the device.

### Raw Packets & Timing Controls

`RAW` is useful for checking packet fields, checksum handling and malformed inputs. Its byte values are hexadecimal even though the effect numbers in the normal interface are decimal. This example sends steady red to group 6, with all other groups black:

```text
RAW 00 00 00 00 00 00 00 00 00 00 0F 00 00 00 00 00 00 00 00 00 0F 00
```

Before sending a long RAW line, send STOP and wait for `Stopped; D9 released (HI-Z).` The Arduino's receive buffer is 64 bytes, and a long command arriving during a frame can lose characters. Waiting at least 200 ms after STOP also leaves enough time for a normal frame to finish. Send one command at a time and wait for the response before continuing.

The TIMING and PREFIX commands allow deliberate boundary tests. Changing these values does not guarantee that the resulting waveform will be accepted. The default settings include the guard and final closing pulse needed for reliable framing.

## Receiver Interference & Quiet Mode

Receiver data and test-contact edges share the wristband's decoder. Noise from a powered receiver can therefore corrupt a wired packet even when D9 generates the expected waveform.

With `QUIET ON`, the controller waits until D8 has remained LOW for 500 µs before starting. It releases D9 and abandons the remainder of a frame if receiver enable becomes HIGH. An abort during the closing pulse or gap can happen after the payload was already received.

The PERIOD setting is a minimum spacing, not a guarantee of the actual send rate. Random effects can leave the receiver enabled for long intervals, delaying quiet-mode sends beyond the roughly six-second communication timeout. The wristband then returns to local acquisition, where another quiet window becomes available. Observe the LED response or the input waveform when checking reception rather than relying only on the sent count.

Effect 15 is different: its long receiver-off phase also disables the decoder. Quiet-mode transmission cannot recover it during that phase. Wired recovery was not demonstrated. Use a power cycle or the [RF wake procedure](../OPERATION.md#receiver-power) to leave effect 15.

## Local Demonstrations

To select local demonstrations, stop transmission and wait at least six seconds for remote control to time out. A `TAP 50` command then simulates a short press. Each press advances one position in the [local demonstration sequence](../OPERATION.md#local-demonstration-modes).

A `TAP 5` pulse is too short to be a normal press. A long hold such as `TAP 2200` has no assigned action, including when it is released. The Arduino is busy for the requested TAP duration, so wait for its completion response before sending another command.
