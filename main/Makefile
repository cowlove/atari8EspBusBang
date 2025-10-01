BOARD=esp32s3-octal
CHIP=esp32
PORT ?= /dev/ttyACM0

ALIBS=${HOME}/Arduino/libraries
EXCLUDE_DIRS=${ALIBS}/lvgl|${ALIBS}/LovyanGFX|${ALIBS}/esp32csim|${ALIBS}/PubSubClient/tests
PART_FILE=partitions.csv
FLASH_DEF=8M
GIT_VERSION := "$(shell git describe --abbrev=6 --dirty --always)"
ESP_ROOT=../arduino-esp32
ARDUINO_LIBS="${ESP_ROOT}/libraries ${HOME}/Arduino/libraries"

CDC_ON_BOOT=1
#BUILD_EXTRA_FLAGS += -DBOARD_HAS_PSRAM 
#BUILD_MEMORY_TYPE=qio_opi
UPLOAD_PORT ?= ${PORT}

BUILD_EXTRA_FLAGS += -DGIT_VERSION=\"$(GIT_VERSION)\" -O1
BUILD_EXTRA_FLAGS += -mno-serialize-volatile
BUILD_EXTRA_FLAGS += ${DEF}

#sed  's|^\(.*/srmodels.bin\)|#\1|g' -i ~/.arduino15/packages/esp32/hardware/esp32/3.2.0/boards.txt  
#
csim-build:
	${MAKE} BOARD=csim csim 

ifeq ($(BOARD),csim)
CSIM_BUILD_DIR=./build/csim
CSIM_INC=-I./littlefs
CSIM_SRC_DIRS=./littlefs
include ${ALIBS}/esp32csim/csim.mk



else
#include ${ALIBS}/makeEspArduino/makeEspArduino.mk
#include ../makeEspArduino/makeEspArduino.mk
endif

1cat:
	stty -F ${PORT} -echo raw 921600 && cat ${PORT} 
cat:    
	while sleep .01; do if [ -c ${PORT} ]; then stty -F ${PORT} -echo raw 921600 && cat ${PORT}; fi; done  | tee ./cat.`basename ${PORT}`.out
socat:  
	socat udp-recvfrom:9000,fork - 
mocat:
	mosquitto_sub -h rp1.local -t "${MAIN_NAME}/#" -F "%I %t %p"   
uc:		pbirom.h
	${MAKE} upload && ${MAKE} cat

u1c:		pbirom.h
	${MAKE} upload && sleep 1 && ${MAKE} 1cat

cuc:	pbirom.h
	${MAKE} clean && ${MAKE} upload && ${MAKE} cat

backtrace:
	tr ' ' '\n' | addr2line -f -i -e /tmp/mkESP/atari8EspBusBang_esp32s3-octal/*.elf


sysMon: sysMonitor.h csim/sysMonitorTest.cpp sysMonitor.cpp
	g++ -DCSIM -g -I. -I./csim csim/sysMonitorTest.cpp -o sysMon

pbirom.h:  pbirom.o65
	../tools/bin2src.py pbirom.o65 > pbirom.h

pbirom.o65:  pbirom.asm asmDefs.h
	~/bin/xa -C pbirom.asm -P pbirom.lst -o pbirom.o65

page6.h:  page6.o65
	../tools/bin2src.py page6.o65 > page6.h

page6.o65:  page6.asm
	xa -C page6.asm -o page6.o65
