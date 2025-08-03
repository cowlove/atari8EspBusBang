#include "arduinoLite.h"

extern void setup();
extern void loop();

#ifndef ARDUINO
extern "C" 
void app_main(void) { 
    setup();
    while(1) { loop(); }
}

ArduinoSerial Serial;
#endif
