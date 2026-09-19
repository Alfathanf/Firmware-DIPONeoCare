#pragma once

#include <Arduino.h>

bool httpPostJson(
    const String& url,
    const String& payload,
    const String& token,
    String& responseBody,
    int& httpCode);
