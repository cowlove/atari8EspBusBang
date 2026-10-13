# Atari 8-bit ESP32 BusBang

Firmware for an ESP32-S3 that interfaces with an Atari 8-bit computer through its PBI port. It began as a bus monitor for diagnosing an 800XL, then grew into a software-defined PBI device capable of observing and responding to the Atari's memory-bus requests.

The timing-critical bus loop runs on the ESP32-S3's second core. The other core handles higher-level work such as networking and disk-image access. The design pushes the ESP32-S3 close to its real-time limits, so timing and hardware compatibility matter.

## Features

- Service Atari memory requests using ESP32 SRAM, including extended-memory banking.
- Provide PBI ROM functionality and support multi-bank cartridges.
- Transfer SIO data directly between the Atari and ESP32 memory for fast disk I/O.
- Offer Telnet access to an unmodified Atari by reading text-screen memory and updating the keyboard buffer.
- Present files from an SMB network share as a dynamically assembled disk image for stock DOS.
- Capture Atari memory-bus traces with trigger filters, pre-roll, and up to two million samples.

## Build and Flash

The project uses ESP-IDF and targets the ESP32-S3. Set up the ESP-IDF environment, connect a compatible ESP32-S3 board, then run:

```sh
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

The board must also be connected to the Atari's PBI port using compatible hardware. This repository's firmware build does not provide a complete hardware construction guide.

## Contributing

Bug reports and focused contributions are welcome. Please open an issue to discuss larger changes before submitting a pull request.

## License

Distributed under GPL-2.0. See [LICENSE.txt](LICENSE.txt).

## Contact

Jim Evans — [jim@vheavy.com](mailto:jim@vheavy.com)

Project repository: <https://github.com/cowlove/atari8EspBusBang>
