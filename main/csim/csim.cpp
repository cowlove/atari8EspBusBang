#include "csim.h"

spiffs_t *spiffs_fs; 
vector<string> spiffsDir(spiffs_t*, char const*, char const*, bool) { return {"/FILE1", "/FILE2"}; }
void mmuOnChange(bool force) {}

AtariCart atariCart;
void AtariCart::open(spiffs_t *, const char *) {}
