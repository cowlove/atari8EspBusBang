# Atari 8-bit ESP32 BusBang

```text
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
@     _   _             _  ___  _     _ _        _____ ____  ____ _________   @
@    / \ | |_ __ _ _ __(_)( _ )| |__ (_) |_     | ____/ ___||  _ \___ /___ \  @
@   / _ \| __/ _` | '__| |/ _ \| '_ \| | __|    |  _| \___ \| |_) ||_ \ __) | @
@  / ___ \ || (_| | |  | | (_) | |_) | | |_     | |___ ___) |  __/___) / __/  @
@ /_/   \_\__\__,_|_|  |_|\___/|_.__/|_|\__|    |_____|____/|_|  |____/_____| @
@                ____            ____                    _ _ _ _              @
@               | __ ) _   _ ___| __ )  __ _ _ __   __ _| | | | |             @
@               |  _ \| | | / __|  _ \ / _` | '_ \ / _` | | | | |             @
@               | |_) | |_| \__ \ |_) | (_| | | | | (_| |_|_|_|_|             @
@               |____/ \__,_|___/____/ \__,_|_| |_|\__, (_|_|_|_)             @
@                                                  |___/                      @
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
READY
█
```

## About the Project

An ESP32-S3 development board can bit-bang the Atari 8-bit memory bus through the PBI port. The timing is tight, but the ESP32-S3 has enough resources to handle the 6502's memory requests, including extended-memory banking, PBI devices, and multi-bank cartridges.

Using the ESP32's internal SRAM for Atari memory opens up ways to experiment with a stock Atari running its original operating system and DOS. Examples include direct SIO transfers between flash and memory, Telnet access to an unmodified Atari through its screen and keyboard buffers, dynamically assembling disk images from an SMB share, and capturing bus traces with trigger filters and up to two million samples.

This started as a bus monitor for troubleshooting a flaky 800XL. Once it became clear that the ESP32 could also respond to bus requests, the project grew into a software-defined PBI device—and a real-time performance exercise. The bus-protocol loop runs on the ESP32's second core; the first handles work such as networking and disk-image access.

The design pushes the ESP32-S3 close to its real-time limits, so timing and hardware compatibility matter.

## Features

- Service Atari memory requests using ESP32 SRAM, including extended-memory banking.
- Provide PBI ROM functionality and support multi-bank cartridges.
- Transfer SIO data directly to/from ESP32 memory for instant disk I/O with stock DOS.
- Offer Telnet access to an unmodified Atari by reading text-screen memory and updating the keyboard buffer.
- Present files from an SMB network share as a dynamically assembled disk image for stock DOS.
- Capture Atari memory-bus traces with trigger filters, pre-roll, and up to two million samples.

## Building the Firmware

The project uses ESP-IDF and targets the ESP32-S3. Project-specific setup, build, and flash notes will be added here.

For the pinned Podman build environment, see [`tools/container/container.sh`](tools/container/container.sh) and [its README](tools/container/README.md).

## Building the Hardware

The KiCad designs, parts list, PCB ordering notes, and assembly warnings are in the [hardware README](hardware/README.md).

## Contributing

Bug reports and focused contributions are welcome. Please open an issue to discuss larger changes before submitting a pull request.

## License

Distributed under GPL-2.0. See [LICENSE.txt](LICENSE.txt).

## Contact

Jim Evans — [jim@vheavy.com](mailto:jim@vheavy.com)

Project repository: <https://github.com/cowlove/atari8EspBusBang>

Project webpage: <http://cowlove.com/atari8EspBusBang/>
