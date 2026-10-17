# Development Access with a Piggyback ESP32

This note records a proposed way to keep the Atari8EspBusBang test-farm workflow
usable if the busbang ESP32-S3's USB D+/D− pins are reassigned to the S3-to-ESP32
data link. The design goal is **no complicated hardware**: use off-the-shelf ESP
boards, ordinary header wiring, and software-controlled operating modes. In
this document, the original ESP32-S3 is the **busbang ESP32**; the classic ESP32
that manages it is the **piggyback ESP32**.

## Roles

- The **busbang ESP32** continues to run the Atari bus firmware. Its GPIO19/20
  can be used for the planned high-speed link to the piggyback ESP32 when that
  link is enabled.
- The **piggyback ESP32** remains connected to the test-farm host over USB. It
  controls the busbang ESP32's reset (`EN`) and download-mode (`GPIO0`) signals,
  provides a UART path for programming and early boot output, and relays
  runtime diagnostics to the host.
- The test farm can power down the Atari hardware. That provides a maintenance
  window in which bus-connected signals are not being used by the Atari system
  and the piggyback ESP32 can take control of the relevant connections.

## Operating Modes

### Maintenance, boot, and flashing

1. Keep the Atari hardware powered down.
2. Have the piggyback ESP32 take control of the busbang ESP32's UART0 pins and
   reset/boot controls.
3. Capture the S3 ROM boot output and application startup logs over UART0. On
   ESP32-S3, UART0 is GPIO43 (`U0TXD`) and GPIO44 (`U0RXD`).
4. To flash, assert `GPIO0` while resetting `EN`, then release reset into the
   ROM serial download bootloader. The piggyback ESP32 provides the UART data
   path between the host and the busbang ESP32.
5. After programming and startup diagnostics, return the shared pins to their
   runtime roles and power up the Atari hardware.

The exact host-to-piggyback flashing protocol and tooling remain to be designed
and tested. In particular, the chosen bridge must provide the byte transport
and reset/download-mode control expected by the flashing workflow.

### Runtime

- GPIO19/20 carry the planned S3-to-classic I²S/data stream, with the receiving
  peripheral configured on the piggyback ESP32. The stream carries the compact
  video-RAM image; a roughly 1 KiB region is also proposed for high-level log
  records.
- The piggyback ESP32 forwards received log records over its USB serial
  connection to the host/test farm.
- Log publication must be bounded and non-blocking for the bus-critical code.
  The exact memory ownership/record format (for example, a sequence-numbered
  ring or a snapshot) is still open.

## Crash Diagnostics

ESP-IDF supports panic core dumps. For a persistent crash artifact, the
intended option is a dedicated **flash** coredump partition; PSRAM is not a
durable store across power loss and may not be trustworthy after a crash. The
piggyback ESP32 can check for and retrieve a saved dump during the next
maintenance/boot window, before normal Atari operation resumes.

The retrieval mechanism is not yet selected: it could be an application-level
export over UART or I²S, or host tooling operating through the piggyback UART
bridge. The dump partition and recovery workflow need to be enabled and tested
in the actual ESP-IDF project. A crash during runtime may also prevent the
current log record from being published, so persistent core dumps complement
rather than replace the runtime log channel.

## Design Constraints and Open Work

- Do not connect a live USB host to GPIO19/20 while those pins are driven as
  ordinary data signals; USB D+/D− and the runtime link are mutually exclusive.
- Do not let the Atari hardware or another active bus driver contend with the
  UART/control signals during maintenance. The power-down sequence is part of
  the mode transition, not just a convenience.
- Confirm the board-specific wiring for UART0, `EN`, and `GPIO0`, plus suitable
  signal levels and shared ground, before implementing the farm fixture.
- Prototype on one test-farm device first. Validate automated flashing, boot
  log capture, runtime log relay, mode transitions, and core-dump recovery
  before applying the approach to all devices.
- The high-speed link framing, log record format, piggyback firmware, and
  cross-flashing host workflow are design/test work, not yet a completed
  integrated feature.

The intended result is to preserve the automated USB-connected test-farm
workflow without a USB mux: the piggyback ESP32 owns the maintenance path while
the Atari is off, and the dedicated data link carries video state and runtime
logs while the system is running.
