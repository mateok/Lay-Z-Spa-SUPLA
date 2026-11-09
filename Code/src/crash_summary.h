#pragma once

#include <stdint.h>

namespace CrashDiagnostics
{
struct Summary
{
    uint32_t reason = 0;
    uint32_t exceptionCause = 0;
    uint32_t programCounter = 0;
    uint32_t source = 0;
};

void begin(bool filesystemMounted);
const Summary& lastCrash();
const char* reasonName(uint32_t reason);
const char* sourceName(uint32_t source);
}
