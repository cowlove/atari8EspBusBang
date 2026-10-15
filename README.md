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

Atari8EspBusBang uses a naked ESP32-S3 dev board to bit-bang the Atari 8-bit memory bus through the PBI port. The ESP32 has enough speed to handle the 6502's memory requests, overriding base memory and emulating extended memory, multi-bank cartridges, and SIO devices.  It implements a true software-defined PBI device.  The outside world, modern ideas, and high-level code all have direct access to the stock Atari system and memory at 960MB/sec.  

This started as a bus monitor for troubleshooting a flaky 800XL. Once it became clear that the ESP32 could also *respond* to bus requests, the project grew into a software-defined PBI device— and an ultimate extreme performance exercise on the ESP32. The bus-protocol loop runs bare-metal on the ESP32's second core; the first core handles utility tasks such as networking, PBI device logic, and flash disk-image access.

This idea pushes the ESP32 to its absolute performance limit, and the lower level parts of the project are intricate.  I'd like to eventually implement a simple module interface for adding functionality and trying ideas in easy code steps without needing to mess with or understand the low-level internals of this project.  

## Building the Hardware

The KiCad designs, parts list, PCB ordering notes, and assembly notes are in the [hardware README](hardware/README.md).  Total cost is about $20, and requires only through-hole soldering of headers. 

## Building the Firmware

The project depends on a specific pinned and lightly patched version of ESP-IDF v5.5, llvm-mos-sdk, and a few other tools.  Using the provided docker image is probably the easiest way to arrange for this. 

```
./tools/container/setup-build-env.sh container-runtime  # Install software needed to run podman  
./tools/container/container.sh pull                     # fetch/build docker image (6Gb/5min)
./tools/container/container.sh flash /dev/ttyACM0       # build firmware and flash esp32 
```

Or you can install this all directly on your system and run esp-idf tools directly.  
```
./tools/container/setup-build-env.sh          # installs patched esp-idf,etc in ~/opt/ (6Gb/5min)
. ~/opt/atari8esp-idf/export.sh               # set env to run esp-idf tools
idf.py --ccache flash -p /dev/ttyACM0         # build firmware and flash esp32
```

For the pinned Podman build environment, see [`tools/container/container.sh`](tools/container/container.sh) and [its README](tools/container/README.md).


## Contributing

Bug reports and focused contributions are welcome.  Please report issues with the environment setup scripts. 

## License

Distributed under GPL-2.0. See [LICENSE.txt](LICENSE.txt).

## Contact

Jim Evans — [jim@vheavy.com](mailto:jim@vheavy.com)

Project repository: <https://github.com/cowlove/atari8EspBusBang>

Project webpage: <http://cowlove.com/atari8EspBusBang/>
