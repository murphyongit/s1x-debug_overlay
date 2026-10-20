#pragma once
#include <windows.h>

// Starts the add-on. Safe to call more than once; only the first call does anything.
void overlay_start(HMODULE self);
