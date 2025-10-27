# M5Stick-Calcifer
🔥 Calcifer Screensaver — a warm little fire spirit living inside your M5StickC Plus 2.

![Calcifer Screensaver](https://i.imgur.com/31ib0fP.gif)

“Calcifer” is a playful sketch for the M5StickC Plus2 that plays an embedded GIF from LittleFS and keeps the display aligned with the device orientation. The firmware handles rotations both in the screen plane (button A on the left/right) and front/back flips. Orientation lock is just a button press away.

This project was created as an experimental build with the CODEX assistant: it started as a fun playground, yet the codebase is tidy and ready for further expansion.

## Repository layout

- `M5Stick-Calcifer/`
  - `M5Stick-Calcifer.ino` – the main sketch with IMU-driven auto-rotation.
  - `screensaver_gif.h` – the GIF embedded in PROGMEM, copied to LittleFS on first boot.
  - `screensaver.gif` – source animation (optional reference/editing).

## Requirements

- M5StickC Plus2.
- Arduino IDE 2.x or `arduino-cli`.
- Installed packages:
  - **Board**: `esp32` (3.x) with target `M5StickC Plus2`; PSRAM must be enabled.
  - **Libraries**: `M5Unified`, `M5GFX`, `LittleFS_esp32`, `AnimatedGIF`.

## Build & upload

1. Open `M5Stick-Calcifer/M5Stick-Calcifer.ino` in Arduino IDE.
2. In **Tools → Board**, pick `M5StickC Plus2` and configure:
   - Partition Scheme: `Default 8MB with spiffs` (or any layout providing ≥1.5 MB LittleFS).
   - PSRAM: `Enabled`.
3. Connect the device and upload the sketch.

On the first run the GIF is written to LittleFS; if the file already exists and the size matches, the copy step is skipped.

## Controls

- **Button A** – toggles the display. After five minutes with the display off, the device enters light sleep and wakes on the same button.
- **Button B** – enables/disables auto-rotation. When active, Calcifer realigns the image when:
  - rotating left/right (button A swaps sides);
  - flipping the device toward/away from you (the back side faces forward).

## Replacing the animation

1. Prepare a GIF (preferably 240 pixels wide).
2. Convert it to a header:
   ```bash
   xxd -i custom.gif > screensaver_gif.h
   ```
3. Ensure the array is declared as `const unsigned char ... PROGMEM` and the length as `const unsigned int`.
4. Replace the files in `M5Stick-Calcifer/` and flash the device again.

## License

Feel free to use and modify Calcifer without restrictions. If you extend it, please credit the original repository so others can trace the source.
