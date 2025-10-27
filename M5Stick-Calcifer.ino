#include <M5Unified.h>
#include <LittleFS.h>
#include <AnimatedGIF.h>
#include <esp_sleep.h>
#include <pgmspace.h>
#include <math.h>

#include "screensaver_gif.h"

// ----------- Configuration -----------
static const char* GIF_PATH = "/screensaver.gif";
static const bool LOOP_FOREVER = true;
static const bool CENTER_GIF = true;
static const uint32_t SCREEN_OFF_TO_LIGHTSLEEP_MS = 5UL * 60UL * 1000UL; // 5 minutes
static const gpio_num_t BTN_A_GPIO = GPIO_NUM_37;                         // button A (active-low)
static const float ROTATION_TRIGGER_DEGREES = 150.0f; // roughly 180°
static const float GYRO_NOISE_DEGREES_PER_SEC = 0.3f;
static const float GYRO_DAMPING = 0.98f;

// ----------- State -----------
AnimatedGIF gif;
bool gifOpen = false;
bool screenOn = true;
int16_t dispW = 0;
int16_t dispH = 0;
int16_t offX = 0;
int16_t offY = 0;
uint32_t screenOffTimestamp = 0;
bool autoRotateEnabled = true;

static uint16_t lineBuf[240]; // display width is 240 pixels

enum class ScreenOrientation : uint8_t {
  ButtonRight = 0,
  ButtonLeft  = 1,
};

ScreenOrientation currentOrientation = ScreenOrientation::ButtonRight;
float accumulatedYaw = 0.0f;
float accumulatedFlip = 0.0f;
uint32_t lastGyroSampleMs = 0;

// ----------- Forward declarations -----------
static bool mountFileSystem();
static bool ensureGifStored();
static bool startGif();
static void stopGif();
static void resumeGif();
static void toggleScreen(bool turnOn);
static void enterLightSleep();
static void setupLightSleepWakeByBtnA();
static void applyOrientation(ScreenOrientation orientation, bool redrawImmediate = true);
static void processOrientation();

// AnimatedGIF callbacks for LittleFS
struct GifFileWrap {
  File file;
};

static void* gifOpenFile(const char* fname, int32_t* pSize) {
  auto* wrap = new GifFileWrap();
  if (!wrap) return nullptr;
  wrap->file = LittleFS.open(fname, "r");
  if (!wrap->file) {
    delete wrap;
    return nullptr;
  }
  wrap->file.seek(0, SeekSet);
  *pSize = wrap->file.size();
  return wrap;
}

static void gifCloseFile(void* handle) {
  auto* wrap = static_cast<GifFileWrap*>(handle);
  if (!wrap) return;
  if (wrap->file) {
    wrap->file.close();
  }
  delete wrap;
}

static int32_t gifReadFile(GIFFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  auto* wrap = static_cast<GifFileWrap*>(pFile->fHandle);
  if (!wrap) return 0;
  if (pBuf) {
    int32_t readLen = wrap->file.read(pBuf, iLen);
    pFile->iPos = wrap->file.position();
    return readLen;
  }
  wrap->file.seek(wrap->file.position() + iLen, SeekSet);
  pFile->iPos = wrap->file.position();
  return iLen;
}

static int32_t gifSeekFile(GIFFILE* pFile, int32_t position) {
  auto* wrap = static_cast<GifFileWrap*>(pFile->fHandle);
  if (!wrap) return -1;
  wrap->file.seek(position, SeekSet);
  pFile->iPos = wrap->file.position();
  return pFile->iPos;
}

// ----------- Rendering -----------
static void gifDraw(GIFDRAW* pDraw) {
  int drawY = pDraw->iY + pDraw->y + offY;
  int drawX = pDraw->iX + offX;
  int width = pDraw->iWidth;

  if (drawY < 0 || drawY >= dispH) return;

  if (drawX < 0) {
    int skip = -drawX;
    drawX = 0;
    width -= skip;
    pDraw->pPixels += skip;
  }
  if (drawX + width > dispW) {
    width = dispW - drawX;
  }
  if (width <= 0) return;

  const uint8_t* src = pDraw->pPixels;
  const uint8_t* pal = reinterpret_cast<const uint8_t*>(pDraw->pPalette);

  if (pDraw->ucHasTransparency) {
    M5.Display.startWrite();
    for (int x = 0; x < width; ++x) {
      uint8_t idx = src[x];
      if (idx == pDraw->ucTransparent) continue;
      uint16_t color = ((pal[idx * 3 + 0] & 0xF8) << 8) |
                       ((pal[idx * 3 + 1] & 0xFC) << 3) |
                       (pal[idx * 3 + 2] >> 3);
      M5.Display.drawPixel(drawX + x, drawY, color);
    }
    M5.Display.endWrite();
  } else {
    for (int x = 0; x < width; ++x) {
      uint8_t idx = src[x];
      lineBuf[x] = ((pal[idx * 3 + 0] & 0xF8) << 8) |
                   ((pal[idx * 3 + 1] & 0xFC) << 3) |
                   (pal[idx * 3 + 2] >> 3);
    }
    M5.Display.pushImage(drawX, drawY, width, 1, lineBuf);
  }
}

