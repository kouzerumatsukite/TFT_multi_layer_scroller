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
static bool enable_batching = true;
static bool enable_skipping = true;
static bool enable_occlusion = true;
static bool debug_enable = false;
static uint32_t debug_mode = 0;
// 1: visualize normal
// 2: visualize overdraw
// 3: visualize skipped pixels hierarchy
// 4: visualize unique vs clone tile render
// 5: visualize depthmap
// 6: grayscale mode
static uint32_t framerate = FRAMERATE;



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

    // Stage 2, iterate each layer for heurestical rendering
    for (int l = 3; l >= 0; l--){
      if( !layers[l].visible ){ continue; }
      const struct tilemap *map = layers[l].mapData;
      const uint16_t *tsColors = map->tileset->colors;
      unsigned int startBufferX = 16 - col_pos_off[l];
      uint16_t *bufferX = &rowDest[startBufferX];
      uint8_t *fullRowCache = (uint8_t*)&tileChunksRow[l][0];

      // Hold opaque pixel bits of currently rendered tile
      // and store it into the array for whole screen tiles row
      unsigned int opaqueTilesInRow[TILES_SCREEN_WIDTH]= { 0 };

      // Loop through 4 chunks of 4-tiles-at-once
      for(unsigned int i = 0; i < (TILES_SCREEN_WIDTH+3)/4; i++){
        uint32_t*tilesChunk = (uint32_t*)&tileChunksRow[l][i];

        // check for 64 pixels (or 4 tiles) skip
        if(!*tilesChunk){ bufferX += 64; continue; }
        uint8_t *rowCache = (uint8_t*)tilesChunk;

        // We are inside a chunk of 4-tiles-at-once
        // Let's loop through 4 tiles
        for(unsigned int j = 0; j < 4; j++ ){
          unsigned int tileID = rowCache[j];
          unsigned int opaquePixelsInTile = 0;

          // check for 16 pixels (or 1 tile) skip
          if(!tileID){ bufferX += 16; continue; }
          // do batching for identical tileIDs
          unsigned int batchLen = 0;
          for(unsigned int n = i*4+j, p = 0; n < TILES_SCREEN_WIDTH; n++, p += 16){
            if(tileID == fullRowCache[n]){
              tileBatches[batchLen++] = p;
              fullRowCache[n] = 0;
            }
          }
          unsigned int tileX = (tileID % 16)*16; // dword aligned
          unsigned int tileY = (tileID / 16)*16 + row_pos_off[l];
          unsigned int tileCoord = tileY*256 + tileX;
          const uint32_t *tsDwords = (uint32_t*)&map->tileset->pixels[tileCoord];

          // We are inside a tile, and it has 16 pixels width,
          // Lets loop through 4 chunks of 4-pixels-at-once
          for(unsigned int k = 0; k < 4; k++){
            unsigned int pixelsChunk = pgm_read_dword(&tsDwords[k]);
            // check for 4 pixels skip
            if(!pixelsChunk){ bufferX += 4; continue; }
            uint8_t *tsPixels = (uint8_t*)&pixelsChunk;
            for(unsigned int m = 0; m < 4; m++){
              unsigned int pixel = tsPixels[m];
              opaquePixelsInTile <<= 1;
              // check for 1 pixel skip
              if(!pixel){ bufferX++; continue; }
              opaquePixelsInTile |= 1;
              unsigned int color = pgm_read_word(&tsColors[pixel]);
              for(unsigned o = 0; o < batchLen; o++){
                if(!bufferX[tileBatches[o]])
                  bufferX[tileBatches[o]] = (color >> 8) | (color << 8);
              }
              bufferX++;
            }
          }
          // 
          for(unsigned k = 0; k < batchLen; k++){
            opaqueTilesInRow[i*4+j+(tileBatches[k]>>4)] = opaquePixelsInTile;
          }
        }
      }

      // if occlusion enabled, prune the below layers
      for(unsigned int i = 1; i < TILES_SCREEN_WIDTH; i++){
        if( opaqueTilesInRow[i] == 0b1111111111111111 ){
          if( opaqueTilesInRow[i-1] == 0b1111111111111111 ){
            for ( unsigned int z = 0; z < l ; z++){
              int o = (int)col_pos_off[z]-(int)col_pos_off[l];
              ((uint8_t*)&tileChunksRow[z][0])[i-(o<0)] = 0;
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

void renderLayers_debug() {
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

    // Stage 2, iterate each layer for heurestical rendering
    for (unsigned int L = 0; L < 4; L++){
      unsigned int l = enable_occlusion ? 3-L : L;
      if( !layers[l].visible ){ continue; }
      const struct tilemap *map = layers[l].mapData;
      const uint16_t *tsColors = map->tileset->colors;
      unsigned int startBufferX = 16 - col_pos_off[l];
      uint16_t *bufferX = &rowDest[startBufferX];
      uint8_t *fullRowCache = (uint8_t*)&tileChunksRow[l][0];

      // Hold opaque pixel bits of currently rendered tile
      // and store it into the array for whole screen tiles row
      unsigned int opaqueTilesInRow[TILES_SCREEN_WIDTH]= { 0 };

      // Loop through 4 chunks of 4-tiles-at-once
      for(unsigned int i = 0; i < (TILES_SCREEN_WIDTH+3)/4; i++){
        uint32_t*tilesChunk = (uint32_t*)&tileChunksRow[l][i];

        // check for 64 pixels (or 4 tiles) skip
        if(!*tilesChunk){ if(enable_skipping) { bufferX += 64; continue; } }
        if(debug_mode==2){
          for(int z = 0; z < 64; z++){
            bufferX[z]  |= 
              0b0000000000010000 * (l == 1) | // Layer 1 no skip 64 pixels
              0b1000000000000000 * (l == 2) | // Layer 2 no skip 64 pixels
              0b0000001000000000 * (l == 3) ; // Layer 3 no skip 64 pixels
          }
        }
        uint8_t *rowCache = (uint8_t*)tilesChunk;

        // We are inside a chunk of 4-tiles-at-once
        // Let's loop through 4 tiles
        for(unsigned int j = 0; j < 4; j++ ){
          unsigned int tileID = rowCache[j];
          unsigned int opaquePixelsInTile = 0;

          // check for 16 pixels (or 1 tile) skip
          if(!tileID){ if(enable_skipping) { bufferX += 16; continue; } }
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
              tileBatches[batchLen++] = p;
              fullRowCache[n] = 0;
            }
            if (!enable_batching) break; // put only one in the batch, for debug purposes
          }
          unsigned int tileX = (tileID % 16)*16; // dword aligned
          unsigned int tileY = (tileID / 16)*16 + row_pos_off[l];
          unsigned int tileCoord = tileY*256 + tileX;
          const uint32_t *tsDwords = (uint32_t*)&map->tileset->pixels[tileCoord];

          // We are inside a tile, and it has 16 pixels width,
          // Lets loop through 4 chunks of 4-pixels-at-once
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
              if(!enable_occlusion){
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
              else{
                for(unsigned int m = 0; m < 4; m++){
                  unsigned int pixel = tsPixels[m];
                  opaquePixelsInTile <<= 1;
                  // check for 1 pixel skip
                  if(!pixel){ bufferX++; continue; }
                  opaquePixelsInTile |= 1;

                  unsigned int color = pgm_read_word(&tsColors[pixel]);
                  for(unsigned o = 0; o < batchLen; o++){
                    if(!bufferX[tileBatches[o]]){
                      bufferX[tileBatches[o]] = (color >> 8) | (color << 8);
                    }
                  }
                  bufferX++;
                }
              }
            }
            else {
              for(unsigned int m = 0; m < 4; m++){
                unsigned int pixel = tsPixels[m];
                opaquePixelsInTile <<= 1;
                // check for 1 pixel skip
                if(!pixel){ bufferX++; continue; }
                opaquePixelsInTile |= 1;

                for(unsigned o = 0; o < batchLen; o++){
                  switch(debug_mode){
                    case 1: ///////////////////////////// VISUALIZE OVERDRAW
                      bufferX[tileBatches[o]] |= 0  |  // ------------------
                      0b1110101101011010 * (l == 0) |  // Layer 0   overdraw
                      0b0000000010100000 * (l == 1) |  // Layer 1   overdraw
                      0b0000000000000101 * (l == 2) |  // Layer 2   overdraw
                      0b0001010000000000 * (l == 3) ;  // Layer 3   overdraw
                      break;
                    case 2: ///////////////////////////// VISUALIZE  SKIP PIXEL
                      bufferX[tileBatches[o]] |= 0  |  // ---------------------
                      0b0000000010000000 * (l == 1) |  // Layer 1 no skip pixel
                      0b0000000000000100 * (l == 2) |  // Layer 2 no skip pixel
                      0b0001000000000000 * (l == 3) ;  // Layer 3 no skip pixel
                      break;
                    case 3: ///////////////////////////// VISUALIZE UNIQUE VS CLONE
                      bufferX[tileBatches[o]] |= o  ?  // -------------------------
                      0b1000001000010000 * (l == 0) |  // Layer 0 unique tile color
                      0b0000000001001000 * (l == 1) |  // Layer 1 unique tile color
                      0b0110000000000010 * (l == 2) |  // Layer 2 unique tile color
                      0b0000100100000000 * (l == 3) :  // Layer 3 unique tile color
                      0b0000010000100001 * (l == 0) |  // Layer 0 clone batch color
                      0b0000000010000000 * (l == 1) |  // Layer 1 clone batch color
                      0b0000000000000100 * (l == 2) |  // Layer 2 clone batch color
                      0b0001000000000000 * (l == 3) ;  // Layer 3 clone batch color
                      break;
                    case 4: ///////////////////////////// VISUALIZE DEPTH
                      if(!bufferX[tileBatches[o]]){
                      bufferX[tileBatches[o]]    = 0  |  // ---------------
                        0b0100010100101001 * (l == 0) |  // Layer 0 depth 1
                        0b1010101001010010 * (l == 1) |  // Layer 1 depth 2
                        0b1110111101111011 * (l == 2) |  // Layer 2 depth 3
                        0b0101010110101101 * (l == 3) ;  // Layer 3 depth 4
                      }
                      break;
                    case 5: ///////////////////////////// GRAYSCALE MODE
                      if(!bufferX[tileBatches[o]]){
                        unsigned int color = pgm_read_word(&tsColors[pixel]);
                        unsigned int r_col = color >> 11           ; // 25.0% red
                        unsigned int g_col = color >>  5 & 0b111111; // 50.0% green
                        unsigned int b_col = color >>  1 & 0b001111; // 12.5% blue
                        unsigned int gray  = r_col + g_col + b_col ; // sum all
                        gray  = gray * 0b1001011;
                        r_col = gray >> 8 << 11 ;
                        g_col = gray >> 7 <<  5 ;
                        b_col = gray >> 8       ;
                        color = r_col | g_col | b_col ;
                        bufferX[tileBatches[o]] = color >> 8 | color << 8 ;
                      }
                      break;
                  }
                }
                bufferX++;
              }
            }
          }
          // 
          for(unsigned k = 0; k < batchLen; k++){
            opaqueTilesInRow[i*4+j+(tileBatches[k]>>4)] = opaquePixelsInTile;
          }
        }
      }

      // if occlusion enabled, prune the below layers
      if(enable_occlusion){
        for(unsigned int i = 1; i < TILES_SCREEN_WIDTH; i++){
          if( opaqueTilesInRow[i] == 0b1111111111111111 ){
            if( opaqueTilesInRow[i-1] == 0b1111111111111111 ){
              for ( unsigned int z = 0; z < l ; z++){
                int o = (int)col_pos_off[z]-(int)col_pos_off[l];
                ((uint8_t*)&tileChunksRow[z][0])[i-(o<0)] = 0;
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
int64_t lastMillis3;
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
int scrollingX = 0;
int scrollingY = 0;
int cameraSpeed = 64;
bool cameraAutoScroll = true;
void loop() {
  uint64_t millisDelta = millis() - lastMillis3;
  lastMillis3 += millisDelta;
  scrollingX += (cameraSpeed*millisDelta) / 25;
  scrollingY = 1024-1024*sinf(fmodf((float)cameraX/6400.f,(float)PI*2.f));

  if (cameraAutoScroll){
    cameraX += ( scrollingX - cameraX ) / 5;
    cameraY += ( scrollingY - cameraY ) / 10;
  }
  // Example: Independently scroll each layer at different speeds (Parallax effect)
  layers[0].scrollX = cameraX / 128; // Layer 1 sky
  layers[0].scrollY = cameraY / 128; // Layer 1 sky
  layers[1].scrollX = cameraX /  64; // Layer 2 forests
  layers[1].scrollY = cameraY /  64; // Layer 2 forests
  layers[2].scrollX = cameraX /  16; // Layer 3 foreground 1 (grounds)
  layers[2].scrollY = cameraY /  16; // Layer 3 foreground 1 (grounds)
  layers[3].scrollX = cameraX /  16; // Layer 4 foreground 2 (props)
  layers[3].scrollY = cameraY /  16; // Layer 4 foreground 2 (props)

  if(!debug_enable)
    renderLayers();
  else
    renderLayers_debug();

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
      case 0: // debug mode selector
        if (payload > 0){
          debug_enable = true;
          debug_mode = ( payload < 7 ? payload : 1 ) - 1;
          Serial.print("debug mode is adjusted to: ");
          if (debug_mode == 0) Serial.println("visualize normal");
          if (debug_mode == 1) Serial.println("visualize overdraw");
          if (debug_mode == 2) Serial.println("visualize skipping pixels");
          if (debug_mode == 3) Serial.println("visualize unique vs clone tile render");
          if (debug_mode == 4) Serial.println("visualize depthmap");
          if (debug_mode == 5) Serial.println("gray scale mode");
        }else{
          debug_enable = false;
          Serial.println("debug is disabled");
        }
        break;
      case 1: // layer options
        command = payload / 100;
        payload = payload % 100;
        if(command<3){
          switch(command){
            case 0 : layers[payload].visible = !layers[payload].visible; break; // toggle
            case 1 : layers[payload].visible = false; break; // set layer hidden
            case 2 : layers[payload].visible = true; break; // set layer visible
          }
          Serial.printf("layer %d is %s\n",payload,layers[payload].visible?"visible":"hidden");
        }else
        {
          if(payload < 4){
            layers[0].visible = true;
            layers[1].visible = true;
            layers[2].visible = true;
            layers[3].visible = true;
            Serial.println("all layers is set to visible");
          }
        }
        break;
      case 2: { // tile engine rendering optimizations
        command = payload / 100;
        payload = payload % 100;

        bool bool_options[3] = { enable_skipping, enable_batching, enable_occlusion };
        char*name_options[3] = { "skipping", "batching", "occlussion" };

        if(command<3){
          switch(command){
            case 0 : bool_options[payload] = !bool_options[payload]; //toggle
            case 1 : bool_options[payload] = false; // disable option
            case 2 : bool_options[payload] = true; // enable option
          }
          enable_skipping  = bool_options[0];
          enable_batching  = bool_options[1];
          enable_occlusion = bool_options[2];
          Serial.printf("enable_%s is %s\n", name_options[payload], bool_options[payload] ? "enabled":"disabled");
        }else
        {
          if(payload < 3){
            enable_skipping  = true;
            enable_batching  = true;
            enable_occlusion = true;
            Serial.println("tile skipping, batching, and occlusion is set to all enabled");
          }
        }
        break;
      }
      case 3: { // screen rendering options
        command = payload / 100;
        payload = payload % 100;

        bool bool_options[3] = { interlace, double_buffer };
        char*name_options[3] = { "interlace", "double_buffer" };

        if(command<3){
          switch(command){
            case 0 : bool_options[payload] = !bool_options[payload]; //toggle
            case 1 : bool_options[payload] = false; // disable option
            case 2 : bool_options[payload] = true; // enable option
          }
          Serial.printf("%s is %s\n", name_options[payload], bool_options[payload] ? "enabled":"disabled");
        }else
        {
          if(payload < 2){
            interlace = true;
            double_buffer = true;
            Serial.println("interlace and double_buffer is set to all enabled");
          }
        }
        break;
      }
      case 4: // framerate
        framerate = std::max(payload,(unsigned int)1);
        lastMillis = millis()*framerate;
        Serial.printf("max framerate is adjusted to: %d\n", framerate);
        break;
      case 5: // camera speed
        cameraSpeed = payload % 1000;
        Serial.printf("Camera speed is adjusted to: %d\n", cameraSpeed);
        break;
      case 6: // camera position X
        cameraX = ( payload % 1000 ) * 16;
        Serial.printf("Camera X is adjusted to: %d\n", cameraX);
        break;
      case 7: // camera position Y
        cameraY = ( payload % 1000 ) * 16;
        Serial.printf("Camera Y is adjusted to: %d\n", cameraY);
        break;
      case 8: // key events ( use python's pyserial or similar to send these commands using keyboard )
        command = payload / 100;
        payload = payload % 100;
        switch(command){
          // change camera positions
          case 0: cameraX -= payload * 16; break;
          case 1: cameraX += payload * 16; break;
          case 2: cameraY -= payload * 16; break;
          case 3: cameraY += payload * 16; break;
          case 4: cameraAutoScroll ^= payload & 1; break;
        }
        break;
    }
  }
  
  // Control frame rate / speed
  GPOC=1; // CPU "idle" indicator
  while ( lastMillis-millis()*framerate >= 0 ) delay(1);
  while ( lastMillis-millis()*framerate <  0 ) lastMillis += 1000;
  GPOS=1;
}