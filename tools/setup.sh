#!/bin/bash
# on host machine:
# sudo apt-get install docker.io apt-cacher-ng
if [ ! -d /arduino ]; then 
	docker run -v ${PWD}:/arduino -v ${HOME}/.arduino15/staging:/root/.arduino15/staging -it ubuntu /arduino/setup.sh
	exit
fi

echo 'Acquire::http { Proxy "http://172.17.0.1:3142"; };' | tee /etc/apt/apt.conf.d/01proxy
ln -s /usr/share/zoneinfo/America/Los_Angeles /etc/localtime
apt-get update
apt-get install -yq git curl build-essential python3 

mkdir -p ${HOME}/bin
export BINDIR=${HOME}/bin
export PATH=$PATH:${HOME}/bin
/root/.arduino15/staging/arduino-cli.$(uname -m) version && ln -s /root/.arduino15/staging/arduino-cli.$(uname -m) ${HOME}/bin/arduino-cli
arduino-cli version || curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | sh

arduino-cli config init 
sed -i 's|additional_urls: \[\]|additional_urls: \[https://dl.espressif.com/dl/package_esp32_index.json,http://arduino.esp8266.com/stable/package_esp8266com_index.json\]|' ~/.arduino15/arduino-cli.yaml 
arduino-cli update
arduino-cli core install esp32:esp32
arduino-cli lib install ArduinoOTA PubSubClient HTTPClient OneWireNg \
	ArduinoJson Arduino_CRC32 "DHT sensor library" "Adafruit HX711" 

mkdir -p ${HOME}/Arduino/libraries 
cd ${HOME}/Arduino/libraries 
git config --global user.name "Jim Evans"
git config --global user.email "jim@vheavy.com"
printf "Host *\n StrictHostKeyChecking no" >> ~/.ssh/config
chmod 600 ~/.ssh/config

git clone https://github.com/cowlove/esp32jimlib
git clone https://github.com/cowlove/esp32csim.git
git clone https://github.com/cowlove/makeEspArduino.git


cd ${HOME}
SKETCH=$( echo `basename /arduino/*.ino` | sed s/.ino// )

git clone https://github.com/cowlove/${SKETCH}.git

cd ${SKETCH}
make 

# makeEspArduino needs needs a preferences.txt file 
#echo sketchbook.path=${HOME}/Arduino >> ~/.arduino15/preferences.txt

echo Sleeping to leave docker container usable...
while true; do sleep 1; done

