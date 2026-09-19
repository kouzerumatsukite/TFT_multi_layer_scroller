// Force maximum O3 optimization and inline expansions for this specific function
#pragma GCC optimize ("O3")
#pragma GCC target ("inline-functions")

#include <Arduino.h>
#include <pgmspace.h>
#include <TFT_eSPI.h> // Change this to match your TFT library (e.g., Adafruit_GFX)

// Include all 4 generated layers safely without collisions
#include "frame_001_tilemap.h"
#include "frame_002_tilemap.h"
#include "frame_003_tilemap.h"
#include "frame_004_tilemap.h"

TFT_eSPI tft = TFT_eSPI(); // Invoke library

// Target screen dimensions (e.g., 240x320)
#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

#define DOUBLE_BUFFER 1
#define INTERLACE 1
#define FRAMERATE 60

#define BUFFER_SIZE (SCREEN_WIDTH + 16) * (1 + DOUBLE_BUFFER)
// Spare 16 pixels for fast tiles blitting with scrolling offset
static uint16_t lineBuffer[BUFFER_SIZE];

static bool double_buffer = DOUBLE_BUFFER;
static bool interlace = INTERLACE;
static uint32_t framerate = FRAMERATE;
static uint32_t debug_mode = 0;
// 0: visualize normal
// 1: visualize overdraw
// 2: visualize skipped pixels hierarchy
// 3: visualize unique vs clone tile render



// Layer structure wrapper to handle them dynamically in arrays
struct LayerControl {
  const struct tilemap *mapData;
  int scrollX;
  int scrollY;
  bool visible;
};

// Array holding all 4 layers with their individual independent scroll positions
LayerControl layers[4] = {
  { &level_map_data_frame_001, 0, 0, true}, // Layer 1 (Bottom)
  { &level_map_data_frame_002, 0, 0, true}, // Layer 2
  { &level_map_data_frame_003, 0, 0, true}, // Layer 3
  { &level_map_data_frame_004, 0, 0, true}, // Layer 4
};

uint8_t interleave = 0;
uint8_t buffer_flip = 0;

//  1 pixel  = 1 tile
//  2 pixels = 2 tiles
// 16 pixels = 2 tiles
// 17 pixels = 3 tiles
// 32 pixels = 3 tiles
// 33 pixels = 4 tiles
unsigned int TILES_SCREEN_WIDTH = ( SCREEN_WIDTH + 30 ) / 16;