// ----------- Setup / main loop -----------
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.println("\n[Boot] Screensaver starting");

  M5.Display.setRotation(1); // default orientation: button A on the right
  M5.Display.fillScreen(TFT_BLACK);

  dispW = M5.Display.width();
  dispH = M5.Display.height();
  currentOrientation = ScreenOrientation::ButtonRight;

  gif.begin(GIF_PALETTE_RGB888);

  if (!mountFileSystem()) {
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.drawString("LittleFS error", dispW / 2, dispH / 2);
    while (true) delay(1000);
  }

  if (!ensureGifStored()) {
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.drawString("GIF copy failed", dispW / 2, dispH / 2);
    while (true) delay(1000);
  }

  if (!startGif()) {
    M5.Display.setTextDatum(textdatum_t::middle_center);
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.drawString("Can't open GIF", dispW / 2, dispH / 2);
    while (true) delay(1000);
  }
}

void loop() {
  M5.update();

  if (M5.BtnA.wasPressed()) {
    toggleScreen(!screenOn);
  }
  if (M5.BtnB.wasPressed()) {
    autoRotateEnabled = !autoRotateEnabled;
    accumulatedYaw = 0.0f;
    lastGyroSampleMs = millis();
    Serial.println(autoRotateEnabled ? "[Orientation] Auto-rotate enabled"
                                     : "[Orientation] Auto-rotate disabled");
  }

  processOrientation();

  if (!screenOn) {
    if (millis() - screenOffTimestamp >= SCREEN_OFF_TO_LIGHTSLEEP_MS) {
      enterLightSleep();
    } else {
      delay(20);
    }
    return;
  }

  if (!gifOpen) {
    if (LOOP_FOREVER) {
      startGif();
    } else {
      delay(15);
    }
    return;
  }

  if (!gif.playFrame(true, nullptr)) {
    if (LOOP_FOREVER) {
      startGif();
    } else {
      stopGif();
    }
  }
}

// ----------- Helpers -----------

static bool mountFileSystem() {
  if (LittleFS.begin(false)) {
    Serial.println("[FS] Mounted LittleFS.");
    return true;
  }
  Serial.println("[FS] Mount failed, formatting...");
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Format failed.");
    return false;
  }
  Serial.println("[FS] Formatted and mounted.");
  return true;
}

static bool ensureGifStored() {
  if (LittleFS.exists(GIF_PATH)) {
    File existing = LittleFS.open(GIF_PATH, "r");
    if (existing) {
      size_t currentSize = existing.size();
      existing.close();
      if (currentSize == screensaver_gif_len) {
        Serial.println("[FS] GIF already in LittleFS.");
        return true;
      }
    }
    Serial.println("[FS] GIF mismatch, rewriting.");
    LittleFS.remove(GIF_PATH);
  }

  Serial.printf("[FS] Writing GIF (%u bytes)...\n", screensaver_gif_len);
  File f = LittleFS.open(GIF_PATH, "w");
  if (!f) {
    Serial.println("[FS] Failed to open GIF for writing.");
    return false;
  }

  for (unsigned int i = 0; i < screensaver_gif_len; ++i) {
    uint8_t b = pgm_read_byte(&screensaver_gif[i]);
    if (f.write(b) != 1) {
      Serial.printf("[FS] Write error at byte %u\n", i);
      f.close();
      LittleFS.remove(GIF_PATH);
      return false;
    }
  }
  f.close();
  Serial.println("[FS] GIF stored.");
  return true;
}

static bool startGif() {
  stopGif();

  if (!LittleFS.exists(GIF_PATH)) {
    Serial.println("[GIF] File missing.");
    return false;
  }

  if (gif.open(GIF_PATH, gifOpenFile, gifCloseFile, gifReadFile, gifSeekFile, gifDraw) == 0) {
    Serial.println("[GIF] gif.open failed.");
    return false;
  }

  if (CENTER_GIF) {
    offX = (dispW - gif.getCanvasWidth()) / 2;
    offY = (dispH - gif.getCanvasHeight()) / 2;
  } else {
    offX = 0;
    offY = 0;
  }

  gifOpen = true;
  Serial.println("[GIF] Playback started.");
  gif.playFrame(false, nullptr);
  return true;
}

