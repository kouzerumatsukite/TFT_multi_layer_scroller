# ESP8266 Multi-Layer Parallax Engine

A high-performance, tile-based 2D rendering engine built for ESP8266, and can also be compiled for ESP32 or RP2040. It achieves a stable 25–35 FPS (ESP8266 @160MHz) while pushing four simultaneous layers of parallax tiles across a TFT display.

https://github.com/user-attachments/assets/c29ea7c5-c16b-4343-80fd-1d8297859b68

The engine is heavily optimized for low-resource microcontrollers, utilizing aggressive spatial caching, bit-shifting math, and run-time occlusion culling to minimize SPI bandwidth and CPU overhead.

## Core Features

* **4-Layer Parallax Scrolling:** Independent X/Y camera offsets for background, midground, and dual foreground layers.
* **Dynamic Occlusion Culling:** Builds a 16-bit vertical opacity map on the fly. When foreground layers completely obscure background tiles, the background rendering is pruned at the row-cache level.
* **Tile Skipping & Dynamic Batching:**
  * Evaluates 64-pixel (4-tile) and 16-pixel (1-tile) chunks, instantly skipping transparent regions.
  * Batches identical background tiles across the same row to push contiguous color data to the display in single bursts.
* **Fixed-Point Math Optimizations:** Grayscale visual modes use pure bit-shifting and integer multiplication (~25% R, 50% G, 12.5% B) to bypass the ESP8266's slow floating-point unit.
* **Interlacing & Double Buffering:** Splits rendering workloads across alternating scanlines to maintain high framerates and prevent screen tearing.

## Debug Visualization Suite

The engine includes a built-in hardware profiler and visualizer controlled via Serial input, allowing real-time analysis of the rendering pipeline.

### Runtime Debug Selection

Send a single integer value over Serial at 115200 baud:

* **0**: Disable debug mode and return to the normal production renderer
* **1**: Standard diagnostic rendering
* **2**: Overdraw Heatmap (visualizes wasted pixels drawn behind other layers)
* **3**: Skipped Pixels (highlights chunks effectively bypassed by the engine)
* **4**: Unique vs. Clone Tile Render (highlights dynamically batched tiles)
* **5**: Z-Depth Map
* **6**: Hardware-Accelerated Grayscale

### Important Note About Runtime Optimization Controls

The engine has two render paths:

* **`renderLayers()`**: the production renderer used for normal operation. It is the fixed high-performance path.
* **`renderLayers_debug()`**: the diagnostic renderer used when debug mode is enabled. This path supports runtime experimentation and visualization.

The optimization controls (`skipping`, `batching`, and `occlusion`) are intended for comparison and profiling inside the debug renderer. They are not meant to dynamically reconfigure the production render path at runtime.

## Requirements

* **Hardware:** ESP8266 (e.g., Wemos D1 Mini / NodeMCU) and an SPI TFT Display (ILI9341 or similar).
* **Software:** Arduino IDE, `TFT_eSPI` library.
* **Compiler:** Uses `#pragma GCC optimize ("O3")` for maximum inline expansion.

## Serial Command Interface

The engine listens for 4-digit integer commands over the 115200 baud Serial monitor. The format is `C P P P`, where `C` is the command group (0-8) and `PPP` is the payload.

### Examples:

* `0001` - Switch to the diagnostic render mode for overdraw visualization.
* `0000` - Disable debug mode and return to normal rendering.
* `1201` - Set Layer 1 to visible.
* `2201` - Enable dynamic tile batching.
* `3101` - Enable interlaced rendering.
* `4060` - Set target framerate to 60 FPS.
* `8101` - Move camera right by 1 step.

### Command Reference:

