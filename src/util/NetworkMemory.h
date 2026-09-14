#pragma once

#include <Arduino.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SdCardFont.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "ReadingStatsStore.h"
#include "SdCardFontGlobals.h"
#include "util/TimeUtils.h"

/**
 * Memory management utilities for network operations.
 * 
 * Manages heap cleanup and resource release before WiFi operations.
 * The ESP32-C3 has limited memory (~380 KB usable PSRAM), so aggressive
 * cache clearing and resource release is essential for reliable network
 * operations. This namespace coordinates:
 * 
 * - Font cache and SD card font cleanup
 * - Reading statistics memory management
 * - Network time synchronization
 * - Detailed memory telemetry logging
 */
namespace NetworkMemory {

/**
 * Log current heap state for debugging memory pressure.
 * 
 * @param tag Debug tag for logging
 * @param stage Operational stage name (e.g., "pre-upload", "post-download")
 */
inline void logSnapshot(const char* tag, const char* stage) {
  const uint32_t freeHeap = esp_get_free_heap_size();
  const uint32_t contigHeap = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_DEFAULT);
  LOG_DBG(tag, "Network mem[%s]: free=%lu contig=%lu", stage, freeHeap, contigHeap);
}

/**
 * Clear renderer font and graphics caches to free memory before network ops.
 * 
 * This function:
 * - Clears the built-in font cache (glyph bitmaps currently rendered)
 * - Releases SD card font runtime caches (uncompressed data in RAM)
 * - Logs telemetry on cache sizes released
 * 
 * @param renderer Reference to the active renderer
 * @param tag Debug tag for logging
 */
inline void trimRendererCaches(const GfxRenderer& renderer, const char* tag) {
  if (auto* cacheManager = renderer.getFontCacheManager()) {
    cacheManager->clearCache();
    cacheManager->resetStats();
    LOG_DBG(tag, "Cleared font cache before network");
  }

  unsigned releasedSdFonts = 0;
  for (const auto& entry : renderer.getSdCardFonts()) {
    if (!entry.second) {
      continue;
    }
    entry.second->releaseForLowMemory();
    releasedSdFonts++;
  }
  if (releasedSdFonts > 0) {
    LOG_DBG(tag, "Released %u SD font runtime cache(s) before network", releasedSdFonts);
  }
}

/**
 * Comprehensive memory cleanup before network operations.
 * 
 * Prepares the system for WiFi/HTTP by:
 * 1. Stopping NTP time synchronization
 * 2. Clearing all renderer caches
 * 3. Releasing SD card font memory
 * 4. Optionally releasing reading statistics memory
 * 
 * @param renderer Reference to the active renderer (modified)
 * @param tag Debug tag for logging
 * @param stage Operational stage name for telemetry
 * @param releaseReadingStats If true, release reading stats memory (default: true)
 */
inline void prepareBeforeNetwork(GfxRenderer& renderer, const char* tag, const char* stage,
                                 const bool releaseReadingStats = true) {
  TimeUtils::stopNtp();
  trimRendererCaches(renderer, tag);

  if (Storage.ready()) {
    sdFontSystem.releaseForNetwork(renderer);
  }

  if (releaseReadingStats && !READING_STATS.releaseMemoryForNetwork()) {
    LOG_ERR(tag, "Failed to release reading stats memory before network");
  }

  delay(20);
  logSnapshot(tag, stage);
}

/**
 * Restore renderer and system state after network operations complete.
 * 
 * Reverses memory cleanup performed by prepareBeforeNetwork():
 * 1. Optionally reload reading statistics from persistent storage
 * 2. Reinitialize SD card font system
 * 3. Verify system heap recovery
 * 
 * @param renderer Reference to the active renderer (modified)
 * @param tag Debug tag for logging
 * @param stage Operational stage name for telemetry
 * @param reloadReadingStats If true, reload stats from storage (default: true)
 */
inline void restoreAfterNetwork(GfxRenderer& renderer, const char* tag, const char* stage,
                                const bool reloadReadingStats = true) {
  if (reloadReadingStats && !READING_STATS.reloadAfterNetwork()) {
    LOG_ERR(tag, "Failed to reload reading stats after network");
  }

  if (Storage.ready()) {
    sdFontSystem.ensureLoaded(renderer);
  }

  delay(10);
  logSnapshot(tag, stage);
}

}  // namespace NetworkMemory
