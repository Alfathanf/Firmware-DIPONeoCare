#include "sd/sd_manager.h"

#include <Preferences.h>
#include <SD.h>
#include <SPI.h>

#include "config.h"

namespace {
constexpr char kPreferencesNamespace[] = "sd";
constexpr char kSequenceKey[] = "next_seq";
constexpr char kPendingDirectory[] = "/unsent";
bool g_initialized = false;

String sanitizeFileStem(const String& value) {
    String clean;
    clean.reserve(value.length());
    for (size_t index = 0; index < value.length(); ++index) {
        const char character = value[index];
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '_' || character == '-' || character == '.') {
            clean += character;
        } else {
            clean += '_';
        }
    }
    if (clean.length() == 0) {
        clean = "frame";
    }
    return clean;
}

uint32_t nextSequenceNumber() {
    Preferences prefs;
    if (!prefs.begin(kPreferencesNamespace, false)) {
        return 0;
    }

    uint32_t nextValue = prefs.getUInt(kSequenceKey, 0) + 1U;
    prefs.putUInt(kSequenceKey, nextValue);
    prefs.end();
    return nextValue;
}

String paddedSequence(uint32_t value) {
    String text = String(value);
    while (text.length() < 8) {
        text = String("0") + text;
    }
    return text;
}

bool ensurePendingDirectory() {
    if (SD.exists(kPendingDirectory)) {
        return true;
    }

    if (SD.mkdir(kPendingDirectory)) {
        return true;
    }

    Serial.println("[SD] Failed to create /unsent directory");
    return false;
}

bool listPendingFiles(String* filePaths, size_t maxFiles, size_t& actualCount) {
    actualCount = 0;
    if (!SD.exists(kPendingDirectory)) {
        return false;
    }

    File root = SD.open(kPendingDirectory);
    if (!root || !root.isDirectory()) {
        return false;
    }

    while (File entry = root.openNextFile()) {
        const String entryPath = entry.name();
        if (entry.isDirectory()) {
            entry.close();
            continue;
        }

        if (entryPath.endsWith(".jpg")) {
            if (actualCount < maxFiles) {
                filePaths[actualCount++] = entryPath;
            }
        }
        entry.close();
    }
    root.close();
    return actualCount > 0;
}
}  // namespace

