#pragma once

#include <SdCardFontManager.h>
#include <SdCardFontRegistry.h>

#include <atomic>

class GfxRenderer;

/**
 * High-level facade for SD card font loading and management.
 * 
 * Abstracts the complexity of font discovery, loading, persistence, and lifecycle
 * management. Coordinates between:
 * - SdCardFontRegistry: Scans and catalogs available fonts on SD card
 * - SdCardFontManager: Loads and caches font data in RAM
 * - GfxRenderer: Rendering engine that consumes loaded fonts
 * 
 * Workflow:
 * 1. begin() - Initialize and register the resolver (called at startup)
 * 2. ensureLoaded() - Load configured font before reading (called before HomeActivity)
 * 3. releaseForNetwork() - Free font memory before WiFi ops (called by NetworkMemory)
 * 
 * Memory Management:
 * - SD fonts consume RAM (typical 50-200 KB per font)
 * - Must be released before network operations to ensure WiFi connectivity
 * - Automatically reloaded when returning to normal operation
 * 
 * Thread Safety:
 * - Registry dirty flag is atomic (safe for web server thread to signal)
 * - Main reader thread checks and processes dirty flag in ensureLoaded()
 */
class SdCardFontSystem {
 public:
  /**
   * Default constructor (no special initialization needed).
   */
  SdCardFontSystem() = default;

  /**
   * Prevent copying (only one font system per device).
   */
  SdCardFontSystem(const SdCardFontSystem&) = delete;
  SdCardFontSystem& operator=(const SdCardFontSystem&) = delete;

  /**
   * Initialize the SD card font system at startup.
   * 
   * - Registers the font ID resolver with CrossPointSettings
   * - Discovers available fonts on the SD card
   * - Does not load fonts yet; that happens in ensureLoaded()
   * 
   * @param renderer Renderer instance (needed for font setup)
   */
  void begin(GfxRenderer& renderer);

  /**
   * Ensure the user-selected SD font is loaded and ready for reading.
   * 
   * - Checks if configured font matches currently loaded font
   * - Re-discovers fonts if registry was marked dirty (e.g., by web upload)
   * - Sets up UI fallback fonts if discovery fails
   * 
   * Call before entering the reader activity or after user changes font settings.
   * 
   * @param renderer Renderer to receive font updates
   */
  void ensureLoaded(GfxRenderer& renderer);

  /**
   * Release SD font memory before network operations.
   * 
   * Frees font data from RAM to ensure reliable WiFi/HTTP connectivity.
   * The renderer will also clear its font cache. Memory is restored when
   * ensureLoaded() is called again after network operations complete.
   * 
   * Used by NetworkMemory utility before WiFi uploads/downloads.
   * 
   * @param renderer Renderer to notify of font release
   * @return true if release succeeded, false on error
   */
  bool releaseForNetwork(GfxRenderer& renderer);

  /**
   * Look up an SD card font by family name and size.
   * 
   * Used by CrossPointSettings to validate font selections and provide
   * the renderer with the correct font ID at load time.
   * 
   * @param familyName Font family (e.g., "Noto Serif")
   * @param fontSizeEnum Font size selector from CrossPointSettings
   * @return Font ID (> 0) if found, 0 if not found
   */
  int resolveFontId(const char* familyName, uint8_t fontSizeEnum) const;

  /**
   * Get read-only access to the font registry.
   * Used by settings UI to show available fonts and by settings serialization.
   */
  const SdCardFontRegistry& registry() const { return registry_; }

  /**
   * Get writable access to the font registry.
   * Used by FontInstaller to add or modify font entries during installation.
   */
  SdCardFontRegistry& registry() { return registry_; }

  /**
   * Signal that the registry needs to be re-scanned.
   * 
   * Thread-safe: can be called from the web server task when fonts
   * are uploaded or deleted via the browser UI.
   * 
   * The next ensureLoaded() call will refresh the registry.
   */
  void markRegistryDirty() { registryDirty_.store(true, std::memory_order_release); }

  /**
   * Manually trigger a registry refresh if it's been marked dirty.
   * 
   * Allows the web UI to show updated font lists without waiting for
   * the reader to load. Called by web endpoints after font installation.
   */
  void refreshIfDirty();

 private:
  /**
   * Internal: Re-scan registry if dirty flag is set.
   * 
   * @param wasDirty Output: true if registry was dirty before refresh
   * @param wasReleased Output: true if fonts were in released state
   * @return true if refresh succeeded or was not needed
   */
  bool refreshRegistryIfNeeded(bool* wasDirty = nullptr, bool* wasReleased = nullptr);

  /**
   * Internal: Set up fallback fonts in renderer if SD fonts unavailable.
   * Ensures text rendering doesn't completely fail if SD card is missing.
   */
  void setupUiFallbacks(GfxRenderer& renderer);

  /* Registry of available fonts discovered on SD card */
  SdCardFontRegistry registry_;

  /* Manager for loading and caching font data */
  SdCardFontManager manager_;

  /* Flag (set by web server) indicating registry needs re-discovery */
  std::atomic<bool> registryDirty_{false};

  /* Flag indicating fonts were released for network operation */
  std::atomic<bool> registryReleasedForNetwork_{false};

  /* Flag indicating fonts have been successfully loaded */
  std::atomic<bool> registryLoaded_{false};
};