void renderLayers() {
  // Tile state caching for fast accesses
  unsigned int row_pos_blk[4]; // - Current row tile block position
  unsigned int row_pos_off[4]; // - Current row tile pixel offset
  unsigned int col_pos_blk[4]; // - Column start tile block position
  unsigned int col_pos_off[4]; // - Column start tile pixel offset

  unsigned int last_row_pos_blk[4];

  // Tile ID caching for rendering current row
  // Aligned to DWORD size for fast heuristical comparison
  unsigned int tileChunksRowOriginal[4][(TILES_SCREEN_WIDTH+3)/4];
  // Copy of it because constantly modified during rendering
  unsigned int tileChunksRow[4][(TILES_SCREEN_WIDTH+3)/4];
  // For Dynamic Batching
  unsigned int tileBatches[TILES_SCREEN_WIDTH];

  // Iterate each layer to get their states, calculated only once
  for (unsigned int l = 0; l < 4; l++){
    const struct tilemap *map = layers[l].mapData;
    // mapWorldPos = scrollPos % mapSize
    unsigned int mapWorldY = ( layers[l].scrollY + interleave ) % (map->height * 16);
    unsigned int mapWorldX = layers[l].scrollX % (map->width * 16);
    // split tile block and pixel offset position
    row_pos_blk[l] = mapWorldY / 16;
    row_pos_off[l] = mapWorldY % 16;
    col_pos_blk[l] = mapWorldX / 16;
    col_pos_off[l] = mapWorldX % 16;
    // tileID caching must be triggered on the first row
    last_row_pos_blk[l] = row_pos_blk[l]-1;
  }

  tft.startWrite();
  uint16_t *rowDest = &lineBuffer[BUFFER_SIZE * buffer_flip];

  for (int screenY = interleave; screenY < SCREEN_HEIGHT; screenY += (1+interlace)) {
    memset(rowDest, 0, BUFFER_SIZE*2);

    // Stage 1, iterate each layer for caching tile IDs
    for (unsigned int l = 0; l < 4; l++){
      if( !layers[l].visible ){ continue; }
      uint8_t *rowTiles = (uint8_t*)(&tileChunksRowOriginal[l][0]);
      uint8_t *rowTilesCopy = (uint8_t*)(&tileChunksRow[l][0]);
      if (row_pos_blk[l] != last_row_pos_blk[l]){
        last_row_pos_blk[l] = row_pos_blk[l];
        const struct tilemap *map = layers[l].mapData;
        unsigned int tileIdx = row_pos_blk[l] * map->width;
        unsigned int tileCol = col_pos_blk[l];
        // Pre-load them for this entire row into SRAM from PROGMEM
        for (unsigned int col = 0; col < TILES_SCREEN_WIDTH; col++) {
            rowTiles[col] = pgm_read_byte(&map->tilemap[tileIdx + tileCol]);
            tileCol = ( tileCol + 1 ) % map->width;
        }
      }
      // Copy to the second cache for constant modify, preserving original
      for (unsigned int col = 0; col < TILES_SCREEN_WIDTH; col++) {
          rowTilesCopy[col] = rowTiles[col];
      }
    }

    // Stage 2, iterate each layer for heuristical rendering
    for (unsigned int l = 0; l < 4; l++){
      if( !layers[l].visible ){ continue; }
      const struct tilemap *map = layers[l].mapData;
      const uint16_t *tsColors = map->tileset->colors;
      unsigned int startBufferX = 16 - col_pos_off[l];
      uint16_t *bufferX = &rowDest[startBufferX];
      uint8_t *fullRowCache = (uint8_t*)&tileChunksRow[l][0];

      // Loop through chunks of 4-tiles-at-once
      for(unsigned int i = 0; i < (TILES_SCREEN_WIDTH+3)/4; i++){
        uint32_t*tilesChunk = (uint32_t*)&tileChunksRow[l][i];
        // check for 64 pixels (or 4 tiles) skip
        if(!*tilesChunk){ bufferX += 64; continue; }
        if(debug_mode==2){
          for(int z = 0; z < 64; z++){
            bufferX[z]  |= 
              0b0000000000010000 * (l == 1) | // Layer 1 no skip 64 pixels
              0b1000000000000000 * (l == 2) | // Layer 2 no skip 64 pixels
              0b0000001000000000 * (l == 3) ; // Layer 3 no skip 64 pixels
          }
        }
        uint8_t *rowCache = (uint8_t*)tilesChunk;
        for(unsigned int j = 0; j < 4; j++ ){
          unsigned int tileID = rowCache[j];
          // check for 16 pixels (or 1 tile) skip
          if(!tileID){ bufferX += 16; continue; }
          if(debug_mode==2){
            for(int z = 0; z < 16; z++){
              bufferX[z]  |= 
                0b0000000000100000 * (l == 1) | // Layer 1 no skip 16 pixels
                0b0000000000000001 * (l == 2) | // Layer 2 no skip 16 pixels
                0b0000010000000000 * (l == 3) ; // Layer 3 no skip 16 pixels
            }
          }
          // do batching for identical tileIDs
          unsigned int batchLen = 0;
          for(unsigned int n = i*4+j, p = 0; n < TILES_SCREEN_WIDTH; n++, p += 16){
            if(tileID == fullRowCache[n]){
              fullRowCache[n] = 0;
              tileBatches[batchLen++] = p;
            }
            //break; // put only one in the batch, for debug purposes
          }
          unsigned int tileX = (tileID % 16)*16; // dword aligned
          unsigned int tileY = (tileID / 16)*16 + row_pos_off[l];
          unsigned int tileCoord = tileY*256 + tileX;
          const uint32_t *tsDwords = (uint32_t*)&map->tileset->pixels[tileCoord];
          for(unsigned int k = 0; k < 4; k++){
            unsigned int pixelsChunk = pgm_read_dword(&tsDwords[k]);
            // check for 4 pixels skip
            if(!pixelsChunk){ bufferX += 4; continue; }
            if(debug_mode==2){
              for(int z = 0; z < 4; z++){
                bufferX[z]  |= 
                  0b0000000001000000 * (l == 1) | // Layer 0 no skip 4 pixels
                  0b0000000000000010 * (l == 2) | // Layer 1 no skip 4 pixels
                  0b0000100000000000 * (l == 3) ; // Layer 2 no skip 4 pixels
              }
            }

            uint8_t *tsPixels = (uint8_t*)&pixelsChunk;

            if(!debug_mode){
              for(unsigned int m = 0; m < 4; m++){
                unsigned int pixel = tsPixels[m];
                // check for 1 pixel skip
                if(!pixel){ bufferX++; continue; }
                unsigned int color = pgm_read_word(&tsColors[pixel]);
                for(unsigned o = 0; o < batchLen; o++){
                  bufferX[tileBatches[o]] = (color >> 8) | (color << 8);
                }
                bufferX++;
              } 
            }
            else {
              for(unsigned int m = 0; m < 4; m++){
                unsigned int pixel = tsPixels[m];
                // check for 1 pixel skip
                if(!pixel){ bufferX++; continue; }
                for(unsigned o = 0; o < batchLen; o++){
                  switch(debug_mode){
                    case 1: bufferX[tileBatches[o]] |= 
                      0b1110101101011010 * (l == 0) | // Layer 0 draw
                      0b0000000010100000 * (l == 1) | // Layer 1 draw
                      0b0000000000000101 * (l == 2) | // Layer 2 draw
                      0b0001010000000000 * (l == 3) ; // Layer 3 draw
                      break;
                    case 2: bufferX[tileBatches[o]] |=
                      0b0000000010000000 * (l == 1) | // Layer 1 no skip pixel
                      0b0000000000000100 * (l == 2) | // Layer 2 no skip pixel
                      0b0001000000000000 * (l == 3) ; // Layer 3 no skip pixel
                      break;
                    case 3: bufferX[tileBatches[o]] |= o ? 
                      0b1000001000010000 * (l == 0) | // Layer 0 unique tile color
                      0b0000000001001000 * (l == 1) | // Layer 1 unique tile color
                      0b0110000000000010 * (l == 2) | // Layer 2 unique tile color
                      0b0000100100000000 * (l == 3) : // Layer 3 unique tile color
                      0b0000010000100001 * (l == 0) | // Layer 0 clone batch color
                      0b0000000010000000 * (l == 1) | // Layer 1 clone batch color
                      0b0000000000000100 * (l == 2) | // Layer 2 clone batch color
                      0b0001000000000000 * (l == 3) ; // Layer 3 clone batch color
                      break;
                  }
                }
                bufferX++;
              }
            }
          }
        }
      }
      row_pos_off[l] += (1+interlace);
      if(row_pos_off[l] >= 16){
        row_pos_off[l] -= 16;
        row_pos_blk[l] += 1;
        if (row_pos_blk[l] >= map->height){
          row_pos_blk[l] -= map->height;
        }
      }
    }
    tft.setAddrWindow(0, screenY, SCREEN_WIDTH, 1);
    tft.pushPixels(&rowDest[16], SCREEN_WIDTH* 1);
  }
  
  tft.endWrite();
  interleave ^= interlace;
}

