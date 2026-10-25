#ifdef CSIM
#include "csim.h"
#endif

#include "arduinoLite.h"

extern void setup();
extern void loop();

#ifndef ARDUINO
extern "C" 
void app_main(void) { 
    setup();
#ifndef CSIM
    while(1) { loop(); }
#endif
}

ArduinoSerial Serial;
#endif
