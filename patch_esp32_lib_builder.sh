#!/bin/bash

# Are we not running on docker?  Use this script to prepare and run docker image 
if [ ! -d /arduino-esp32 ]; then 
	git clone -b idf-release/v5.4 git@github.com:espressif/arduino-esp32
	(cd arduino-esp32/tools && python3 ./get.py) 
	cp $0 arduino-esp32/
	docker run -it -v ${PWD}/arduino-esp32:/arduino-esp32 -e DISPLAY=$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix --net=host \
		espressif/esp32-arduino-lib-builder:release-v5.4 \
		/arduino-esp32/patch_esp32_lib_builder.sh
	exit
fi

if [ ! -t ]; then while true; do sleep 1; done; fi

# docker run -it jim-agit_saha_1 -v ${HOME}/src/esp32/:/arduino-esp32 -e DISPLAY=$DISPLAY -v /tmp/.X11-unix:/tmp/.X11-unix --net=host  
# 
# docker cp patch_esp32_lib_builder.sh interesting_beaver:/opt/esp/lib-builder/

#how to patch docker container
cd /opt/esp/lib-builder/
if [ ! -d ./components/arduino ]; then cp -a /arduino-esp32/ ./components/arduino; fi

git clone https://github.com/hathach/tinyusb.git /opt/esp/lib-builder/components/arduino_tinyusb/tinyusb
git -C /opt/esp/lib-builder/components/arduino_tinyusb/tinyusb checkout 72b1fc50e
sed -i  's/exit 1/exit 0/' ./tools/update-components.sh 
grep CONFIG_ESP_INT_WDT_CHECK_CPU1=n ./configs/defconfig.common || echo CONFIG_ESP_INT_WDT_CHECK_CPU1=n >> configs/defconfig.common
grep CONFIG_FREERTOS_UNICORE=y ./configs/defconfig.common || echo CONFIG_FREERTOS_UNICORE=y >> configs/defconfig.common
grep CONFIG_ESP_INT_WDT=n ./configs/defconfig.common || echo CONFIG_ESP_INT_WDT=n >> configs/defconfig.common

./build.sh -t esp32s3 -c /arduino-esp32/
#exec /bin/bash


