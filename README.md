# xiaomiao-desktop

Metro UI desktop system for xiaomiao handheld (ESP32 + LVGL).

## Files

- `main/main.c` - desktop entry
- `main/tiles.c` - metro tile renderer
- `main/dock.c` - bottom dock bar
- `main/return_to_loader.c/h` - loader integration
- `main/CMakeLists.txt` - main component
- `main/idf_component.yml` - lvgl dependency
- `sdkconfig.defaults` - default sdkconfig
- `partitions.csv` - partition table

## Build

Use ESP-IDF `idf.py build` then `idf.py merge-bin` to produce the desktop merged binary.
