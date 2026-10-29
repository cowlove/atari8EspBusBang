#pragma once

#include <esp_attr.h>

#include "main.h"
#include "pbi.h"

int handlePbiAptDcb(PbiIocb *request, AtariDCB *dcb);