* **000Y**: Debug Visualizers (0 = Disable debug mode, 1 = Diagnostic, 2 = Overdraw, 3 = Skip, 4 = Batching, 5 = Depth, 6 = Grayscale).
* **1X0Y**: Layer Visibility (X is 0=Toggle, 1=Hide, 2=Show. Y is target Layer 0-3).
* **2X0Y**: Engine Optimizations (X is 0=Toggle, 1=Enable, 2=Disable. Y is 0=Reset, 1=Skipping, 2=Batching, 3=Occlusion).
* **3X0Y**: Display Configuration (X is 0=Toggle, 1=Enable, 2=Disable. Y is 0=Reset, 1=Interlace, 2=Double Buffer).
* **4XXX**: Target Framerate (e.g., `4030` for 30 FPS).
* **5XXX**: Camera Auto-Scroll Speed.
* **6XXX / 7XXX**: Absolute Camera X / Y Position.
* **8XXX**: Manual Camera Nudge (0=Left, 1=Right, 2=Up, 3=Down, 4=Toggle Auto-Scroll).

## Layer Visibility Options (1XXX)

| Command | Action | Target | Description |
| --- | --- | --- | --- |
| **1000** | Toggle | Layer 0 | Reverses visibility state of Layer 1 (Sky) |
| **1001** | Toggle | Layer 1 | Reverses visibility state of Layer 2 (Forests) |
| **1002** | Toggle | Layer 2 | Reverses visibility state of Layer 3 (Grounds) |
| **1003** | Toggle | Layer 3 | Reverses visibility state of Layer 4 (Props) |
| **1100** | Disable | Layer 0 | Forces Layer 1 to be hidden |
| **1101** | Disable | Layer 1 | Forces Layer 2 to be hidden |
| **1102** | Disable | Layer 2 | Forces Layer 3 to be hidden |
| **1103** | Disable | Layer 3 | Forces Layer 4 to be hidden |
| **1200** | Enable | Layer 0 | Forces Layer 1 to be visible |
| **1201** | Enable | Layer 1 | Forces Layer 2 to be visible |
| **1202** | Enable | Layer 2 | Forces Layer 3 to be visible |
| **1203** | Enable | Layer 3 | Forces Layer 4 to be visible |
| **1300** | Enable All | All Layers | Forces all 4 layers to be visible simultaneously |

## Tile Engine Optimizations (2XXX)

| Command | Action | Target | Description |
| --- | --- | --- | --- |
| **2000** | Toggle | Skipping | Reverses the state of 64px/16px transparent pixel skipping |
| **2001** | Toggle | Batching | Reverses the state of dynamic tile batching |
| **2002** | Toggle | Occlusion | Reverses the state of row occlusion culling |
| **2100** | Disable | Skipping | Turns off pixel and chunk skipping |
| **2101** | Disable | Batching | Turns off dynamic tile batching |
| **2102** | Disable | Occlusion | Turns off row occlusion culling |
| **2200** | Enable | Skipping | Turns on pixel and chunk skipping |
| **2201** | Enable | Batching | Turns on dynamic tile batching |
| **2202** | Enable | Occlusion | Turns on row occlusion culling |
| **2300** | Enable All | All Ops | Enables skipping, batching, and occlusion simultaneously |

## Screen Rendering Options (3XXX)

| Command | Action | Target | Description |
| --- | --- | --- | --- |
| **3000** | Toggle | Interlace | Reverses the state of interlaced rendering |
| **3001** | Toggle | Double Buffer | Reverses the state of the DMA double buffer |
| **3100** | Disable | Interlace | Turns off interlaced rendering |
| **3101** | Disable | Double Buffer | Turns off the DMA double buffer |
| **3200** | Enable | Interlace | Turns on interlaced rendering |
| **3201** | Enable | Double Buffer | Turns on the DMA double buffer |
| **3300** | Enable All | All Display | Enables both interlace and double buffering simultaneously |

## Setup & Installation

1. Install the `TFT_eSPI` library in the Arduino IDE and configure your `User_Setup.h` file to match your specific display driver and ESP8266 pinout.
2. Ensure the generated tilemap header files (`frame_001_tilemap.h`, etc.) are placed in the same directory as the `.ino` file.
3. Compile and flash to the ESP8266.


# TFT_multi_layer_scroller

Multi Layer Scrolling Parallax of Sunny Land

## Author
Kouzerumatsukite / Kouzeru / Bananafox / Bitwisefox 

## Assets used in this project:

Sunny Land - Pixel Game Art Assets Pack by ansimuz
https://ansimuz.itch.io/sunny-land-pixel-game-art