static void stopGif() {
  if (gifOpen) {
    gif.close();
    gifOpen = false;
    Serial.println("[GIF] Playback stopped.");
  }
}

static void resumeGif() {
  if (gifOpen) {
    gif.reset();
    gif.playFrame(false, nullptr);
  } else {
    startGif();
  }
}

static void toggleScreen(bool turnOn) {
  if (turnOn == screenOn) return;
  screenOn = turnOn;

  if (screenOn) {
    M5.Display.wakeup();
    screenOffTimestamp = 0;
    resumeGif();
    Serial.println("[Screen] ON");
  } else {
    screenOffTimestamp = millis();
    M5.Display.sleep();
    stopGif();
    Serial.println("[Screen] OFF");
  }
}

static uint_fast8_t rotationFor(ScreenOrientation orientation) {
  return (orientation == ScreenOrientation::ButtonRight) ? 1 : 3;
}

static void applyOrientation(ScreenOrientation orientation, bool redrawImmediate) {
  uint_fast8_t desiredRotation = rotationFor(orientation);
  if (M5.Display.getRotation() == desiredRotation) return;

  M5.Display.setRotation(desiredRotation);
  dispW = M5.Display.width();
  dispH = M5.Display.height();

  if (CENTER_GIF && gifOpen) {
    offX = (dispW - gif.getCanvasWidth()) / 2;
    offY = (dispH - gif.getCanvasHeight()) / 2;
  }

  if (screenOn && redrawImmediate) {
    M5.Display.fillScreen(TFT_BLACK);
    if (gifOpen) {
      gif.reset();
      gif.playFrame(false, nullptr);
    } else if (LOOP_FOREVER) {
      startGif();
    }
  }
}

static void processOrientation() {
  if (!autoRotateEnabled || !screenOn) {
    lastGyroSampleMs = millis();
    accumulatedYaw = 0.0f;
    accumulatedFlip = 0.0f;
    return;
  }

  float gx = 0.0f, gy = 0.0f, gz = 0.0f;
  if (!M5.Imu.getGyro(&gx, &gy, &gz)) {
    return;
  }

  uint32_t now = millis();
  if (lastGyroSampleMs == 0) {
    lastGyroSampleMs = now;
    return;
  }

  float dt = (now - lastGyroSampleMs) / 1000.0f;
  lastGyroSampleMs = now;
  if (dt <= 0.0f) return;

  accumulatedYaw += gz * dt;
  float dominantXY = (fabsf(gx) > fabsf(gy)) ? gx : gy;
  accumulatedFlip += dominantXY * dt;

  if (fabsf(gz) < GYRO_NOISE_DEGREES_PER_SEC) {
    accumulatedYaw *= GYRO_DAMPING;
  }
  if (fabsf(dominantXY) < GYRO_NOISE_DEGREES_PER_SEC) {
    accumulatedFlip *= GYRO_DAMPING;
  }

  if (accumulatedYaw > 360.0f || accumulatedYaw < -360.0f) accumulatedYaw = 0.0f;
  if (accumulatedFlip > 360.0f || accumulatedFlip < -360.0f) accumulatedFlip = 0.0f;

  if (fabsf(accumulatedYaw) >= ROTATION_TRIGGER_DEGREES) {
    currentOrientation = (currentOrientation == ScreenOrientation::ButtonRight)
                           ? ScreenOrientation::ButtonLeft
                           : ScreenOrientation::ButtonRight;
    applyOrientation(currentOrientation);
    accumulatedYaw = 0.0f;
    accumulatedFlip = 0.0f;
    return;
  }

  if (fabsf(accumulatedFlip) >= ROTATION_TRIGGER_DEGREES) {
    currentOrientation = (currentOrientation == ScreenOrientation::ButtonRight)
                           ? ScreenOrientation::ButtonLeft
                           : ScreenOrientation::ButtonRight;
    applyOrientation(currentOrientation);
    accumulatedYaw = 0.0f;
    accumulatedFlip = 0.0f;
  }
}

static void setupLightSleepWakeByBtnA() {
  pinMode(static_cast<int>(BTN_A_GPIO), INPUT_PULLUP);
  esp_sleep_enable_ext0_wakeup(BTN_A_GPIO, 0);
}

static void enterLightSleep() {
  Serial.println("[Power] Entering light sleep...");
  stopGif();
  M5.Display.sleep();
  setupLightSleepWakeByBtnA();
  esp_light_sleep_start();

  Serial.println("[Power] Woke from light sleep.");
  M5.Display.wakeup();
  screenOn = true;
  screenOffTimestamp = 0;
  resumeGif();
}
