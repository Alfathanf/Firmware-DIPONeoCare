#pragma once

#include <Arduino.h>

namespace sdManager {
bool begin();
bool isAvailable();
uint32_t pendingFileCount();
bool saveFailedUploadFrame(
    const uint8_t* jpegBuffer,
    size_t jpegLength,
    const String& captureId,
    const String& captureTimestamp);
bool nextPendingFile(String& jpegPath, String& metaPath);
bool readPendingMetadata(
    const String& metaPath,
    String& captureId,
    String& captureTimestamp);
bool readPendingJpeg(
    const String& jpegPath,
    uint8_t** outBuffer,
    size_t& outLength);
bool deletePendingFile(const String& jpegPath, const String& metaPath);
void printStatus();
}  // namespace sdManager
