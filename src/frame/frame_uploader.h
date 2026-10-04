#pragma once

#include <Arduino.h>

void frameUploaderInit();
void frameUploaderUpdate();
void frameUploaderRegisterSuccessfulCapture(
	const String& captureId,
	const String& captureTimestamp);
bool frameUploaderGetCaptureIdsForWindow(
	const String& startedAt,
	const String& endedAt,
	String* captureIds,
	String* captureTimestamps,
	size_t maxCaptureIds,
	size_t* actualCaptureCount);