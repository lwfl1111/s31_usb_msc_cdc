# ESP32-S31 USB CDC bring-up

Phase 1 for ESP-Mosaico: one TinyUSB CDC-ACM interface, byte echo and a
status line every second. No MSC or automatic download yet.

## Environment and dependencies

- Local ESP-IDF: D:/esp_idf/master/esp-idf, 6.2.0, commit d445e947.
- Target: esp32s31.
- main/idf_component.yml follows the local tusb_serial_device example:
  esp_tinyusb ~3.0.0. The actual patch and transitive TinyUSB versions will
  be resolved on the user's first configure/build. Commit dependencies.lock
  after successful dependency resolution; it has not been generated yet.
- sdkconfig is tracked to preserve the working board configuration.
- sdkconfig.defaults adds the CDC settings for fresh configurations. It is
  not a complete replacement for the tracked board configuration.
- Existing 2 MB flash configuration and single-app partition table are unchanged.

## User build and flash

The agent has not configured, compiled, linked, generated firmware, or flashed.
Use the same ESP-IDF environment and flashing method that worked for Hello World.
The first build needs network access to download the managed USB components.

1. Build from the ESP-IDF extension, or run `idf.py build` in an activated terminal.
2. Close serial tools before flashing. If necessary, power off the board,
   hold BOOT, then power on to enter ROM download mode.
3. Flash using the ROM COM port: `idf.py -p COMxx flash`.
4. Return to normal boot (release BOOT and power-cycle if needed).
5. Find the application's new COM port in Windows Device Manager. Its USB
   product string is `S31 USB CDC Test`; the COM number may differ from ROM.

Do not expect application CDC to accept another flash automatically.
Manual BOOT recovery remains necessary in this phase.

## CDC acceptance

Open the application COM port in a serial terminal with DTR asserted,
115200 / 8N1, no hardware flow control, and local echo disabled.
The baud setting does not determine the USB bus speed.

- A line like `[CDC] alive=1 heap=... tx_dropped=0` appears every second.
- Send `hello` followed by Enter; the same bytes return from the device.
- Close/reopen the terminal and unplug/replug USB; status and echo resume.
- Send a small multiline text and confirm its echo. Periodic status lines may
  interleave with echo. This is a bring-up test, not a lossless bulk protocol.
- DTR must be enabled: the application only transmits when CDC is connected.
- tx_dropped counts bytes not accepted by the TX queue within the bounded send
  interval. It does not prove host receipt or count USB RX FIFO overflow.

Periodic status is sent directly through CDC. printf/ESP_LOG and early boot logs
still use the existing console configuration, not this new CDC interface.
High-Speed enumeration and full console redirection remain separate checks.

## Implementation references

- Local IDF examples/peripherals/usb/device/tusb_serial_device.
- Mosaico factory USB console (commit 2654a4a9):
  https://github.com/esp-mosaico/esp-mosaico-claw/tree/2654a4a9f76f5d7ea108e4dff7ade7b1df4c1c5c/boards/esp_mosaico/components/usb_hs_console

Only its HS USB shutdown workaround is used here, not its auto-download logic.
The original Hello World pytest was removed because it no longer describes
this firmware. Acceptance requires a physical USB host and board.

## CDC standard console

The application calls tinyusb_console_init after CDC initialization,
matching the factory firmware's console hookup. printf, stderr and normal
ESP_LOG output after initialization use CDC. The existing task still owns
RX and echoes via fwrite on stdout; do not add another stdin reader.

The resolved local esp_tinyusb version is 3.0.0 (see dependencies.lock).
This supersedes the earlier note that dependency resolution is pending.

After the user builds/flashes, enable DTR and expect both [printf] and
[ESP_LOGI] lines every second. Echo, close/reopen and unplug/replug must
still work. Startup messages before CDC initialization and ROM/panic output
are not guaranteed on this application USB interface. Initial messages can
be missed before the host opens its port; periodic lines provide validation.

The component VFS uses nonblocking TX. A slow or absent host can lose output;
this is live logging without history or a delivery guarantee. echo_tx_dropped
only counts short echo writes, not lost log bytes. TX newline conversion is
LF (unchanged bytes), preserving exact echo, and printf supplies explicit CRLF.
The agent did not build or flash this console change; hardware validation remains a user step.