int64_t lastMillis;
int64_t lastMillis2;
int64_t framecounts;

void setup() {
  Serial.begin(115200);
  pinMode(0, OUTPUT); // For CPU profiling
  tft.init();

  #if defined (ESP32)
    tft.initDMA(true);
  #endif

  tft.setRotation(1); // Adjust orientation as needed (0 to 3)
  tft.fillScreen(TFT_BLACK);
  delay(1000);

  lastMillis = millis()*framerate;
}

int cameraX = 0;
int cameraY = 0;
int cameraSpeed = 64;
void loop() {
  cameraY = 64-64*sinf(fmodf((float)cameraX/6400.f,(float)PI*2.f));
  // Example: Independently scroll each layer at different speeds (Parallax effect)
  layers[0].scrollX = cameraX / 128; // Layer 1 sky
  layers[1].scrollX = cameraX / 64; // Layer 2 forests
  layers[2].scrollX = cameraX / 16; // Layer 3 foreground 1 (grounds)
  layers[3].scrollX = cameraX / 16; // Layer 4 foreground 2 (props)
  layers[0].scrollY = cameraY / 8; // Layer 1 sky
  layers[1].scrollY = cameraY / 4; // Layer 2 forests
  layers[2].scrollY = cameraY ; // Layer 3 foreground 1 (grounds)
  layers[3].scrollY = cameraY ; // Layer 4 foreground 2 (props)

  renderLayers();
  
  // Control frame rate / speed
  GPOC=1; // CPU "idle" indicator
  while ( lastMillis-millis()*framerate >= 0 ) delay(1);
  while ( lastMillis-millis()*framerate <  0 ) lastMillis += 1000;
  GPOS=1;
  cameraX += cameraSpeed;

  framecounts++;
  if (millis()-lastMillis2 > 1000){
    Serial.printf("%d fps\n",framecounts);
    lastMillis2 = millis();
    framecounts = 0;
  }

  if (Serial.available() > 0) {
    // Read the incoming string until the newline character and convert it to a long/integer
    long incomingNumber = Serial.readStringUntil('\n').toInt();
    
    // Do something with your number (e.g., change camera speed or scroll position)
    Serial.print("Received number: ");
    Serial.println(incomingNumber);

    uint32_t command = incomingNumber / 1000;
    uint32_t payload = incomingNumber % 1000;

    switch(command){
      case 0:
        debug_mode = payload < 4 ? payload : 0;
        Serial.print("debug mode is adjusted to: ");
        if (debug_mode == 0) Serial.println("visualize normal");
        if (debug_mode == 1) Serial.println("visualize overdraw");
        if (debug_mode == 2) Serial.println("visualize skipping pixels");
        if (debug_mode == 3) Serial.println("visualize unique vs clone tile render");
        break;
      case 1:
        if(payload < 4){
          layers[payload].visible = !layers[payload].visible;
          Serial.printf("layer %d is %s\n",payload,layers[payload].visible?"visible":"hidden");
        }
        break;
      case 4:
        framerate = payload;
        lastMillis = millis()*framerate;
        Serial.printf("max framerate is adjusted to: %d\n", framerate);
        break;
      case 5:
        double_buffer = payload & 1 & DOUBLE_BUFFER;
        Serial.printf("double_buffer is %s\n", double_buffer ? "enabled":"disabled");
        break;
      case 6:
        interlace = payload & 1;
        interleave = interleave & interlace;
        Serial.printf("Interlace is %s\n", interlace ? "enabled":"disabled");
        break;
      case 7:
        cameraSpeed = payload % 1000;
        Serial.printf("Camera speed is adjusted to: %d\n", cameraSpeed);
        break;
    }
  }
}