
BLEScanner aims to be a replacement for lepresenced in FHEM.
See https://wiki.fhem.de/wiki/PRESENCE

It is based on Espressif's ESP32s (ESP-WROOM-32), which has
Wifi and Bluetooth-LE on board.

You'll need Espressif's SDK, which you can get here:
https://github.com/espressif/arduino-esp32

The latest sketch seems too big to be uploaded.
Applying the settings from this thread (https://github.com/nkolban/esp32-snippets/issues/441#issuecomment-375795379), fixed the issue for me.

