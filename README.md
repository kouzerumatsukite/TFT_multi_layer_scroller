# Multi-Layer Parallax Tile Renderer

A high-performance, tile-based 2D rendering engine for ESP32 and ESP8266 TFT projects. The renderer is optimized around a single-core pipeline that keeps the hot path compact, uses row caching from flash/PROGMEM, and aggressively avoids wasted work.

This project focuses on a fast tile renderer for layered parallax scenes and is tuned for low-memory microcontrollers. It is designed around a direct-write fast path, batched tile rendering, transparent-pixel skipping, and per-row occlusion pruning.

https://github.com/user-attachments/assets/c29ea7c5-c16b-4343-80fd-1d8297859b68

## What this project does

- Renders a 4-layer scrolling parallax scene
- Keeps each layer independently scrollable in X/Y
- Uses dynamic tile batching to reduce redundant work
- Skips transparent or zero-value chunks early
- Uses a fast direct-write path for single opaque tiles
- Prunes lower layers when upper layers fully cover the row
- Supports interlaced display updates and optional double buffering
- Includes runtime debug overlays for optimization analysis

## Core optimizations

### Fast path / direct write

The rendering engine detects cases where a tile is unique, opaque, and fully safe to write directly without checking whether a pixel is already filled. That avoids the cost of the slower read-check-write path.

### Dynamic batching

Adjacent identical tiles are grouped and rendered together. This reduces repeated fetch and color conversion work in dense regions.

### Transparent pixel skipping

The code evaluates 64-pixel (4-tile), 16-pixel (1-tile), and 4-pixel tile chunks. Empty chunks are skipped immediately, saving a lot of wasted CPU time.

### Occlusion pruning

When upper layers are fully opaque, lower layers are pruned for that region to avoid drawing behind already-occupied pixels. This reduces redundant overdraw and helps the engine stay fast.

### PROGMEM-friendly data flow

Tile indices and tile data are cached from flash/PROGMEM into row-local SRAM buffers before rendering, keeping the hot path small and the memory footprint predictable.

### DMA-friendly TFT output

The renderer fills a line buffer and pushes it to the TFT using TFT_eSPI with DMA when available on ESP32.

## Runtime debug suite

The engine includes a built-in visual profiler and rendering debugger, controllable over Serial at 115200 baud.

### Debug modes

Send a single integer value over Serial:

- 0: Disable debug mode and render normally
- 1: Overdraw heatmap
- 2: Skip visualization
- 3: Unique vs clone tile render
- 4: Fast-path vs slow-path visualization
- 5: Depth visualization
- 6: Grayscale mode

### Runtime optimization toggles

The engine exposes runtime controls for:

- layer visibility
- skipping
- batching
- occlusion
- interlace
- double buffering
- target framerate
- camera speed and position

## Project layout

- `multi_layer_scroller.ino` – main render loop and engine logic
- `frame_001_tilemap.h` through `frame_004_tilemap.h` – generated tilemaps for each parallax layer

## Firmware requirements

- ESP32 or ESP8266
- SPI TFT display (ILI9341 or similar)
- Arduino IDE
- `TFT_eSPI` library configured for your display and board

## Serial command interface

The engine listens for integer commands over Serial. Commands use the format `C P P P`, where `C` is the command group and `PPP` is the payload.

Examples:

- `0000` – disable debug mode
- `0001` – enable overdraw visualization
- `1201` – show layer 1
- `2201` – enable batching
- `3101` – enable interlace
- `4060` – set framerate to 60 FPS
- `8101` – nudge camera right

### Command reference

- `0XXX` – debug visualizers
- `1X0Y` – layer visibility
  - X: 0 = toggle, 1 = hide, 2 = show
  - Y: target layer 0–3
- `2X0Y` – engine optimization toggles
  - X: 0 = toggle, 1 = enable, 2 = disable
  - Y: skipping / batching / occlusion
- `3X0Y` – display settings
  - X: 0 = toggle, 1 = enable, 2 = disable
  - Y: interlace / double buffer
- `4XXX` – target framerate
- `5XXX` – camera speed
- `6XXX` – camera X position
- `7XXX` – camera Y position
- `8XXX` – manual camera adjustment / auto-scroll toggle

## Typical performance

This project is tuned for the ESP32 in particular, where the TFT DMA path and better CPU throughput make the renderer comfortably fast. On ESP8266, the same pipeline still runs reasonably well with lower graphics cost, especially when interlace and skipping are enabled.

Expected performance depends on:

- screen resolution
- layer count
- tile density / fill rate
- toggles like interlace, batching, skipping, and occlusion
- whether debug visualizations are active

## Notes

This repo is intentionally focused on the render pipeline itself. The goal is to maximize performance per pixel and keep the hot path as small and predictable as possible before adding more game systems or sprite logic.

## Asset attribution

Sunny Land - Pixel Game Art Assets Pack by ansimuz
https://ansimuz.itch.io/sunny-land-pixel-game-art

## Author

Kouzerumatsukite / Kouzeru / Bananafox / Bitwisefox
