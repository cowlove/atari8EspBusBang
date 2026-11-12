#include "csim.h"

vector<string> listFiles(char const*, char const*) { return {"/FILE1", "/FILE2"}; }
void mmuOnChange(bool force) {}

AtariCart atariCart;
void AtariCart::open(const char *) {}
