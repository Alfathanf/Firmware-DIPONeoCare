#pragma once

#include <Arduino.h>
#include <esp_camera.h>

bool cameraInit();
camera_fb_t* cameraCapture();
void cameraRelease(camera_fb_t* fb);
bool cameraIsReady();
bool cameraIsPsramAvailable();
void cameraPrintDebugInfo();