namespace sdManager {

bool begin() {
    if (g_initialized) {
        return true;
    }

    SPI.begin(
        config::kSdSpiSckPin,
        config::kSdSpiMisoPin,
        config::kSdSpiMosiPin,
        config::kSdSpiCsPin);

    if (!SD.begin(config::kSdSpiCsPin, SPI, 8000000)) {
        Serial.println("[SD] Initialization failed");
        g_initialized = false;
        return false;
    }

    if (!ensurePendingDirectory()) {
        g_initialized = false;
        return false;
    }

    g_initialized = true;
    printStatus();
    return true;
}

bool isAvailable() {
    return g_initialized && SD.cardSize() > 0;
}

uint32_t pendingFileCount() {
    if (!isAvailable()) {
        return 0;
    }

    const size_t maxEntries = 256;
    String paths[maxEntries];
    size_t actualCount = 0;
    listPendingFiles(paths, maxEntries, actualCount);
    return static_cast<uint32_t>(actualCount);
}

bool saveFailedUploadFrame(
    const uint8_t* jpegBuffer,
    size_t jpegLength,
    const String& captureId,
    const String& captureTimestamp) {
    if (jpegBuffer == nullptr || jpegLength == 0 || !isAvailable()) {
        return false;
    }

    const String safeStem = sanitizeFileStem(
        captureId.length() > 0 ? captureId : "frame");
    const String filePrefix = "frame_" + paddedSequence(nextSequenceNumber()) + "_" + safeStem;
    const String jpegPath = String(kPendingDirectory) + "/" + filePrefix + ".jpg";
    const String metaPath = String(kPendingDirectory) + "/" + filePrefix + ".meta";

    File jpegFile = SD.open(jpegPath.c_str(), FILE_WRITE);
    if (!jpegFile) {
        Serial.printf("[SD] Failed to open file for write: %s\n", jpegPath.c_str());
        return false;
    }

    const size_t written = jpegFile.write(jpegBuffer, jpegLength);
    jpegFile.close();
    if (written != jpegLength) {
        SD.remove(jpegPath.c_str());
        Serial.printf("[SD] Write mismatch for %s: expected %u got %u\n",
            jpegPath.c_str(),
            static_cast<unsigned>(jpegLength),
            static_cast<unsigned>(written));
        return false;
    }

    File metaFile = SD.open(metaPath.c_str(), FILE_WRITE);
    if (!metaFile) {
        SD.remove(jpegPath.c_str());
        Serial.printf("[SD] Failed to create metadata file: %s\n", metaPath.c_str());
        return false;
    }

    metaFile.print("captureId=");
    metaFile.println(captureId);
    metaFile.print("timestamp=");
    metaFile.println(captureTimestamp);
    metaFile.close();

    Serial.printf("[SD] Saved failed upload frame to %s (%u bytes)\n",
        jpegPath.c_str(),
        static_cast<unsigned>(jpegLength));
    return true;
}

bool nextPendingFile(String& jpegPath, String& metaPath) {
    if (!isAvailable()) {
        return false;
    }

    if (!SD.exists(kPendingDirectory)) {
        return false;
    }

    File root = SD.open(kPendingDirectory);
    if (!root || !root.isDirectory()) {
        return false;
    }

    String bestJpeg;
    while (File entry = root.openNextFile()) {
        const String candidate = entry.name();
        if (entry.isDirectory()) {
            entry.close();
            continue;
        }

        if (!candidate.endsWith(".jpg")) {
            entry.close();
            continue;
        }

        if (bestJpeg.length() == 0 || candidate.compareTo(bestJpeg) < 0) {
            bestJpeg = candidate;
        }
        entry.close();
    }
    root.close();

    if (bestJpeg.length() == 0) {
        return false;
    }

    jpegPath = bestJpeg;
    metaPath = bestJpeg.substring(0, bestJpeg.length() - 4) + ".meta";
    return true;
}

bool readPendingMetadata(
    const String& metaPath,
    String& captureId,
    String& captureTimestamp) {
    if (!isAvailable()) {
        return false;
    }

    File metaFile = SD.open(metaPath.c_str(), FILE_READ);
    if (!metaFile) {
        return false;
    }

    captureId = "";
    captureTimestamp = "";
    while (metaFile.available()) {
        String line = metaFile.readStringUntil('\n');
        line.trim();
        if (line.startsWith("captureId=")) {
            captureId = line.substring(strlen("captureId="));
        } else if (line.startsWith("timestamp=")) {
            captureTimestamp = line.substring(strlen("timestamp="));
        }
    }
    metaFile.close();
    return captureId.length() > 0 && captureTimestamp.length() > 0;
}

bool readPendingJpeg(
    const String& jpegPath,
    uint8_t** outBuffer,
    size_t& outLength) {
    if (outBuffer == nullptr) {
        return false;
    }

    *outBuffer = nullptr;
    outLength = 0;

    if (!isAvailable()) {
        return false;
    }

    File jpegFile = SD.open(jpegPath.c_str(), FILE_READ);
    if (!jpegFile) {
        return false;
    }

    const size_t fileSize = jpegFile.size();
    if (fileSize == 0) {
        jpegFile.close();
        return false;
    }

    uint8_t* data = static_cast<uint8_t*>(malloc(fileSize));
    if (data == nullptr) {
        jpegFile.close();
        return false;
    }

    const size_t bytesRead = jpegFile.read(data, fileSize);
    jpegFile.close();
    if (bytesRead != fileSize) {
        free(data);
        return false;
    }

    *outBuffer = data;
    outLength = fileSize;
    return true;
}

bool deletePendingFile(const String& jpegPath, const String& metaPath) {
    if (!isAvailable()) {
        return false;
    }

    bool deleted = true;
    if (SD.exists(jpegPath.c_str())) {
        deleted = SD.remove(jpegPath.c_str());
    }

    if (SD.exists(metaPath.c_str())) {
        if (!SD.remove(metaPath.c_str())) {
            deleted = false;
        }
    }

    if (deleted) {
        Serial.printf("[SD] Synchronized and deleted %s\n", jpegPath.c_str());
    }
    return deleted;
}

void printStatus() {
    if (!isAvailable()) {
        Serial.println("[SD] Offline storage disabled");
        return;
    }

    Serial.println("========================================");
    Serial.println("[SD] MicroSD status");
    Serial.println("========================================");
    Serial.printf("[SD] Card type: %d\n", SD.cardType());
    Serial.printf("[SD] Total size: %llu bytes\n", static_cast<unsigned long long>(SD.cardSize()));
    Serial.printf("[SD] Used bytes: %llu\n", static_cast<unsigned long long>(SD.usedBytes()));
    Serial.printf("[SD] Free bytes: %llu\n", static_cast<unsigned long long>(SD.cardSize() - SD.usedBytes()));
    Serial.printf("[SD] Pending frames: %u\n", static_cast<unsigned>(pendingFileCount()));
    Serial.println("========================================");
}

}  // namespace sdManager
